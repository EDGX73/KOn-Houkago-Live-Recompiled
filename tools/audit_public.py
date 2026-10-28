"""Publication audit: nothing of the game, nothing personal, no secrets.

Runs before anything leaves this PC (tools/export_public.py, the git hooks of
tools/hooks/) and on release packages. Exit code 0 = clean, 1 = findings,
2 = usage error. Warnings are reported but do not fail.

  python tools/audit_public.py --tree DIR             every file under DIR
  python tools/audit_public.py --git-staged REPO      the index of REPO (pre-commit)
  python tools/audit_public.py --git-commits REPO A..B  every blob and the metadata
                                                      of every commit in A..B (pre-push;
                                                      a lone B means all of its history)
  python tools/audit_public.py --release PATH         release folder or .zip (zips inside
                                                      zips are opened too)
  --json FILE   also write the findings as JSON

What is checked (rules are listed in RULES below):
  * game material: file names and extensions of the disc, its formats by their
    magic bytes (ISO, CPK, encrypted PRX, PBP, SFO, PSMF, GIM, MIPS ELF), the
    SHA-256 of every file of both discs (read from analysis/iso_*.json when this
    private inventory exists) and of the decrypted executable, translated game
    code (unit symbols, guest-address labels) and large embedded byte tables;
  * personal data: the Windows user name, the git identity of this PC, the
    folder names above the project, any C:\\Users\\<name> path, and the extra
    words of config/audit_private_words.txt (never published);
  * secrets: tokens and private keys;
  * git metadata (--git-commits): author and committer must use a no-reply
    address (GitHub's or Claude's) and no message may carry personal data.
Accepted exceptions live in tools/audit_allowlist.json (path + rule + reason).
"""
import argparse, fnmatch, functools, hashlib, io, json, os, re, subprocess, sys, zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
PELF_SHA256 = 'eda890135ea4aee1a8d0a82b2f00a7159060c58498a6705df7518b36e46b85b1'
# Addresses that identify nobody: GitHub no-reply addresses (also GitHub's own,
# as committer of merges made on the web) and the one of Claude's commits.
NOREPLY = re.compile(r'^(?:(?:[0-9]+\+)?[A-Za-z0-9-]+@users\.noreply\.github\.com|noreply@github\.com|noreply@anthropic\.com)$')

RULES = {
    'game-name': 'name of a disc file, save or game-data folder',
    'game-extension': 'extension of disc images, game archives, saves or game formats',
    'game-magic': 'game format recognised by its magic bytes',
    'game-hash': 'identical to a file of the original discs',
    'game-code': 'translated game code (recompiled unit or guest-address labels)',
    'binary-in-source': 'binary file in a source tree',
    'large-file': 'file larger than the source-tree limit',
    'embedded-data': 'large embedded byte table',
    'personal-data': 'personal data of this PC or its owner',
    'personal-path': 'path inside a Windows user profile',
    'secret': 'token or private key',
    'git-identity': 'commit with a real e-mail address (use the GitHub no-reply address)',
    'unexpected-binary': 'binary outside the known parts of the release package',
    'large-binary': 'binary larger than its part of the package allows (compiled game code?)',
}
# Release package: every program or library must be one of these parts
# (pattern on its path inside the package, size limit or None). The game built
# by the developer weighs ~38 MB; the launcher ~1 MB.
RELEASE_BINARIES = [('*K-On Houkago Live Recompiled.exe', 8 << 20), ('*recomp/psp_recomp.exe', 8 << 20),
                    ('*recomp/lib/lib*.a', 16 << 20), ('*recomp/app.res', 1 << 20), ('*recomp/zig/*', None),
                    ('*recomp/python/*', None), ('*third_party/ffmpeg/bin/*.dll', None), ('*tools/pspdecrypt.exe', 8 << 20),
                    # Public Android app (updater, saves, PSPRecomp core; no game data/code).
                    ('*android.apk', 32 << 20), ('*android.apk!/lib/*/lib*.so', 16 << 20)]
BINARY_SUFFIXES = {'.exe', '.dll', '.a', '.lib', '.so', '.o', '.obj', '.pyd', '.res', '.apk', '.aab'}
GAME_NAMES = ['*.iso', '*.cso', '*.pbp', 'eboot.bin', 'boot.bin', 'param.sfo', 'std.cpk', 'snd.cpk', '*.cpk',
              'pkon.elf', 'libkon_game.so', 'game.kon', '*.prx', '*.konpc', 'savedata', 'generated_unit_*.cpp',
              'generated_registry.cpp', 'generated_units.hpp', 'auto_codegen_report.json', '*.kondraw']
GAME_DIRS = ['roms psp', 'userdata', 'analysis', 'work', 'recompiled_corrected', 'recomp_cache', 'savedata',
             'psp_game', 'ms0', 'texdump']
GAME_EXTENSIONS = {'.iso', '.cso', '.pbp', '.cpk', '.prx', '.konpc', '.at3', '.pmf', '.gim', '.sfo', '.fmdx'}
SOURCE_BINARY_OK = {'.png', '.ico'}
SOURCE_SIZE_LIMIT = 1 << 20
SECRETS = [(re.compile(p), name) for p, name in [
    (rb'gh[pousr]_[A-Za-z0-9]{36,}', 'GitHub token'), (rb'github_pat_[A-Za-z0-9_]{60,}', 'GitHub token'),
    (rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA |PGP |ENCRYPTED )?PRIVATE KEY(?: BLOCK)?-----\r?\n(?:[A-Za-z-]+: [^\n]*\n|\r?\n)*[A-Za-z0-9+/=]{40}', 'private key'),
    (rb'AKIA[0-9A-Z]{16}', 'AWS key'), (rb'xox[abpors]-[A-Za-z0-9-]{10,}', 'Slack token'),
    (rb'AIza[0-9A-Za-z_-]{35}', 'Google API key'),
    (rb'[MNO][A-Za-z0-9_-]{23,25}\.[A-Za-z0-9_-]{6}\.[A-Za-z0-9_-]{27,}', 'Discord bot token')]]
USER_PATH = re.compile(rb'(?i)(?:[a-z]:[\\/]{1,2}|/[a-z]/|/mnt/[a-z]/)users[\\/]{1,2}(?!public\b|default\b|all users\b|<|%|\$|\.\.\.)[^\\/\s"\'<>]{2,}')
UNIT_SYMBOL = re.compile(rb'recomp_unit_[0-9]{4}|register_generated_unit_[0-9]')
GUEST_LABEL = re.compile(rb'\bL_08[89A][0-9A-F]{5}\b')
HEX_BYTE = re.compile(rb'0x[0-9A-Fa-f]{2}\s*,')

def _git(repo, *args, data=None):
    return subprocess.run(['git', '-C', str(repo), *args], input=data, capture_output=True, check=True).stdout

def private_words():
    """Words that identify the owner of this PC; computed here, never written to the repository.
    The PC-derived ones (user and folder names, identities of the history) only
    come from the private repository (the one with the local analysis/ inventory):
    in a public clone or on a CI runner they would only be generic names."""
    words = {w for w in os.environ.get('KON_AUDIT_EXTRA_WORDS', '').split(';') if w}
    extra = ROOT / 'config' / 'audit_private_words.txt'
    if extra.is_file():
        words |= {l.strip() for l in extra.read_text(encoding='utf-8').splitlines() if l.strip() and not l.startswith('#')}
    if not (ROOT / 'analysis').is_dir():
        return sorted(w for w in words if len(w) >= 4)
    words.add(Path.home().name)
    words |= {p.name for p in ROOT.parents if p.name and p != Path(ROOT.anchor) and p.parent != Path(ROOT.anchor)}
    words |= _identities()
    # The public identity and generic profile names are not personal.
    words -= {'EDGX73', 'Users', 'Documents', 'Desktop', 'Public', 'home'}
    return sorted(w for w in words if len(w) >= 4)

@functools.lru_cache(maxsize=None)
def _identities():
    """Names and e-mail addresses of this PC's git configuration and of the private history."""
    values, words = [], set()
    for args in (['config', '--global', 'user.name'], ['config', '--global', 'user.email'], ['config', 'user.name'], ['config', 'user.email'],
                 ['log', '--all', '--format=%an%n%ae%n%cn%n%ce']):
        try:
            values += _git(ROOT, *args).decode('utf-8', 'replace').splitlines()
        except subprocess.CalledProcessError:
            continue
    for value in {v.strip() for v in values if v.strip()}:
        if not NOREPLY.match(value) and value not in ('Claude', 'EDGX73', 'GitHub'):
            words |= {value, value.split('@')[0]}
    return frozenset(words)

def word_pattern(words):
    if not words:
        return None
    alternatives = b'|'.join(re.escape(w.encode('utf-8')) for w in sorted(words, key=len, reverse=True))
    return re.compile(rb'(?i)(?<![A-Za-z0-9])(?:' + alternatives + rb')(?![A-Za-z0-9])')

def known_game_hashes():
    hashes = {PELF_SHA256: 'decrypted executable pkon.elf'}
    for inventory in sorted((ROOT / 'analysis').glob('iso_*.json')):
        try:
            data = json.loads(inventory.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        if isinstance(data, dict) and 'files' in data:
            hashes[data.get('sha256', '')] = f'disc image {data.get("label", inventory.stem)}'
            for f in data['files']:
                if f.get('sha256') and f.get('size', 0) > 0:
                    hashes[f['sha256']] = f'{data.get("label", "")}:{f["path"]}'
    hashes.pop('', None)
    return hashes

class Audit:
    def __init__(self, mode):
        self.mode = mode  # 'source' (repository content) or 'release' (built package)
        self.findings, self.warnings, self.allowed = [], [], []
        self.words = private_words()
        self.word_re = word_pattern(self.words)
        self.hashes = known_game_hashes()
        allow = ROOT / 'tools' / 'audit_allowlist.json'
        self.allowlist = json.loads(allow.read_text(encoding='utf-8'))['allow'] if allow.is_file() else []
        self.files = 0
        self.where = ''  # shown after the path: the commit of a blob

    def report(self, path, rule, detail, warning=False):
        shown = path + self.where
        for entry in self.allowlist:
            if entry['rule'] == rule and fnmatch.fnmatch(path, entry['path']):
                self.allowed.append((shown, rule, detail, entry['reason']))
                return
        (self.warnings if warning else self.findings).append((shown, rule, detail))

    def check_name(self, path):
        parts = [p.lower() for p in PurePosixPath(path).parts]
        name = parts[-1]
        if any(fnmatch.fnmatch(name, pattern) for pattern in GAME_NAMES):
            self.report(path, 'game-name', name)
        if any(p in GAME_DIRS for p in parts[:-1]):
            self.report(path, 'game-name', 'inside a game-data or private folder')
        suffix = PurePosixPath(name).suffix
        if suffix in GAME_EXTENSIONS:
            self.report(path, 'game-extension', suffix)

    def check_bytes(self, path, data):
        self.files += 1
        if self.word_re:
            hit = self.word_re.search(data)
            if hit:
                self.report(path, 'personal-data', f'"{_mask(hit.group().decode("utf-8", "replace"))}" at byte {hit.start()}')
        hit = USER_PATH.search(data)
        if hit:
            self.report(path, 'personal-path', _mask(hit.group().decode('utf-8', 'replace')))
        for pattern, name in SECRETS:
            if pattern.search(data):
                self.report(path, 'secret', name)
        digest = hashlib.sha256(data).hexdigest()
        if digest in self.hashes:
            self.report(path, 'game-hash', self.hashes[digest])
        head = data[:0x8010]
        magic = None
        if head[:4] == b'CPK ': magic = 'CPK archive'
        elif head[0x8001:0x8006] == b'CD001': magic = 'ISO 9660 image'
        elif head[:4] == b'~PSP': magic = 'encrypted PSP executable'
        elif head[:4] == b'\0PBP': magic = 'PSP PBP package'
        elif head[:4] == b'\0PSF': magic = 'PSP SFO'
        elif head[:4] == b'PSMF': magic = 'PSP movie'
        elif head[:11] == b'MIG.00.1PSP': magic = 'PSP GIM texture'
        elif head[:4] == b'\x7fELF' and len(head) > 19 and head[18] == 8: magic = 'MIPS ELF (PSP executable)'
        if magic:
            self.report(path, 'game-magic', magic)
        if UNIT_SYMBOL.search(data):
            self.report(path, 'game-code', 'symbol of a translated game unit')
        labels = len(GUEST_LABEL.findall(data))
        if labels:
            self.report(path, 'game-code', f'{labels} guest-address labels', warning=labels < 8)
        if self.mode == 'release':
            suffix = PurePosixPath(path).suffix.lower()
            if data[:2] == b'MZ' or data[:8] == b'!<arch>\n' or data[:4] == b'\x7fELF' or suffix in BINARY_SUFFIXES:
                part = next((limit for pattern, limit in RELEASE_BINARIES if fnmatch.fnmatch(path, pattern)), False)
                if part is False:
                    self.report(path, 'unexpected-binary', f'{len(data)} bytes')
                elif part is not None and len(data) > part:
                    self.report(path, 'large-binary', f'{len(data)} bytes, limit {part}')
        if self.mode == 'source':
            suffix = PurePosixPath(path).suffix.lower()
            if b'\0' in data[:8192] and suffix not in SOURCE_BINARY_OK:
                self.report(path, 'binary-in-source', f'{len(data)} bytes')
            if len(data) > SOURCE_SIZE_LIMIT:
                self.report(path, 'large-file', f'{len(data)} bytes')
            tables = len(HEX_BYTE.findall(data))
            if tables > 4096:
                self.report(path, 'embedded-data', f'{tables} byte literals')

    def check(self, path, data, where=''):
        self.where = where
        self.check_name(path)
        self.check_bytes(path, data)
        if self.mode == 'release' and zipfile.is_zipfile(io.BytesIO(data)) and path.lower().endswith(('.zip', '.apk')):
            self.check_zip(path, data)

    def check_zip(self, label, data):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            for info in z.infolist():
                if not info.is_dir():
                    self.check(f'{label}!/{info.filename}', z.read(info), self.where)

    def check_commit_metadata(self, repo, rev_range):
        log = _git(repo, 'log', '--format=%H%x00%an%x00%ae%x00%cn%x00%ce%x00%B%x01', rev_range).decode('utf-8', 'replace')
        for record in filter(str.strip, log.split('\x01')):
            sha, an, ae, cn, ce, body = record.strip('\n').split('\x00', 5)
            self.where = ' (metadata)'
            for who, email in {(an, ae), (cn, ce)}:
                if not NOREPLY.match(email):
                    self.report(f'commit {sha[:10]}', 'git-identity', f'{_mask(who)} <{_mask(email)}>')
            self.check_bytes(f'commit {sha[:10]}', '\n'.join((an, ae, cn, ce, body)).encode('utf-8'))
            self.files -= 1

def _mask(text):
    return text if len(text) <= 4 else text[:2] + '*' * (len(text) - 3) + text[-1]

def blobs_of(repo, entries):
    """entries: list of (path, blob id). Yields (path, bytes) reading each blob once."""
    ids = sorted({b for _, b in entries})
    out = _git(repo, 'cat-file', '--batch', data=''.join(i + '\n' for i in ids).encode())
    content, pos = {}, 0
    for i in ids:
        header_end = out.index(b'\n', pos)
        size = int(out[pos:header_end].split()[2])
        content[i] = out[header_end + 1:header_end + 1 + size]
        pos = header_end + 1 + size + 1
    for path, b in entries:
        yield path, content[b]

def tree_entries(repo, rev):
    entries = []
    for line in _git(repo, 'ls-tree', '-r', '-z', rev).split(b'\0'):
        if line:
            meta, path = line.split(b'\t', 1)
            mode, kind, blob = meta.split()
            if kind == b'blob':
                entries.append((path.decode('utf-8'), blob.decode()))
    return entries

def run(args):
    audit = Audit('release' if args.release else 'source')
    if args.tree:
        base = Path(args.tree)
        for f in sorted(base.rglob('*')):
            if f.is_file() and '.git' not in f.relative_to(base).parts:
                audit.check(f.relative_to(base).as_posix(), f.read_bytes())
    elif args.git_staged:
        entries = []
        for line in _git(args.git_staged, 'ls-files', '-s', '-z').split(b'\0'):
            if line:
                meta, path = line.split(b'\t', 1)
                entries.append((path.decode('utf-8'), meta.split()[1].decode()))
        for path, data in blobs_of(args.git_staged, entries):
            audit.check(path, data)
    elif args.git_commits:
        repo, rev_range = args.git_commits
        seen = set()
        for sha in _git(repo, 'rev-list', rev_range).decode().split():
            fresh = [(p, b) for p, b in tree_entries(repo, sha) if (p, b) not in seen]
            seen |= set(fresh)
            for path, data in blobs_of(repo, fresh):
                audit.check(path, data, f' @{sha[:10]}')
        audit.check_commit_metadata(repo, rev_range)
    elif args.release:
        target = Path(args.release)
        files = [target] if target.is_file() else sorted(p for p in target.rglob('*') if p.is_file())
        for f in files:
            audit.check(f.name if f == target else f.relative_to(target).as_posix(), f.read_bytes())
    return audit

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument('--tree')
    g.add_argument('--git-staged')
    g.add_argument('--git-commits', nargs=2, metavar=('REPO', 'RANGE'))
    g.add_argument('--release')
    p.add_argument('--json')
    p.add_argument('--quiet', action='store_true', help='print only problems')
    args = p.parse_args()
    try:
        audit = run(args)
    except (OSError, subprocess.CalledProcessError, ValueError) as e:
        print(f'audit: cannot read the target: {e}', file=sys.stderr)
        return 2
    inventory = f'{len(audit.hashes) - 1} disc files' if len(audit.hashes) > 1 else 'not available on this machine'
    if not args.quiet or audit.findings or audit.warnings:
        print(f'Publication audit: {audit.files} files; game inventory: {inventory}; '
              f'{len(audit.words)} private words; {len(audit.allowed)} accepted exceptions.')
    for path, rule, detail in audit.findings:
        print(f'  BLOCKED  {path}: {RULES[rule]} ({detail})')
    for path, rule, detail in audit.warnings:
        print(f'  warning  {path}: {RULES[rule]} ({detail})')
    if args.json:
        Path(args.json).write_text(json.dumps({
            'files': audit.files, 'game_inventory': inventory,
            'findings': [dict(path=a, rule=b, detail=c) for a, b, c in audit.findings],
            'warnings': [dict(path=a, rule=b, detail=c) for a, b, c in audit.warnings],
            'accepted': [dict(path=a, rule=b, detail=c, reason=d) for a, b, c, d in audit.allowed]}, indent=2), encoding='utf-8')
    if audit.findings:
        print('Publication audit FAILED: nothing may be published until these are removed.')
        return 1
    if not args.quiet:
        print('Publication audit passed.')
    return 0

if __name__ == '__main__':
    sys.exit(main())
