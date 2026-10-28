"""Portability probe: compiles the port's code for another platform with zig,
without linking or running it, and reports what is portable today
(docs/ANDROID.md; backlog INF-1, AND-3).

  python tools/portability_probe.py                     aarch64-linux-gnu, all groups
  python tools/portability_probe.py --target x86_64-linux-gnu --groups core tests
  python tools/portability_probe.py --json work/arm64_probe/report.json

Groups:
  core     third_party/PSPRecomp/src/*.cpp (tools/psprecomp_patch.py fetch first)
  corpus   work/recompiled_corrected/generated_*.cpp: the game's translated code,
           only on a PC with the installed disc (-O2, as the real build)
  runtime  the sources of the port's libraries (kon_host, kon_native_media,
           kon_native_vulkan in CMakeLists.txt) (-O0)
  tests    src/*_test.cpp (-O0)
Objects go to work/portability/<target>/ and are never published.
Exit code 1 if something that compiled before regressed (see --expect).
"""
import argparse, json, os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ZIG_DIR = ROOT / 'third_party/toolchain/zig-x86_64-windows-0.15.2'
INCLUDES = ['src', 'third_party/PSPRecomp/include', 'work/import_fixture/generated', 'build/native_full/native_shaders',
            'third_party/vulkan_headers/Vulkan-Headers-1.3.290/include', 'work/recompiled_corrected', 'third_party/ffmpeg/include']
RUNTIME = ['boot_probe.cpp', 'native_network.cpp', 'native_adhoc.cpp', 'native_online.cpp', 'native_controls.cpp',
           'host_headphones.cpp', 'native_media.cpp', 'native_vulkan_clear.cpp', 'native_vulkan_present.cpp']

def zig():
    found = [ZIG_DIR / 'zig.exe', ZIG_DIR / 'zig']
    for candidate in found:
        if candidate.is_file():
            return str(candidate)
    return 'zig'  # a zig 0.15.2 on PATH (CI)

def compile_one(source, target, out_dir, optimize):
    obj = out_dir / (source.stem + '.o')
    args = [zig(), 'c++', '-target', target, '-std=c++20', optimize, '-g0', '-fno-sanitize=undefined', '-c', str(source), '-o', str(obj)]
    args += [f'-I{ROOT / i}' for i in INCLUDES]
    result = subprocess.run(args, capture_output=True, text=True, cwd=ROOT)
    first_error = next((l.strip() for l in result.stderr.splitlines() if 'error' in l), '')
    first_error = first_error.replace('\\', '/').replace(ROOT.as_posix() + '/', '')  # no PC paths in reports
    return source.relative_to(ROOT).as_posix(), result.returncode == 0, first_error[:200]

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--target', default='aarch64-linux-gnu')
    p.add_argument('--groups', nargs='+', default=['core', 'corpus', 'runtime', 'tests'], choices=['core', 'corpus', 'runtime', 'tests'])
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    p.add_argument('--json', type=Path)
    p.add_argument('--expect', type=Path, help='previous --json report: fail if a file that compiled now fails')
    a = p.parse_args()
    os.environ.setdefault('ZIG_LOCAL_CACHE_DIR', str(ROOT / f'work/zig-cache-{a.target}'))
    os.environ.setdefault('ZIG_GLOBAL_CACHE_DIR', str(ROOT / 'work/zig-global-cache'))
    groups = {
        'core': (sorted((ROOT / 'third_party/PSPRecomp/src').glob('*.cpp')), '-O2'),
        'corpus': (sorted((ROOT / 'work/recompiled_corrected').glob('generated_*.cpp')), '-O2'),
        'runtime': ([ROOT / 'src' / name for name in RUNTIME if (ROOT / 'src' / name).is_file()], '-O0'),
        'tests': (sorted((ROOT / 'src').glob('*_test.cpp')), '-O0'),
    }
    report = {'target': a.target, 'groups': {}}
    for name in a.groups:
        sources, optimize = groups[name]
        if not sources:
            print(f'{name}: nothing to compile here (skipped)')
            continue
        out_dir = ROOT / 'work/portability' / a.target / name
        out_dir.mkdir(parents=True, exist_ok=True)
        start = time.monotonic()
        with ThreadPoolExecutor(a.jobs) as pool:
            results = list(pool.map(lambda s: compile_one(s, a.target, out_dir, optimize), sources))
        seconds = round(time.monotonic() - start, 1)
        ok = [r[0] for r in results if r[1]]
        failed = {r[0]: r[2] for r in results if not r[1]}
        report['groups'][name] = {'ok': ok, 'failed': failed, 'seconds': seconds}
        print(f'{name}: {len(ok)}/{len(sources)} compile for {a.target} ({seconds} s)')
        for path, error in failed.items():
            print(f'  not yet  {path}: {error}')
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if a.expect and a.expect.is_file():
        before = json.loads(a.expect.read_text(encoding='utf-8'))
        regressed = [path for name, g in before['groups'].items() for path in g['ok']
                     if path in report['groups'].get(name, {}).get('failed', {})]
        if regressed:
            print('REGRESSION: these compiled before and fail now:', *regressed, sep='\n  ')
            return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
