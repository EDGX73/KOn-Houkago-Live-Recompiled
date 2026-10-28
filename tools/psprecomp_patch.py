"""The port's changes to PSPRecomp as a reproducible patch.

third_party/ is not part of the repository: PSPRecomp (MIT, by its author at
github.com/jessicanataliagta/PSPRecomp) is fetched at a pinned revision and the
port's changes are applied from patches/psprecomp/kon-port.patch. The five
tools/*framework*/fix_*/add_* scripts run by tools/build_framework.ps1 are
idempotent and find their changes already applied.

  python tools/psprecomp_patch.py fetch   clone the pinned revision into
                                          third_party/PSPRecomp and apply the patch
                                          (nothing happens if it is already there)
  python tools/psprecomp_patch.py write   regenerate the patch from the changes in
                                          third_party/PSPRecomp (after editing it)
  python tools/psprecomp_patch.py check   fail if the patch no longer matches those
                                          changes (run by tools/export_public.py)
"""
import shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = 'https://github.com/jessicanataliagta/PSPRecomp.git'
REVISION = 'f6e7d415c7f447b934cc3865a31eb725f353d659'
SOURCE = ROOT / 'third_party' / 'PSPRecomp'
PATCHES = ROOT / 'patches' / 'psprecomp'
PATCH = PATCHES / 'kon-port.patch'
SPARSE = ['src', 'include', 'tools', 'tests', 'configs', 'docs']  # plus the root files (cone mode)

def git(*args, cwd=SOURCE, check=True, data=None):
    return subprocess.run(['git', '-c', 'core.autocrlf=false', *args], cwd=cwd, input=data, capture_output=True, check=check)

def patch_bytes():
    # LF whatever the checkout did to the .patch file (core.autocrlf on Windows).
    return PATCH.read_bytes().replace(b'\r\n', b'\n')

def local_diff():
    if git('rev-parse', 'HEAD').stdout.decode().strip() != REVISION:
        raise SystemExit(f'{SOURCE} is not at the pinned revision {REVISION}')
    # LF on both sides: the working files may mix line endings after the patch scripts.
    return git('-c', 'core.autocrlf=true', 'diff', '--no-color', '--full-index').stdout.replace(b'\r\n', b'\n')

def write():
    PATCHES.mkdir(parents=True, exist_ok=True)
    PATCH.write_bytes(local_diff())
    shutil.copyfile(SOURCE / 'LICENSE', PATCHES / 'PSPRecomp-LICENSE.txt')
    files = git('diff', '--name-only').stdout.decode().split()
    print(f'Wrote {PATCH.relative_to(ROOT)} ({len(files)} files)')

def check():
    if not PATCH.is_file():
        raise SystemExit(f'Missing {PATCH.relative_to(ROOT)}: run "python tools/psprecomp_patch.py write"')
    if not (SOURCE / '.git').exists():
        print('No local PSPRecomp checkout: nothing to compare (the patch is what gets published).')
        return
    if local_diff() != patch_bytes():
        raise SystemExit('patches/psprecomp/kon-port.patch is out of date with third_party/PSPRecomp: '
                         'run "python tools/psprecomp_patch.py write" and commit it')
    print('PSPRecomp patch matches third_party/PSPRecomp.')

def fetch():
    if (SOURCE / '.git').exists():
        if git('apply', '--reverse', '--check', '-', check=False, data=patch_bytes()).returncode == 0:
            print(f'{SOURCE.relative_to(ROOT)} already has the patch applied.')
            return
        raise SystemExit(f'{SOURCE} exists without the patch; it is left untouched (move it away to fetch again)')
    SOURCE.parent.mkdir(parents=True, exist_ok=True)
    git('clone', '--quiet', '--filter=blob:none', '--no-checkout', UPSTREAM, str(SOURCE), cwd=ROOT)
    # Sparse, like the original checkout: upstream also carries profiles/ with another
    # game's data and generated code, which this project never downloads.
    git('sparse-checkout', 'set', '--cone', *SPARSE)
    git('checkout', '--quiet', REVISION)
    if (SOURCE / 'profiles').exists():
        raise SystemExit('PSPRecomp checkout contains profiles/: the sparse checkout did not apply')
    git('apply', '--whitespace=nowarn', '-', data=patch_bytes())
    changed = git('diff', '--name-only').stdout.decode().split()
    print(f'PSPRecomp {REVISION[:7]} fetched into {SOURCE.relative_to(ROOT)} with the port patch ({len(changed)} files).')

if __name__ == '__main__':
    commands = {'fetch': fetch, 'write': write, 'check': check}
    if len(sys.argv) != 2 or sys.argv[1] not in commands:
        raise SystemExit(__doc__)
    commands[sys.argv[1]]()
