#!/usr/bin/env python3
"""Build the private Android game pack (game.kon) from local inputs.

The pack contains the game module compiled from the player's own ISO
(``libkon_game.so``) and the decrypted executable (``pkon.elf``). It is imported
into the public APK on the phone; it is never published and never part of the
APK.

Usage:
  python tools/make_android_game_pack.py --abi arm64-v8a
  python tools/make_android_game_pack.py --abi x86_64 --skip-build
  python tools/make_android_game_pack.py --all
"""
from __future__ import annotations

import argparse
import hashlib
import sys
import zipfile
from pathlib import Path

import make_android_apk as base

ROOT = base.ROOT
OUT = ROOT / "dist" / "android-pack"
ELF = ROOT / "analysis" / "executables" / "pkon.elf"

ABI_DIRS = {"arm64-v8a": "android_arm64", "x86_64": "android_x86_64"}


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--abi", choices=sorted(ABI_DIRS), help="target ABI of the phone")
    parser.add_argument("--all", action="store_true", help="build one pack per ABI")
    parser.add_argument("--skip-build", action="store_true",
                        help="reuse build/android_<abi>/libkon_game.so")
    parser.add_argument("--elf", default=str(ELF))
    parser.add_argument("--out", default=str(OUT))
    return parser.parse_args(argv)


def build_module(abi: str, skip: bool) -> Path:
    build_dir = ROOT / "build" / ABI_DIRS[abi]
    module = build_dir / "libkon_game.so"
    if not skip:
        base.run([
            str(ROOT / "third_party" / "python_build_deps" / "cmake" / "data" / "bin" / "cmake.exe"),
            "--build", str(build_dir), "--target", "kon_android_game_module", "--parallel", "8",
        ])
    if not module.is_file():
        raise base.BuildError(f"missing module {module}; configure and build {ABI_DIRS[abi]} first")
    return module


def make_pack(abi: str, module: Path, elf: Path, out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"game-{abi}.kon"
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(module, "libkon_game.so", compress_type=zipfile.ZIP_DEFLATED)
        archive.write(elf, "pkon.elf", compress_type=zipfile.ZIP_DEFLATED)
    return target


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not args.all and not args.abi:
        print("error: indica --abi arm64-v8a (móvil) o --all", file=sys.stderr)
        return 2
    abis = sorted(ABI_DIRS) if args.all else [args.abi]
    elf = Path(args.elf)
    if not elf.is_file():
        print(f"error: falta {elf}", file=sys.stderr)
        return 2
    out = Path(args.out)
    try:
        for abi in abis:
            module = build_module(abi, args.skip_build)
            pack = make_pack(abi, module, elf, out)
            print(f"{abi}: {pack}")
            print(f"  module {module.stat().st_size} bytes, elf {elf.stat().st_size} bytes")
            print(f"  pack   {pack.stat().st_size} bytes")
            print(f"  sha256 {sha256(pack)}")
    except base.BuildError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print("")
    print("Copia el .kon al teléfono e impórtalo con «Importar juego».")
    print("El móvil usa arm64-v8a; el emulador x86_64 usa el otro paquete.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
