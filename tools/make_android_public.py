#!/usr/bin/env python3
"""Package the public Android APK (runtime only, no game code).

The APK contains the portable runtime, the PSPRecomp probe, the updater/saves
UI and the licence notices. It contains no ISO, EBOOT, CPK, generated corpus,
pkon.elf or signing key. The game module is built by the player on their own PC
and imported as game.kon.
"""
from __future__ import annotations

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

import make_android_apk as base

ROOT = base.ROOT
BUILD = ROOT / "build" / "android_public"
OUT = ROOT / "dist" / "android-public"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--abis", default="arm64-v8a,x86_64")
    parser.add_argument("--version-code", type=int, default=200)
    parser.add_argument("--out", default=str(OUT))
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--toolchain", help="bundle with lib/<abi>/, toolchain_id.txt, sysroot/ and headers")
    parser.add_argument("--icon", help="square PNG used as launcher icon (default assets/branding/icon_512.png)")
    parser.add_argument("--legal-dir", help="extra licence/NOTICE tree copied to assets/licenses/")
    parser.add_argument("--notice", help="master NOTICE copied to assets/NOTICE.txt")
    parser.add_argument("--source-archive", action="append", default=[],
                        help="copyleft source archive copied to assets/toolchain/SOURCES/")
    parser.add_argument("--source-dir", help="copyleft source tree copied to assets/toolchain/SOURCES/")
    parser.add_argument("--sdk")
    parser.add_argument("--build-tools")
    parser.add_argument("--ndk")
    parser.add_argument("--java-home")
    return parser.parse_args(argv)


def build_runtime(abis: tuple[str, ...], skip: bool, ndk: Path | None = None) -> dict[str, Path]:
    libraries: dict[str, Path] = {}
    for abi in abis:
        build_dir = ROOT / "build" / ("android_arm64" if abi == "arm64-v8a" else "android_x86_64")
        runtime = build_dir / "libkon_runtime.so"
        if not skip:
            base.run([
                str(ROOT / "third_party" / "python_build_deps" / "cmake" / "data" / "bin" / "cmake.exe"),
                "--build", str(build_dir), "--target", "kon_android_runtime", "--parallel", "8",
            ])
        if not runtime.is_file():
            raise base.BuildError(f"missing runtime {runtime}")
        base.sanitize_native_library(runtime, ndk)
        libraries[abi] = runtime
    return libraries


def probes(abis: tuple[str, ...]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for abi in abis:
        path = ROOT / "build" / "android_apk" / "native" / abi / "libkon_probe.so"
        if path.is_file():
            result[abi] = path
    return result


NATIVE_EXECUTABLES = ("libclang.so", "libld.lld.so", "libkon_psprecomp.so", "libkon_pspdecrypt.so")


def toolchain_files(toolchain: Path, abis: tuple[str, ...]) -> tuple[str, dict[str, Path]]:
    """Only the executables go to lib/<abi>/.

    Android does not extract names without .so (libz.so.1, libxml2.so.16, ...);
    those libraries travel as assets under assets/toolchain/lib/<abi>/ and are
    found through LD_LIBRARY_PATH after extraction.
    """
    identifier = ""
    id_file = toolchain / "toolchain_id.txt"
    if id_file.is_file():
        identifier = id_file.read_text(encoding="utf-8").strip()
    if not identifier:
        raise base.BuildError(f"missing toolchain_id.txt in {toolchain}")
    files: dict[str, Path] = {}
    for abi in abis:
        directory = toolchain / "lib" / abi
        if not directory.is_dir():
            raise base.BuildError(f"missing {directory}")
        for name in NATIVE_EXECUTABLES:
            source = directory / name
            if not source.is_file():
                raise base.BuildError(f"missing {source}")
            files[f"lib/{abi}/{name}"] = source
    return identifier, files


def stage_toolchain_assets(toolchain: Path, assets: Path) -> None:
    """Full runtime toolchain extracted to files/toolchain on first use."""
    target = assets / "toolchain"
    target.mkdir(parents=True, exist_ok=True)
    clang = toolchain / "include" / "clang"
    if clang.is_dir():
        shutil.copytree(clang, target / "clang", dirs_exist_ok=True)
    direct = toolchain / "clang"
    if direct.is_dir():
        shutil.copytree(direct, target / "clang", dirs_exist_ok=True)
    for name in ("sysroot", "psprecomp"):
        source = toolchain / name
        if source.is_dir():
            shutil.copytree(source, target / name, dirs_exist_ok=True)
    for abi_dir in (toolchain / "lib").glob("*"):
        if not abi_dir.is_dir():
            continue
        destination = target / "lib" / abi_dir.name
        destination.mkdir(parents=True, exist_ok=True)
        for library in abi_dir.iterdir():
            if library.is_file() and library.name not in NATIVE_EXECUTABLES:
                shutil.copy2(library, destination / library.name)


def package_apk(base_apk: Path, dex: Path, runtime: dict[str, Path], probe: dict[str, Path],
                extras: dict[str, Path], output: Path) -> None:
    shutil.copy2(base_apk, output)
    with zipfile.ZipFile(output, "a", zipfile.ZIP_DEFLATED) as archive:
        archive.write(dex, "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        for abi, library in runtime.items():
            archive.write(library, f"lib/{abi}/libkon_runtime.so", compress_type=zipfile.ZIP_STORED)
        for abi, library in probe.items():
            archive.write(library, f"lib/{abi}/libkon_probe.so", compress_type=zipfile.ZIP_STORED)
        for name, library in extras.items():
            # Deflated: the installer extracts compressed native entries, while
            # STORED ones are left as-is (and non-.so names are then missing).
            archive.write(library, name, compress_type=zipfile.ZIP_DEFLATED)


def build(args: argparse.Namespace) -> int:
    version_name, _, _, _ = base.read_version_header()
    version_code = args.version_code
    abis = tuple(part.strip() for part in args.abis.split(",") if part.strip())
    for abi in abis:
        if abi not in base.ABI_TRIPLES:
            raise base.BuildError(f"unsupported ABI {abi!r}")
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

    runtime = build_runtime(abis, args.skip_build, ndk)
    probe = probes(abis)
    for library in probe.values():
        base.sanitize_native_library(library, ndk)
    toolchain_id = ""
    extras: dict[str, Path] = {}
    if args.toolchain:
        toolchain_id, extras = toolchain_files(Path(args.toolchain), abis)
    icon_source = Path(args.icon) if args.icon else None
    if icon_source is not None and not icon_source.is_file():
        raise base.BuildError(f"missing icon: {icon_source}")
    icon, icon_origin = base.icon_bytes(icon_source)
    print(f"version {version_name} (versionCode {version_code}), package {base.PACKAGE}, ABI {', '.join(abis)}")
    print(f"launcher icon: {icon_origin}")
    if toolchain_id:
        print(f"toolchain:    {toolchain_id} ({len(extras)} executables)")
    if BUILD.exists():
        shutil.rmtree(BUILD)
    BUILD.mkdir(parents=True)
    resources = base.stage_resources(BUILD, icon)
    manifest = base.stage_manifest(BUILD, version_name, version_code)
    legal = Path(args.legal_dir) if args.legal_dir else None
    notice = Path(args.notice) if args.notice else None
    licenses = base.stage_licenses(BUILD, legal, notice)
    if toolchain_id:
        target = licenses / "toolchain"
        target.mkdir(parents=True, exist_ok=True)
        (target / "toolchain_id.txt").write_text(toolchain_id + "\n", encoding="utf-8")
        stage_toolchain_assets(Path(args.toolchain), licenses)
    if args.source_archive or args.source_dir:
        sources = licenses / "toolchain" / "SOURCES"
        sources.mkdir(parents=True, exist_ok=True)
        for path_text in args.source_archive:
            path = Path(path_text)
            if not path.is_file():
                raise base.BuildError(f"missing source archive: {path}")
            shutil.copy2(path, sources / path.name)
        if args.source_dir:
            source_dir = Path(args.source_dir)
            if not source_dir.is_dir():
                raise base.BuildError(f"missing source directory: {source_dir}")
            shutil.copytree(source_dir, sources, dirs_exist_ok=True)
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
    package_apk(base_apk, dex, runtime, probe, extras, unsigned)
    aligned = BUILD / "aligned.apk"
    base.run([zipalign, "-p", "-f", "4", unsigned, aligned])
    password_file = base.ensure_keystore(keytool, None)
    signed = BUILD / f"KOn-Houkago-Live-Recompiled-{version_name}-android.apk"
    signature, schemes = base.sign_apk(java_exe, apksigner_jar, aligned, signed, password_file)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    output = out / signed.name
    shutil.copy2(signed, output)
    print("")
    print(f"Public APK: {output}")
    print(f"size:       {output.stat().st_size} bytes")
    print(f"sha256:     {base.sha256_file(output)}")
    print(f"signature:  {signature} ({', '.join(schemes)})")
    return 0


def main(argv: list[str]) -> int:
    try:
        return build(parse_args(argv))
    except base.BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
