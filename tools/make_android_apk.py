#!/usr/bin/env python3
"""Builds the public Android APK of K-On! Houkago Live!! Recompiled without Gradle.

The package contains only the Java UI and ``libkon_probe.so`` (the MIT
PSPRecomp decoder/runtime). It never packages the ISO, std.cpk, snd.cpk, EBOOT,
the generated corpus (work/recompiled_corrected) or player saves. The script
audits the finished APK and fails if any of those appear.

Toolchain: the already-installed Android SDK/NDK and a JDK. The signing key is
created on first use at config/android-test.keystore; that file and its password
are local only (both are in .gitignore) and must never be published.

Outputs:
  dist/android/KOn-Houkago-Live-Recompiled-<version>-android.apk
  dist/android/manifest-android.json

Usage (PowerShell):
  python tools\\make_android_apk.py

The versionName is read from src/kon_version.h; versionCode defaults to 200.
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import os
import platform
import re
import secrets
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "android" / "app"
PACKAGE = "com.edgx73.konrecompiled"
EXPECTED_REPO = "EDGX73/KOn-Houkago-Live-Recompiled"
DEFAULT_VERSION_CODE = 200
MIN_SDK = 26
TARGET_SDK = 36
DEFAULT_ABIS = ("arm64-v8a", "x86_64")
ABI_TRIPLES = {
    "arm64-v8a": "aarch64-linux-android",
    "x86_64": "x86_64-linux-android",
}


def sanitize_native_library(path: Path, ndk: Path | None = None) -> None:
    """Strip debug info and neutralise this PC's profile path in a library.

    FFmpeg keeps its build path (``C:/Users/<name>/...``) inside error strings,
    which the publication audit rejects as personal data. The replacement keeps
    every byte count so no string offset moves.
    """
    if ndk is not None:
        strip = ndk / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "llvm-strip.exe"
        if strip.is_file():
            run([str(strip), "--strip-debug", str(path)], capture=True)
    user = os.environ.get("USERNAME") or Path.home().name
    if not user or len(user) < 2:
        return
    neutral = ("x" * len(user))
    pairs = [
        (("C:/" + "Users/" + user).encode(), f"C:/build/{neutral}".encode()),
        (("/c/" + "Users/" + user).encode(), f"/c/build/{neutral}".encode()),
        (("C:" + chr(92) + "Users" + chr(92) + user).encode(), f"C:\\build\\{neutral}".encode()),
        (f"Users/{user}".encode(), f"build/{neutral}".encode()),
        (user.encode(), neutral.encode()),
        (user.upper().encode(), neutral.upper().encode()),
    ]
    data = path.read_bytes()
    for old, new in pairs:
        if len(old) == len(new):
            data = data.replace(old, new)
    path.write_bytes(data)
PSPRECOMP = ROOT / "third_party" / "PSPRecomp"
CORE_SOURCES = (
    "decoder",
    "deflate",
    "elf32",
    "guest_memory",
    "nid_registry",
    "program_analysis",
    "runtime",
    "sha256",
)
KEYSTORE = ROOT / "config" / "android-test.keystore"
KEYSTORE_PASSWORD = ROOT / "config" / "android-test.keystore.pass"
KEY_ALIAS = "kon-test"
HOST_TAG = {
    "Windows": "windows-x86_64",
    "Linux": "linux-x86_64",
    "Darwin": "darwin-x86_64",
}.get(platform.system())
FORBIDDEN_APK_PARTS = (
    ".iso",
    ".cpk",
    ".elf",
    ".konpc",
    "eboot.bin",
    "generated_",
    "guest_corpus",
    "pkon",
    "savedata",
)


class BuildError(RuntimeError):
    pass


def run(cmd, *, cwd=None, capture=False, secret=False):
    printable = " ".join(str(part) for part in cmd)
    print(("> " + printable) if not secret else "> (command with local key material)", flush=True)
    process = subprocess.run(
        [str(part) for part in cmd],
        cwd=str(cwd or ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if process.stdout and not capture:
        print(process.stdout, end="")
    if process.stderr and not capture:
        print(process.stderr, end="", file=sys.stderr)
    if process.returncode != 0:
        detail = (process.stdout or "") + (process.stderr or "")
        tail = "\n".join(detail.strip().splitlines()[-25:])
        raise BuildError(f"command failed ({process.returncode}): {printable}\n{tail}")
    return process


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_version_header() -> tuple[str, int, int, int, str]:
    text = (ROOT / "src" / "kon_version.h").read_text(encoding="utf-8")

    def number(name: str) -> int:
        match = re.search(rf"#define\s+{name}\s+(\d+)", text)
        if not match:
            raise BuildError(f"src/kon_version.h does not define {name}")
        return int(match.group(1))

    major = number("KON_VERSION_MAJOR")
    minor = number("KON_VERSION_MINOR")
    patch = number("KON_VERSION_PATCH")
    name_match = re.search(r'#define\s+KON_VERSION_STRING\s+"([^"]+)"', text)
    if not name_match:
        raise BuildError("src/kon_version.h does not define KON_VERSION_STRING")
    version = name_match.group(1)
    if version != f"{major}.{minor}.{patch}":
        raise BuildError(
            f"KON_VERSION_STRING ({version}) does not match "
            f"{major}.{minor}.{patch} in src/kon_version.h"
        )
    repo_match = re.search(r'#define\s+KON_GITHUB_REPO\s+"([^"]*)"', text)
    if not repo_match:
        raise BuildError("src/kon_version.h does not define KON_GITHUB_REPO")
    if repo_match.group(1) != EXPECTED_REPO:
        raise BuildError(
            "KON_GITHUB_REPO in src/kon_version.h and GithubReleases.API differ "
            f"({repo_match.group(1)!r} != {EXPECTED_REPO!r})"
        )
    return version, major, minor, patch


def find_java_home(argument: str | None) -> Path:
    candidates: list[Path] = []
    if argument:
        candidates.append(Path(argument))
    if os.environ.get("JAVA_HOME"):
        candidates.append(Path(os.environ["JAVA_HOME"]))
    java_root = ROOT / "third_party" / "java"
    if java_root.is_dir():
        candidates.extend(sorted(java_root.glob("jdk-*"), reverse=True))
    for candidate in candidates:
        if (candidate / "bin" / "java.exe").is_file() and (candidate / "bin" / "keytool.exe").is_file():
            return candidate
    raise BuildError("no JDK found; pass --java-home or set JAVA_HOME")


def find_sdk(argument: str | None) -> Path:
    candidates: list[Path] = []
    if argument:
        candidates.append(Path(argument))
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(variable):
            candidates.append(Path(os.environ[variable]))
    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates.append(Path(local) / "Programs" / "android-sdk")
    candidates.append(Path.home() / "AppData" / "Local" / "Programs" / "android-sdk")
    for candidate in candidates:
        if (candidate / "platforms" / f"android-{TARGET_SDK}" / "android.jar").is_file():
            return candidate
    raise BuildError(
        "Android SDK not found (android-36 platform); pass --sdk or set ANDROID_HOME"
    )


def find_build_tools(sdk: Path, argument: str | None) -> Path:
    candidates: list[Path] = []
    if argument:
        candidates.append(Path(argument))
    build_tools = sdk / "build-tools"
    if build_tools.is_dir():
        candidates.extend(
            sorted(
                (entry for entry in build_tools.iterdir() if entry.is_dir()),
                key=lambda entry: [int(part) for part in re.findall(r"\d+", entry.name)],
                reverse=True,
            )
        )
    for candidate in candidates:
        required = (
            candidate / "aapt2.exe",
            candidate / "zipalign.exe",
            candidate / "lib" / "apksigner.jar",
            candidate / "lib" / "d8.jar",
        )
        if all(path.is_file() for path in required):
            return candidate
    raise BuildError("Android build-tools (aapt2, zipalign, apksigner, d8) not found")


def find_ndk(argument: str | None, sdk: Path) -> Path:
    candidates: list[Path] = []
    if argument:
        candidates.append(Path(argument))
    for variable in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if os.environ.get(variable):
            candidates.append(Path(os.environ[variable]))
    ndk_root = sdk / "ndk"
    if ndk_root.is_dir():
        candidates.extend(
            sorted(
                (entry for entry in ndk_root.iterdir() if entry.is_dir()),
                key=lambda entry: [int(part) for part in re.findall(r"\d+", entry.name)],
                reverse=True,
            )
        )
    for candidate in candidates:
        if (candidate / "toolchains" / "llvm" / "prebuilt" / HOST_TAG / "bin" / "clang++.exe").is_file():
            return candidate
    raise BuildError("Android NDK not found; pass --ndk or set ANDROID_NDK_HOME")


def png_size(data: bytes) -> tuple[int, int] | None:
    if not data.startswith(b"\x89PNG\r\n\x1a\n") or data[12:16] != b"IHDR":
        return None
    width, height = struct.unpack(">II", data[16:24])
    return int(width), int(height)


def fallback_icon() -> bytes:
    """A valid 512x512 PNG with a white K on the theme pink (no Pillow needed)."""
    size = 512
    background = (194, 24, 91)
    foreground = (255, 255, 255)
    pixels = [[background] * size for _ in range(size)]

    def rectangle(x0: int, y0: int, x1: int, y1: int) -> None:
        for y in range(max(0, y0), min(size, y1)):
            row = pixels[y]
            for x in range(max(0, x0), min(size, x1)):
                row[x] = foreground

    rectangle(142, 120, 210, 392)
    for step in range(124):
        rectangle(226 + step, 252 - step, 226 + step + 36, 252 - step + 36)
        rectangle(226 + step, 260 + step, 226 + step + 36, 260 + step + 36)
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for red, green, blue in row:
            raw.extend((red, green, blue))
    compressed = zlib.compress(bytes(raw), 9)

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", compressed)
        + chunk(b"IEND", b"")
    )


def icon_bytes(source: Path | None = None) -> tuple[bytes, str]:
    if source is None:
        source = ROOT / "assets" / "branding" / "icon_512.png"
    if source.is_file():
        data = source.read_bytes()
        dimensions = png_size(data)
        if dimensions is not None and dimensions[0] == dimensions[1] and dimensions[0] >= 192:
            origin = f"{source} ({dimensions[0]}x{dimensions[1]})"
            try:
                origin = f"{source.relative_to(ROOT)} ({dimensions[0]}x{dimensions[1]})"
            except ValueError:
                pass
            return data, origin
        print(f"warning: {source} is not a valid square PNG; using the generated icon", file=sys.stderr)
    else:
        print(f"warning: {source} is missing; using the generated icon", file=sys.stderr)
    return fallback_icon(), "generated fallback icon"


def stage_resources(build: Path, icon: bytes) -> Path:
    target = build / "res"
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(APP / "res", target)
    mipmap = target / "mipmap-xxxhdpi"
    mipmap.mkdir(parents=True, exist_ok=True)
    (mipmap / "ic_launcher.png").write_bytes(icon)
    return target


def stage_manifest(build: Path, version_name: str, version_code: int) -> Path:
    template = (APP / "AndroidManifest.xml").read_text(encoding="utf-8")
    manifest = template.replace("@KON_VERSION_NAME@", version_name).replace(
        "@KON_VERSION_CODE@", str(version_code)
    )
    if "@KON_" in manifest:
        raise BuildError("AndroidManifest.xml still has unreplaced @KON_ placeholders")
    target = build / "AndroidManifest.xml"
    target.write_text(manifest, encoding="utf-8")
    return target


def stage_licenses(build: Path, extra: Path | None = None, notice: Path | None = None) -> Path:
    """Packages the notices of the code that ships in the APK.

    ``extra`` is copied recursively into ``assets/licenses/`` (LLVM, NDK,
    iconv, FFmpeg, GPL source offer...). ``notice`` is copied verbatim to
    ``assets/NOTICE.txt``.
    """
    assets = build / "assets"
    if assets.exists():
        shutil.rmtree(assets)
    licenses = assets / "licenses"
    licenses.mkdir(parents=True)
    copies = (
        (ROOT / "LICENSE", "KOn-Houkago-Live-Recompiled-MIT.txt"),
        (PSPRECOMP / "LICENSE", "PSPRecomp-MIT.txt"),
    )
    for source, name in copies:
        if not source.is_file():
            raise BuildError(f"missing license text: {source}")
        shutil.copy2(source, licenses / name)
    if extra is not None:
        if not extra.is_dir():
            raise BuildError(f"missing legal directory: {extra}")
        for entry in sorted(extra.iterdir()):
            if entry.is_symlink():
                raise BuildError(f"symlink not allowed in legal assets: {entry}")
            target = licenses / entry.name
            if entry.is_dir():
                shutil.copytree(entry, target)
            elif entry.is_file():
                shutil.copy2(entry, target)
            else:
                raise BuildError(f"unsupported legal asset: {entry}")
    if notice is not None:
        if not notice.is_file():
            raise BuildError(f"missing NOTICE: {notice}")
        shutil.copy2(notice, assets / "NOTICE.txt")
    return assets


def build_native(
    build: Path, ndk: Path, abis: tuple[str, ...], skip: bool
) -> dict[str, dict]:
    prebuilt = ndk / "toolchains" / "llvm" / "prebuilt" / HOST_TAG / "bin"
    clang = prebuilt / "clang++.exe"
    strip = prebuilt / "llvm-strip.exe"
    readelf = prebuilt / "llvm-readelf.exe"
    if not clang.is_file():
        raise BuildError(f"NDK clang++ not found at {clang}")
    sources = [PSPRECOMP / "src" / f"{name}.cpp" for name in CORE_SOURCES]
    sources.append(APP / "jni" / "kon_probe.cpp")
    for source in sources:
        if not source.is_file():
            raise BuildError(f"missing native source: {source}")

    version_script = build / "kon_probe.map"
    version_script.write_text(
        "{\n  global:\n    Java_*;\n  local:\n    *;\n};\n", encoding="utf-8"
    )

    libraries: dict[str, dict] = {}
    for abi in abis:
        if abi not in ABI_TRIPLES:
            raise BuildError(f"unsupported ABI: {abi}")
        output_dir = build / "native" / abi
        output_dir.mkdir(parents=True, exist_ok=True)
        output = output_dir / "libkon_probe.so"
        if skip and output.is_file():
            print(f"reusing {output}")
        else:
            command = [
                clang,
                f"--target={ABI_TRIPLES[abi]}{MIN_SDK}",
                "-std=c++20",
                "-O2",
                "-fPIC",
                "-shared",
                "-fvisibility=hidden",
                "-fstack-protector-strong",
                "-I",
                PSPRECOMP / "include",
                *sources,
                "-static-libstdc++",
                "-Wl,--build-id=sha1",
                "-Wl,-z,relro",
                "-Wl,-z,now",
                "-Wl,--gc-sections",
                f"-Wl,--version-script={version_script}",
                "-o",
                output,
            ]
            run(command)
            run([strip, "--strip-unneeded", output])

        header = run([readelf, "-h", output], capture=True).stdout
        machine = "AArch64" if abi == "arm64-v8a" else "X86-64"
        if machine not in header:
            raise BuildError(f"{output} is not a {abi} ELF ({machine} missing)")
        dynamic = run([readelf, "-d", output], capture=True).stdout
        if "libc++_shared" in dynamic:
            raise BuildError(f"{output} still depends on libc++_shared.so")
        if "libkon_guest_corpus" in dynamic:
            raise BuildError(f"{output} links the generated corpus")
        libraries[abi] = {
            "path": output,
            "size": output.stat().st_size,
            "sha256": sha256_file(output),
        }
        print(f"{abi}: {output.stat().st_size} bytes, sha256 {libraries[abi]['sha256']}")
    return libraries


def compile_java(
    javac_exe: Path, android_jar: Path, build: Path
) -> tuple[Path, Path]:
    generated = build / "gen"
    r_java = generated / "com" / "edgx73" / "konrecompiled" / "R.java"
    if not r_java.is_file():
        raise BuildError(f"aapt2 did not generate {r_java}")
    classes = build / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)

    sources = sorted(APP.glob("java/**/*.java"))
    if not sources:
        raise BuildError("no Java sources found in android/app/java")
    sources.append(r_java)
    run(
        [
            javac_exe,
            "-encoding",
            "UTF-8",
            "--release",
            "17",
            "-classpath",
            android_jar,
            "-d",
            classes,
            *sources,
        ],
        capture=True,
    )

    classes_jar = build / "classes.jar"
    with zipfile.ZipFile(classes_jar, "w", zipfile.ZIP_DEFLATED) as archive:
        for item in sorted(classes.rglob("*.class")):
            archive.write(item, item.relative_to(classes).as_posix())
    return classes_jar, classes


def make_dex(java_exe: Path, d8_jar: Path, android_jar: Path, build: Path, classes_jar: Path) -> Path:
    dex_dir = build / "dex"
    if dex_dir.exists():
        shutil.rmtree(dex_dir)
    dex_dir.mkdir(parents=True)
    run(
        [
            java_exe,
            "-cp",
            d8_jar,
            "com.android.tools.r8.D8",
            "--release",
            "--min-api",
            str(MIN_SDK),
            "--lib",
            android_jar,
            "--output",
            dex_dir,
            classes_jar,
        ],
        capture=True,
    )
    dex = dex_dir / "classes.dex"
    if not dex.is_file():
        raise BuildError("d8 did not produce classes.dex")
    return dex


def package_apk(base_apk: Path, dex: Path, libraries: dict[str, dict], build: Path) -> Path:
    output = build / "unsigned.apk"
    shutil.copy2(base_apk, output)
    with zipfile.ZipFile(output, "a", zipfile.ZIP_DEFLATED) as archive:
        archive.write(dex, "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        for abi, library in libraries.items():
            archive.write(
                library["path"],
                f"lib/{abi}/libkon_probe.so",
                compress_type=zipfile.ZIP_STORED,
            )
    return output


def ensure_keystore(keytool: Path, create_password: str | None) -> Path:
    if KEYSTORE_PASSWORD.is_file():
        password = KEYSTORE_PASSWORD.read_text(encoding="utf-8").strip()
        if not password:
            raise BuildError(f"{KEYSTORE_PASSWORD} is empty")
    elif create_password:
        password = create_password
        KEYSTORE_PASSWORD.write_text(password, encoding="utf-8")
    elif not KEYSTORE.is_file():
        password = secrets.token_urlsafe(30)
        KEYSTORE_PASSWORD.parent.mkdir(parents=True, exist_ok=True)
        KEYSTORE_PASSWORD.write_text(password, encoding="utf-8")
    else:
        raise BuildError(
            f"{KEYSTORE} exists but {KEYSTORE_PASSWORD} is missing; pass --store-password"
        )

    if not KEYSTORE.is_file():
        KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
        run(
            [
                keytool,
                "-genkeypair",
                "-keystore",
                KEYSTORE,
                "-storetype",
                "PKCS12",
                "-storepass:file",
                KEYSTORE_PASSWORD,
                "-keypass:file",
                KEYSTORE_PASSWORD,
                "-alias",
                KEY_ALIAS,
                "-keyalg",
                "RSA",
                "-keysize",
                "4096",
                "-validity",
                "10000",
                "-dname",
                "CN=K-On Houkago Live Recompiled (local test), OU=Android, O=Local, C=ES",
            ],
            secret=True,
        )
        print(f"created local signing key {KEYSTORE.relative_to(ROOT)} (never publish it)")
    return KEYSTORE_PASSWORD


def sign_apk(
    java_exe: Path, apksigner_jar: Path, aligned: Path, output: Path, password_file: Path
) -> tuple[str, list[str]]:
    run(
        [
            java_exe,
            "-jar",
            apksigner_jar,
            "sign",
            "--ks",
            KEYSTORE,
            "--ks-key-alias",
            KEY_ALIAS,
            "--ks-pass",
            f"file:{password_file}",
            "--v1-signing-enabled",
            "true",
            "--v2-signing-enabled",
            "true",
            "--v3-signing-enabled",
            "true",
            "--min-sdk-version",
            str(MIN_SDK),
            "--out",
            output,
            aligned,
        ],
        secret=True,
    )
    verification = run(
        [java_exe, "-jar", apksigner_jar, "verify", "--verbose", "--print-certs", output],
        capture=True,
    )
    text = verification.stdout + verification.stderr
    match = re.search(r"certificate SHA-256 digest:\s*([0-9a-fA-F]+)", text)
    if not match:
        raise BuildError("apksigner did not report the signing certificate digest")
    if "Verified using v2 scheme (APK Signature Scheme v2): true" not in text:
        raise BuildError("apksigner did not verify the v2 signature")
    schemes = [
        name
        for name, needle in (
            ("v1", "Verified using v1 scheme (JAR signing): true"),
            ("v2", "Verified using v2 scheme (APK Signature Scheme v2): true"),
            ("v3", "Verified using v3 scheme (APK Signature Scheme v3): true"),
        )
        if needle in text
    ]
    return match.group(1).lower(), schemes


def verify_badging(aapt2: Path, apk: Path, version_name: str, version_code: int, abis: tuple[str, ...]) -> None:
    output = run([aapt2, "dump", "badging", apk], capture=True).stdout
    package = re.search(r"package: name='([^']+)' versionCode='(\d+)' versionName='([^']*)'", output)
    if not package:
        raise BuildError("aapt2 dump badging did not report the package identity")
    if package.group(1) != PACKAGE:
        raise BuildError(f"applicationId {package.group(1)!r} != {PACKAGE!r}")
    if int(package.group(2)) != version_code:
        raise BuildError(f"versionCode {package.group(2)} != {version_code}")
    if package.group(3) != version_name:
        raise BuildError(f"versionName {package.group(3)!r} != {version_name!r}")
    sdk = re.search(r"minSdkVersion:'(\d+)'", output)
    target = re.search(r"targetSdkVersion:'(\d+)'", output)
    if not sdk or int(sdk.group(1)) != MIN_SDK:
        raise BuildError(f"minSdk is not {MIN_SDK}")
    if not target or int(target.group(1)) != TARGET_SDK:
        raise BuildError(f"targetSdk is not {TARGET_SDK}")
    for abi in abis:
        if abi not in output:
            raise BuildError(f"aapt2 badging does not list the {abi} native code")


def audit_apk(apk: Path, abis: tuple[str, ...]) -> list[dict]:
    entries: list[dict] = []
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        for required in ("AndroidManifest.xml", "classes.dex", "resources.arsc"):
            if required not in names:
                raise BuildError(f"APK is missing {required}")
        for info in archive.infolist():
            name = info.filename
            lower = name.lower()
            for forbidden in FORBIDDEN_APK_PARTS:
                if forbidden in lower:
                    raise BuildError(f"APK contains forbidden game data entry: {name}")
            if info.file_size > 32 * 1024 * 1024:
                raise BuildError(f"APK entry is unexpectedly large: {name}")
            if not (
                name in ("AndroidManifest.xml", "classes.dex", "resources.arsc")
                or name.startswith(("res/", "lib/", "META-INF/", "assets/licenses/"))
            ):
                raise BuildError(f"APK contains an unexpected entry: {name}")
            if name.startswith("lib/"):
                parts = name.split("/")
                if len(parts) != 3 or parts[0] != "lib" or parts[1] not in abis or parts[2] != "libkon_probe.so":
                    raise BuildError(f"unexpected native library entry: {name}")
            entries.append({"name": name, "size": info.file_size, "sha256": None})
        for abi in abis:
            if f"lib/{abi}/libkon_probe.so" not in names:
                raise BuildError(f"APK is missing lib/{abi}/libkon_probe.so")
        if "assets/licenses/PSPRecomp-MIT.txt" not in names:
            raise BuildError("APK is missing the PSPRecomp MIT notice")
        if archive.getinfo("classes.dex").file_size < 1024:
            raise BuildError("classes.dex looks empty")
        for entry in entries:
            if entry["name"].endswith(".so"):
                with archive.open(entry["name"]) as handle:
                    digest = hashlib.sha256()
                    for block in iter(lambda: handle.read(1 << 20), b""):
                        digest.update(block)
                    entry["sha256"] = digest.hexdigest()
    return entries


def write_manifest(
    output_dir: Path,
    apk: Path,
    version_name: str,
    version_code: int,
    abis: tuple[str, ...],
    signature: str,
    schemes: list[str],
    libraries: dict[str, dict],
) -> Path:
    manifest = {
        "schema": 1,
        "applicationId": PACKAGE,
        "versionCode": version_code,
        "versionName": version_name,
        "minSdk": MIN_SDK,
        "targetSdk": TARGET_SDK,
        "abi": list(abis),
        "apk": apk.name,
        "size": apk.stat().st_size,
        "sha256": sha256_file(apk),
        "signature": signature,
        "signatureSchemes": schemes,
        "nativeLibraries": [
            {
                "abi": abi,
                "name": "libkon_probe.so",
                "size": libraries[abi]["size"],
                "sha256": libraries[abi]["sha256"],
            }
            for abi in abis
        ],
        "licenses": [
            "assets/licenses/KOn-Houkago-Live-Recompiled-MIT.txt",
            "assets/licenses/PSPRecomp-MIT.txt",
        ],
        "containsGameData": False,
        "generator": "tools/make_android_apk.py",
        "generatedAt": _datetime.datetime.now(_datetime.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z"),
    }
    path = output_dir / "manifest-android.json"
    path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return path


def build(args: argparse.Namespace) -> int:
    version_name, _, _, _ = read_version_header()
    version_code = args.version_code if args.version_code is not None else DEFAULT_VERSION_CODE
    if version_code <= 0:
        raise BuildError("--version-code must be positive")
    abis = tuple(part.strip() for part in args.abis.split(",") if part.strip())
    for abi in abis:
        if abi not in ABI_TRIPLES:
            raise BuildError(f"unsupported ABI {abi!r}; known: {', '.join(ABI_TRIPLES)}")

    java_home = find_java_home(args.java_home)
    sdk = find_sdk(args.sdk)
    build_tools = find_build_tools(sdk, args.build_tools)
    ndk = find_ndk(args.ndk, sdk)
    android_jar = sdk / "platforms" / f"android-{TARGET_SDK}" / "android.jar"
    java_exe = java_home / "bin" / "java.exe"
    javac_exe = java_home / "bin" / "javac.exe"
    keytool = java_home / "bin" / "keytool.exe"
    aapt2 = build_tools / "aapt2.exe"
    zipalign = build_tools / "zipalign.exe"
    apksigner_jar = build_tools / "lib" / "apksigner.jar"
    d8_jar = build_tools / "lib" / "d8.jar"
    for tool in (java_exe, javac_exe, keytool, aapt2, zipalign, apksigner_jar, d8_jar, android_jar):
        if not tool.is_file():
            raise BuildError(f"missing build tool: {tool}")

    build_dir = ROOT / "build" / "android_apk"
    if build_dir.exists() and not args.skip_native:
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    print(f"version {version_name} (versionCode {version_code}), package {PACKAGE}, ABI {', '.join(abis)}")
    icon, icon_origin = icon_bytes()
    print(f"launcher icon: {icon_origin}")
    resources = stage_resources(build_dir, icon)
    manifest = stage_manifest(build_dir, version_name, version_code)
    license_assets = stage_licenses(build_dir)

    libraries = build_native(build_dir, ndk, abis, args.skip_native)

    compiled = build_dir / "resources.zip"
    run([aapt2, "compile", "--dir", resources, "-o", compiled])
    base_apk = build_dir / "base.apk"
    run(
        [
            aapt2,
            "link",
            "-o",
            base_apk,
            "-I",
            android_jar,
            "--manifest",
            manifest,
            "-R",
            compiled,
            "-A",
            license_assets,
            "--java",
            build_dir / "gen",
            "--auto-add-overlay",
        ]
    )

    classes_jar, _ = compile_java(javac_exe, android_jar, build_dir)
    dex = make_dex(java_exe, d8_jar, android_jar, build_dir, classes_jar)
    unsigned = package_apk(base_apk, dex, libraries, build_dir)

    aligned = build_dir / "aligned.apk"
    run([zipalign, "-p", "-f", "4", unsigned, aligned])

    password_file = ensure_keystore(keytool, args.store_password)
    signed = build_dir / f"KOn-Houkago-Live-Recompiled-{version_name}-android.apk"
    signature, schemes = sign_apk(java_exe, apksigner_jar, aligned, signed, password_file)

    verify_badging(aapt2, signed, version_name, version_code, abis)
    entries = audit_apk(signed, abis)

    output_dir = Path(args.out).resolve() if args.out else ROOT / "dist" / "android"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_apk = output_dir / signed.name
    shutil.copy2(signed, output_apk)
    manifest_path = write_manifest(
        output_dir, output_apk, version_name, version_code, abis, signature, schemes, libraries
    )

    size = output_apk.stat().st_size
    digest = sha256_file(output_apk)
    print("")
    print(f"APK:      {output_apk}")
    print(f"size:     {size} bytes ({size / 1048576:.2f} MiB)")
    print(f"sha256:   {digest}")
    print(f"signature: {signature} ({', '.join(schemes)})")
    print(f"manifest: {manifest_path}")
    print(f"entries:  {len(entries)}")
    for entry in entries:
        print(f"  {entry['size']:>9}  {entry['name']}")
    if args.print_json:
        print(json.dumps({"apk": str(output_apk), "size": size, "sha256": digest,
                          "signature": signature, "manifest": str(manifest_path)},
                         indent=2, ensure_ascii=False))
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version-code", type=int, default=DEFAULT_VERSION_CODE,
                        help=f"Android versionCode (default {DEFAULT_VERSION_CODE})")
    parser.add_argument("--abis", default=",".join(DEFAULT_ABIS),
                        help="comma-separated ABIs (default arm64-v8a,x86_64)")
    parser.add_argument("--sdk", help="Android SDK root (default ANDROID_HOME)")
    parser.add_argument("--build-tools", help="build-tools directory (default newest with all tools)")
    parser.add_argument("--ndk", help="Android NDK root (default ANDROID_NDK_HOME)")
    parser.add_argument("--java-home", help="JDK with java/javac/keytool (default JAVA_HOME)")
    parser.add_argument("--out", help="output directory (default dist/android)")
    parser.add_argument("--store-password", help="keystore password when creating the local test key")
    parser.add_argument("--skip-native", action="store_true",
                        help="reuse build/android_apk/native without recompiling")
    parser.add_argument("--print-json", action="store_true", help="print the result as JSON")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    try:
        return build(parse_args(argv))
    except BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
