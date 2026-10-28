"""Updates the public GitHub copy (dist/github/KOn-Houkago-Live-Recompiled,
pushed to EDGX73/KOn-Houkago-Live-Recompiled) with the committed tree, as one
new commit by the GitHub no-reply address. The private history, the internal
notes and the e-mail address never leave this PC. This script never pushes.

  python tools/export_public.py --dry-run           audit and list the changes, commit nothing
  python tools/export_public.py "<commit message>"  commit the export in the public copy
  --offline       do not fetch GitHub first (the check of step 2 is skipped)
  --allow-dirty   export HEAD even if the private worktree has uncommitted changes

Steps (any failure stops the export before the public copy is touched):
 1. The export is HEAD of this repository: uncommitted changes are refused, and
    patches/psprecomp/kon-port.patch must match third_party/PSPRecomp.
 2. The public copy must contain everything already on GitHub: if origin/main
    has commits that main lacks (a merged pull request, an edit on the web),
    bring them into this repository first (docs/PUBLICACION.md).
 3. HEAD minus the private files, plus public_overlay/ at the root, is written to
    a staging folder and audited (tools/audit_public.py).
 4. The public worktree is replaced by that tree, every file is staged (also the
    ones a .gitignore would hide), the index is compared with the expected list
    and the commit is made; the public pre-commit hook audits it once more.
Then: review it (git -C dist/github/KOn-Houkago-Live-Recompiled show --stat),
push from that folder (the pre-push hook audits every pushed commit) and
publish the release with the three files of dist/release (tools/make_release.py).
"""
import argparse, datetime, fnmatch, io, json, shutil, subprocess, sys, tarfile, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'dist/github/KOn-Houkago-Live-Recompiled'
OVERLAY = 'public_overlay/'
# Internal notes, local shortcuts and what is published in another form.
EXCLUDE = ['AGENTS.md', 'CLAUDE.md', 'CONTEXTO_PARA_CLAUDE.md', 'PROMPT_PARA_CLAUDE.txt', 'ROADMAP.txt',
           'docs/GIT_CHANGES.md', 'docs/INFORME_PROGRESO_2026-10-05.md', 'Probar *.cmd',
           'third_party/*']  # PSPRecomp is published as patches/psprecomp/kon-port.patch
AUTHOR = ['-c', 'user.name=EDGX73', '-c', 'user.email=92487113+EDGX73@users.noreply.github.com']

def git(repo, *args, check=True, data=None):
    return subprocess.run(['git', '-C', str(repo), *args], input=data, capture_output=True, check=check)

def stop(message):
    raise SystemExit(f'Export stopped: {message}')

def preflight(args):
    dirty = [l for l in git(ROOT, 'status', '--porcelain').stdout.decode('utf-8', 'replace').splitlines() if not l.startswith('??')]
    if dirty and not args.allow_dirty:
        stop(f'{len(dirty)} uncommitted changes in this repository (the export is HEAD); commit them first')
    if subprocess.run([sys.executable, str(ROOT / 'tools/psprecomp_patch.py'), 'check']).returncode:
        stop('the PSPRecomp patch is out of date')
    if not (OUT / '.git').is_dir():
        return
    if args.offline:
        print('Offline: GitHub was not checked for newer commits.')
        return
    if git(OUT, 'fetch', '--quiet', 'origin', check=False).returncode:
        stop('cannot reach GitHub to check for newer commits (use --offline to skip this check)')
    behind = int(git(OUT, 'rev-list', '--count', 'main..origin/main').stdout or b'0')
    if behind:
        stop(f'GitHub has {behind} commit(s) that the public copy lacks; bring them into this repository first (docs/PUBLICACION.md)')

def exported_tree():
    """{path: bytes} of HEAD as published."""
    files = {}
    data = git(ROOT, 'archive', '--format=tar', 'HEAD').stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for m in tar.getmembers():
            if not m.isfile() or any(fnmatch.fnmatch(m.name, p) for p in EXCLUDE):
                continue
            content = tar.extractfile(m).read()
            if m.name.startswith(OVERLAY):
                files[m.name[len(OVERLAY):]] = content  # public_overlay/X becomes X (and wins)
            else:
                files.setdefault(m.name, content)
    return files

def audit(folder):
    return subprocess.run([sys.executable, str(ROOT / 'tools/audit_public.py'), '--tree', str(folder)]).returncode == 0

def message_is_clean(message):
    sys.path.insert(0, str(ROOT / 'tools'))
    from audit_public import Audit
    a = Audit('source')
    a.check_bytes('commit message', message.encode('utf-8'))
    for path, rule, detail in a.findings:
        print(f'  BLOCKED  {path}: {detail}')
    return not a.findings

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('message', nargs='?')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--offline', action='store_true')
    p.add_argument('--allow-dirty', action='store_true')
    args = p.parse_args()
    if not args.dry_run and not args.message:
        p.error('a commit message is required (or --dry-run)')
    preflight(args)
    files = exported_tree()
    with tempfile.TemporaryDirectory(prefix='kon_export_') as staging:
        for name, content in files.items():
            target = Path(staging) / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        if not audit(staging):
            stop('the publication audit found problems (listed above); nothing was changed')
    published = set(git(OUT, 'ls-files', '-z').stdout.decode('utf-8').split('\0')) - {''} if (OUT / '.git').is_dir() else set()
    added = sorted(set(files) - published)
    removed = sorted(published - set(files))
    changed = sorted(n for n in set(files) & published if (OUT / n).read_bytes() != files[n])
    print(f'Export of {git(ROOT, "rev-parse", "--short", "HEAD").stdout.decode().strip()}: {len(files)} files; '
          f'{len(added)} new, {len(changed)} changed, {len(removed)} removed.')
    for label, names in (('new', added), ('changed', changed), ('removed', removed)):
        for n in names:
            print(f'  {label:8} {n}')
    if args.dry_run:
        print('Dry run: the public copy was not modified.')
        return
    if not message_is_clean(args.message):
        stop('the commit message contains personal data')
    if not (OUT / '.git').is_dir():
        OUT.mkdir(parents=True, exist_ok=True)
        git(OUT, 'init', '-q', '-b', 'main')
    for name in published:  # start from an empty tree so removed files disappear too
        (OUT / name).unlink(missing_ok=True)
    for name, content in files.items():
        target = OUT / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    git(OUT, 'add', '-A')
    # Force: a .gitignore rule must never silently drop an exported file.
    git(OUT, '--literal-pathspecs', 'add', '-f', '--pathspec-from-file=-', '--pathspec-file-nul', data='\0'.join(sorted(files)).encode('utf-8'))
    staged = set(git(OUT, 'ls-files', '-z').stdout.decode('utf-8').split('\0')) - {''}
    if staged != set(files):
        stop(f'the public index differs from the export ({sorted(staged ^ set(files))[:10]}); '
             'inspect it with git -C dist/github/KOn-Houkago-Live-Recompiled status')
    if not git(OUT, 'status', '--porcelain').stdout:
        print('No changes.')
        return
    result = git(OUT, *AUTHOR, 'commit', '-q', '-m', args.message, check=False)
    sys.stdout.write(result.stdout.decode('utf-8', 'replace')); sys.stderr.write(result.stderr.decode('utf-8', 'replace'))
    if result.returncode:
        stop('the commit was refused (see above); the public worktree has the new files staged')
    head = git(OUT, 'rev-parse', 'HEAD').stdout.decode().strip()
    state = OUT / '.git' / 'kon_export_state.json'  # local only: which private commit each export came from
    history = json.loads(state.read_text(encoding='utf-8')) if state.is_file() else []
    history.append({'public': head, 'private': git(ROOT, 'rev-parse', 'HEAD').stdout.decode().strip(),
                    'date': datetime.datetime.now().astimezone().isoformat(timespec='seconds')})
    state.write_text(json.dumps(history, indent=2), encoding='utf-8')
    print(git(OUT, 'log', '-1', '--format=%h %an <%ae> %s').stdout.decode('utf-8', 'replace'))
    print('Not pushed. Review with: git -C dist/github/KOn-Houkago-Live-Recompiled show --stat')

if __name__ == '__main__':
    main()
