"""Installs the publication controls as git hooks (tools/git_hook.py):

  this repository   pre-push: always refused (it is never pushed)
  public copy       pre-commit and pre-push: publication audit
                    (dist/github/KOn-Houkago-Live-Recompiled)

The hooks call this repository's tools with this Python, by absolute path: the
public copy cannot weaken its own checks. An existing different hook is kept as
<name>.previous. Run again after moving the project folder.

  python tools/install_hooks.py          install or update
  python tools/install_hooks.py --check  report what is installed (exit 1 if missing)
"""
import subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / 'dist/github/KOn-Houkago-Live-Recompiled'
MARK = '# kon-publication-controls'

def hooks_dir(repo):
    path = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--git-path', 'hooks'], capture_output=True, text=True, check=True).stdout.strip()
    return (repo / path) if not Path(path).is_absolute() else Path(path)

def script(role):
    python = Path(sys.executable).as_posix()
    hook = (ROOT / 'tools' / 'git_hook.py').as_posix()
    return f'#!/bin/sh\n{MARK} (tools/install_hooks.py)\nexec "{python}" "{hook}" {role} "$@"\n'

def plan():
    items = [(ROOT, 'pre-push', 'private-pre-push')]
    if (PUBLIC / '.git').is_dir():
        items += [(PUBLIC, 'pre-commit', 'public-pre-commit'), (PUBLIC, 'pre-push', 'public-pre-push')]
    return items

def main():
    check = '--check' in sys.argv
    missing = 0
    for repo, name, role in plan():
        target = hooks_dir(repo) / name
        wanted = script(role)
        current = target.read_text(encoding='utf-8') if target.is_file() else None
        where = 'private repository' if repo == ROOT else 'public copy'
        if current == wanted:
            print(f'ok         {where}: {name}')
            continue
        if check:
            print(f'MISSING    {where}: {name}')
            missing += 1
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if current is not None and MARK not in current:
            target.replace(target.with_name(name + '.previous'))
        target.write_text(wanted, encoding='utf-8', newline='\n')
        print(f'installed  {where}: {name}')
    if not (PUBLIC / '.git').is_dir():
        print('The public copy does not exist yet: run this again after the first tools/export_public.py.')
    return 1 if missing else 0

if __name__ == '__main__':
    sys.exit(main())
