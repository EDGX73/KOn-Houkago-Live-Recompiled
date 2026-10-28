#!/usr/bin/env python3
"""Package a personal playable APK for one device ABI.

This build is for the owner of the disc: it adds the locally compiled
``libkon_game.so`` and the decrypted ``pkon.elf`` as APK assets, so the app
installs them on first launch. The result contains game code and must never be
published. The public APK (tools/make_android_public.py) plus the game.kon pack
(tools/make_android_game_pack.py) achieve the same thing without embedding
anything.

Usage:
  python tools/make_android_playable.py --abi arm64-v8a
  python tools/make_android_playable.py --abi x86_64 --skip-build
"""
from __future__ import annotations

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

import make_android_apk as base

ROOT = base.ROOT
BUILD = ROOT / "build" / "android_playable"
PLAYABLE_OUT = ROOT / "dist" / "android-playable"
ABI_DIRS = {"arm64-v8a": "android_arm64", "x86_64": "android_x86_64"}


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--abi", required=True, choices=sorted(ABI_DIRS))
    parser.add_argument("--version-code", type=int, default=200)
    parser.add_argument("--out", default=str(PLAYABLE_OUT))
    parser.add_argument("--skip-build", action="store_true",
                        help="reuse build/android_<abi>/libkon_*.so")
    parser.add_argument("--skip-probe", action="store_true")
    parser.add_argument("--sdk")
    parser.add_argument("--build-tools")
    parser.add_argument("--ndk")
    parser.add_argument("--java-home")
    return parser.parse_args(argv)


def build_libraries(abi: str, skip: bool) -> tuple[Path, Path]:
    build_dir = ROOT / "build" / ABI_DIRS[abi]
    runtime = build_dir / "libkon_runtime.so"
    module = build_dir / "libkon_game.so"
    if not skip:
        cmake = ROOT / "third_party" / "python_build_deps" / "cmake" / "data" / "bin" / "cmake.exe"
        base.run([str(cmake), "--build", str(build_dir),
                  "--target", "kon_android_runtime", "kon_android_game_module", "--parallel", "8"])
    for path in (runtime, module):
        if not path.is_file():
            raise base.BuildError(f"missing {path}; configure and build {ABI_DIRS[abi]} first")
    return runtime, module


def package(base_apk: Path, dex: Path, abi: str, runtime: Path, module: Path,
            probe: Path | None, elf: Path, output: Path) -> None:
    shutil.copy2(base_apk, output)
    with zipfile.ZipFile(output, "a", zipfile.ZIP_DEFLATED) as archive:
        archive.write(dex, "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        archive.write(runtime, f"lib/{abi}/libkon_runtime.so", compress_type=zipfile.ZIP_STORED)
        if probe is not None:
            archive.write(probe, f"lib/{abi}/libkon_probe.so", compress_type=zipfile.ZIP_STORED)
        archive.write(elf, "assets/pkon.elf", compress_type=zipfile.ZIP_DEFLATED)
        archive.write(module, "assets/libkon_game.so", compress_type=zipfile.ZIP_STORED)


def build(args: argparse.Namespace) -> int:
    version_name, _, _, _ = base.read_version_header()
    version_code = args.version_code
    abi = args.abi
    java_home = base.find_java_home(args.java_home)
    sdk = base.find_sdk(args.sdk)
    build_tools = base.find_build_tools(sdk, args.build_tools)
    ndk = base.find_ndk(args.ndk, sdk)
    android_jar = sdk / "platforms" / f"android-{base.TARGET_SDK}" / "android.jar"
    java_exe = java_home / "bin" / "java.exe"
    javac_exe = java_home / "bin" / "javac.exe"
    keytool = java_home / "bin" / "keytool.exe"
    aapt2 = build_tools / "aapt2.exe"
    zipalign = build_tools / "zipalign.exe"
    apksigner_jar = build_tools / "lib" / "apksigner.jar"
    d8_jar = build_tools / "lib" / "d8.jar"
    for tool in (java_exe, javac_exe, keytool, aapt2, zipalign, apksigner_jar, d8_jar, android_jar):
        if not tool.is_file():
            raise base.BuildError(f"missing build tool: {tool}")

    runtime, module = build_libraries(abi, args.skip_build)
    probe_path = ROOT / "build" / "android_apk" / "native" / abi / "libkon_probe.so"
    probe = probe_path if probe_path.is_file() and not args.skip_probe else None
    elf = ROOT / "analysis" / "executables" / "pkon.elf"
    if not elf.is_file():
        raise base.BuildError("analysis/executables/pkon.elf is missing; install the disc on PC first")
    icon, icon_origin = base.icon_bytes()
    print(f"version {version_name} (versionCode {version_code}), package {base.PACKAGE}, ABI {abi}")
    print(f"launcher icon: {icon_origin}")
    print(f"game ELF: {elf} ({elf.stat().st_size} bytes)")

    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(parents=True)
    resources = base.stage_resources(BUILD, icon)
    manifest = base.stage_manifest(BUILD, version_name, version_code)
    licenses = base.stage_licenses(BUILD)
    compiled = BUILD / "resources.zip"
    base.run([aapt2, "compile", "--dir", resources, "-o", compiled])
    base_apk = BUILD / "base.apk"
    base.run([
        aapt2, "link", "-o", base_apk, "-I", android_jar, "--manifest", manifest,
        "-R", compiled, "-A", licenses, "--java", BUILD / "gen", "--auto-add-overlay",
    ])
    classes_jar, _ = base.compile_java(javac_exe, android_jar, BUILD)
    dex = base.make_dex(java_exe, d8_jar, android_jar, BUILD, classes_jar)
    unsigned = BUILD / "unsigned.apk"
    package(base_apk, dex, abi, runtime, module, probe, elf, unsigned)
    aligned = BUILD / "aligned.apk"
    base.run([zipalign, "-p", "-f", "4", unsigned, aligned])
    password_file = base.ensure_keystore(keytool, None)
    signed = BUILD / f"KOn-Houkago-Live-Recompiled-{version_name}-{abi}-playable.apk"
    signature, schemes = base.sign_apk(java_exe, apksigner_jar, aligned, signed, password_file)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    output = out / signed.name
    shutil.copy2(signed, output)
    print("")
    print(f"Playable APK: {output}")
    print(f"size:        {output.stat().st_size} bytes")
    print(f"sha256:      {base.sha256_file(output)}")
    print(f"signature:   {signature} ({', '.join(schemes)})")
    print("NOT FOR PUBLICATION: this APK contains the user's generated game code and pkon.elf.")
    return 0


def main(argv: list[str]) -> int:
    try:
        return build(parse_args(argv))
    except base.BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
