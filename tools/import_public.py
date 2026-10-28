"""Brings what was merged on GitHub (pull requests of Claude Code Cloud or other
contributors, edits made on the web) into this private repository, so that the
next tools/export_public.py does not undo it.

  python tools/import_public.py            fetch, show and apply the new public commits
  python tools/import_public.py --dry-run  fetch and show them only

1. Fetches origin in the public copy (dist/github/KOn-Houkago-Live-Recompiled) and
   lists the commits of origin/main that its main lacks.
2. Audits those commits (tools/audit_public.py): something that must not be
   public is reported at once, so it can be removed from GitHub too.
3. Applies their combined change to this worktree, mapping the public-only files
   back to public_overlay/ (git apply --3way: the exported files are the same
   blobs as here). Nothing is committed: review, build, test, then commit.
4. Fast-forwards the public copy's main to origin/main, so the next export
   starts from what is on GitHub.
"""
import argparse, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'dist/github/KOn-Houkago-Live-Recompiled'
OVERLAY = 'public_overlay/'

def git(repo, *args, check=True, data=None):
    return subprocess.run(['git', '-C', str(repo), *args], input=data, capture_output=True, check=check)

def map_overlay(patch, overlay):
    """Rewrites the paths of a public diff: files that only the public copy has
    (exported from public_overlay/X as X) go back to public_overlay/X."""
    def private(name):
        return OVERLAY + name if name in overlay else name
    mapped = []
    for line in patch.splitlines(keepends=True):
        if line.startswith('diff --git a/'):
            a, b = line[len('diff --git a/'):].rstrip('\n').split(' b/', 1)
            line = f'diff --git a/{private(a)} b/{private(b)}\n'
        else:
            for prefix in ('--- a/', '+++ b/', 'rename from ', 'rename to ', 'copy from ', 'copy to '):
                if line.startswith(prefix):
                    line = prefix + private(line[len(prefix):].rstrip('\n')) + '\n'
                    break
        mapped.append(line)
    return ''.join(mapped)

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--dry-run', action='store_true')
    args = p.parse_args()
    if not (OUT / '.git').is_dir():
        raise SystemExit('No public copy yet (tools/export_public.py creates it).')
    if git(OUT, 'fetch', '--quiet', 'origin', check=False).returncode:
        raise SystemExit('Cannot reach GitHub.')
    if git(OUT, 'status', '--porcelain').stdout:
        raise SystemExit('The public copy has uncommitted changes; resolve them first.')
    new = git(OUT, 'log', '--reverse', '--format=%h %an: %s', 'main..origin/main').stdout.decode('utf-8', 'replace').splitlines()
    if not new:
        print('Nothing new on GitHub.')
        return 0
    print(f'{len(new)} commit(s) on GitHub that this repository lacks:')
    for line in new:
        print('  ' + line)
    audit = subprocess.run([sys.executable, str(ROOT / 'tools/audit_public.py'), '--git-commits', str(OUT), 'main..origin/main'])
    if audit.returncode:
        print('WARNING: what is listed above is already public on GitHub; remove it there as well.')
    overlay = {n[len(OVERLAY):] for n in git(ROOT, 'ls-files', OVERLAY).stdout.decode('utf-8').splitlines()}
    patch = map_overlay(git(OUT, 'diff', '--full-index', '--binary', 'main', 'origin/main').stdout.decode('utf-8', 'surrogateescape'), overlay)
    print(git(OUT, 'diff', '--stat', 'main', 'origin/main').stdout.decode('utf-8', 'replace'))
    if args.dry_run:
        print('Dry run: nothing applied.')
        return 0
    result = git(ROOT, 'apply', '--3way', '--whitespace=nowarn', check=False, data=patch.encode('utf-8', 'surrogateescape'))
    sys.stdout.write(result.stdout.decode('utf-8', 'replace')); sys.stderr.write(result.stderr.decode('utf-8', 'replace'))
    if result.returncode:
        raise SystemExit('The changes did not apply cleanly: resolve the conflicts shown by git status, then run this again.')
    git(OUT, 'merge', '--ff-only', '--quiet', 'origin/main')
    print('Applied to this worktree (not committed). Review with git diff, build and test, then commit.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
