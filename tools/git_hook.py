"""Git hooks of the publication controls (installed by tools/install_hooks.py).

  private-pre-push   this repository (full history, personal data, internal
                     notes) is never pushed anywhere: always refuses.
  public-pre-commit  public copy: audits the staged tree (tools/audit_public.py).
  public-pre-push    public copy: audits every blob and the author, committer and
                     message of every commit about to be pushed, for every ref.
Bypassing them (git push --no-verify) defeats the controls: don't.
"""
import subprocess, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
AUDIT = [sys.executable, str(TOOLS / 'audit_public.py')]
ZERO = '0' * 40

def private_pre_push():
    print('BLOCKED: this is the private repository of K-On! Houkago Live!! Recompiled (full history,\n'
          'internal notes, personal data). It is never pushed. Publish with tools/export_public.py and\n'
          'push from dist/github/KOn-Houkago-Live-Recompiled.', file=sys.stderr)
    return 1

def public_pre_commit():
    return subprocess.run(AUDIT + ['--git-staged', '.', '--quiet']).returncode

def public_pre_push(lines):
    ranges = []
    for line in lines:
        parts = line.split()
        if len(parts) != 4:
            continue
        local_ref, local_sha, remote_ref, remote_sha = parts
        if local_sha == ZERO:  # deleting a remote ref publishes nothing
            continue
        ranges.append(local_sha if remote_sha == ZERO else f'{remote_sha}..{local_sha}')
    for rev_range in ranges:
        print(f'Publication audit of the commits to push ({rev_range[:21]}):', file=sys.stderr)
        if subprocess.run(AUDIT + ['--git-commits', '.', rev_range]).returncode:
            print('Push refused by the publication audit.', file=sys.stderr)
            return 1
    return 0

def main():
    role = sys.argv[1] if len(sys.argv) > 1 else ''
    if role == 'private-pre-push':
        return private_pre_push()
    if role == 'public-pre-commit':
        return public_pre_commit()
    if role == 'public-pre-push':
        return public_pre_push(sys.stdin.read().splitlines())
    print(__doc__, file=sys.stderr)
    return 2

if __name__ == '__main__':
    sys.exit(main())
