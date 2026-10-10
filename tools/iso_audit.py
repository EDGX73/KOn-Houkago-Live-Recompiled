"""Read-only ISO9660 inventory and safe extraction for the local PSP copies."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, struct, sys

SECTOR = 2048

class Iso9660:
    def __init__(self, path):
        self.path = Path(path)
        self.file = self.path.open('rb')
        self.size = self.path.stat().st_size
        self.pvd = None
        for sector in range(16, 128):
            d = self.read(sector * SECTOR, SECTOR)
            if d[1:6] != b'CD001':
                raise ValueError('Invalid ISO9660 descriptor signature')
            if d[0] == 1:
                self.pvd = d
                break
            if d[0] == 255:
                break
        if self.pvd is None:
            raise ValueError('No primary volume descriptor')
        if struct.unpack_from('<H', self.pvd, 128)[0] != SECTOR:
            raise ValueError('Unexpected logical sector size')
        self.volume = self.pvd[40:72].decode('ascii', 'replace').strip()
        self.entries = []
        self._walk(self.pvd[156:156 + self.pvd[156]], '', set(), 0)

    def close(self):
        self.file.close()

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError(f'ISO range out of bounds: {offset}:{size}')
        self.file.seek(offset)
        b = self.file.read(size)
        if len(b) != size:
            raise ValueError('Truncated ISO read')
        return b

    def _walk(self, record, parent, visited, depth):
        if depth > 64:
            raise ValueError('ISO directory nesting too deep')
        extent, size = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
        offset = extent * SECTOR
        if (extent, size) in visited:
            return
        visited.add((extent, size))
        data = self.read(offset, size)
        pos = 0
        while pos < len(data):
            length = data[pos]
            if not length:
                pos = ((pos // SECTOR) + 1) * SECTOR
                continue
            rec = data[pos:pos + length]
            if len(rec) < 34 or rec[32] + 33 > len(rec):
                raise ValueError('Malformed ISO directory record')
            ident = rec[33:33 + rec[32]]
            pos += length
            if ident in (b'\x00', b'\x01'):
                continue
            name = ident.decode('ascii', 'strict').split(';', 1)[0].rstrip('.')
            if not name or name in ('.', '..') or any(c in name for c in '/\\:'):
                raise ValueError(f'Unsafe ISO name {name!r}')
            path = f'{parent}/{name}' if parent else name
            if rec[25] & 0x80:
                raise ValueError('Multi-extent ISO entry not supported; extraction stopped')
            if rec[25] & 2:
                self._walk(rec, path, visited, depth + 1)
            else:
                ent = {'path':path, 'offset':struct.unpack_from('<I', rec, 2)[0] * SECTOR,
                       'size':struct.unpack_from('<I', rec, 10)[0], 'flags':rec[25]}
                if ent['offset'] + ent['size'] > self.size:
                    raise ValueError(f'Out of bounds file: {path}')
                self.entries.append(ent)

    def read_entry(self, path):
        e = next(e for e in self.entries if e['path'] == path)
        return self.read(e['offset'], e['size'])

    def hash_range(self, offset, size):
        h = hashlib.sha256()
        self.file.seek(offset)
        remaining = size
        while remaining:
            b = self.file.read(min(4 * 1024 * 1024, remaining))
            if not b:
                raise ValueError('Short read during hashing')
            h.update(b)
            remaining -= len(b)
        return h.hexdigest()

    def extract(self, ent, destination):
        root = Path(destination).resolve()
        target = (root / ent['path']).resolve()
        if not target.is_relative_to(root):
            raise ValueError('Extraction path escapes destination')
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            if target.stat().st_size == ent['size']:
                with target.open('rb') as f:
                    h = hashlib.file_digest(f, 'sha256').hexdigest()
                if h == ent['sha256']:
                    return
            raise FileExistsError(f'Refusing to overwrite a different extracted file: {target}')
        self.file.seek(ent['offset'])
        h = hashlib.sha256()
        with target.open('xb') as f:
            remaining = ent['size']
            while remaining:
                b = self.file.read(min(4 * 1024 * 1024, remaining))
                if not b:
                    raise ValueError('Short read during extraction')
                f.write(b)
                h.update(b)
                remaining -= len(b)
        if h.hexdigest() != ent['sha256']:
            raise ValueError(f'Extraction checksum mismatch: {target}')

def parse_sfo(data):
    if data[:4] != b'\x00PSF':
        raise ValueError('Invalid PARAM.SFO')
    version, keys, values, count = struct.unpack_from('<4I', data, 4)
    result = {}
    if count > 1024 or 20 + count * 16 > len(data):
        raise ValueError('Invalid SFO index count')
    for i in range(count):
        keyoff, fmt, length, maxlen, valueoff = struct.unpack_from('<HHIII', data, 20 + i * 16)
        kp = keys + keyoff
        if kp >= len(data): raise ValueError('Invalid SFO key')
        end = data.find(b'\x00', kp)
        if end < 0: raise ValueError('Unterminated SFO key')
        key = data[kp:end].decode('utf-8', 'strict')
        start = values + valueoff
        if start + length > len(data): raise ValueError('Invalid SFO value')
        v = data[start:start + length]
        if fmt == 0x204:
            result[key] = v.rstrip(b'\x00').decode('utf-8', 'replace')
        elif fmt == 0x404 and len(v) == 4:
            result[key] = struct.unpack('<I', v)[0]
        else:
            result[key] = {'format':fmt, 'hex':v.hex()}
    return result

def audit(path, label, output, extract):
    iso = Iso9660(path)
    try:
        print(f'Inventory {label}: {iso.path.name}', flush=True)
        for e in iso.entries:
            e['sha256'] = iso.hash_range(e['offset'], e['size'])
            e['magic_hex'] = iso.read(e['offset'], min(16, e['size'])).hex()
        sfo = parse_sfo(iso.read_entry('PSP_GAME/PARAM.SFO'))
        result = {'label':label, 'iso':str(iso.path.resolve()), 'size':iso.size,
                  'sha256':iso.hash_range(0, iso.size), 'volume':iso.volume,
                  'sfo':sfo, 'files':iso.entries}
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        if extract:
            for e in iso.entries:
                iso.extract(e, extract / label)
        print(f'{label}: {len(iso.entries)} files; ID={sfo.get("DISC_ID")}; extracted and verified={bool(extract)}', flush=True)
        return result
    finally:
        iso.close()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--extract', action='store_true')
    args = p.parse_args()
    root = args.root.resolve()
    sources = {'jp':root / 'Roms PSP/K-ON! Houkago Live!! (Japan).iso',
               'en':root / 'Roms PSP/K-on After School (English).iso'}
    results = {label:audit(path, label, root / f'analysis/iso_{label}.json',
               root / 'analysis/extracted' if args.extract else None) for label,path in sources.items()}
    a,b = [{e['path']:e for e in results[k]['files']} for k in ('jp','en')]
    changed, same = [], []
    for name in sorted(a.keys() & b.keys()):
        (same if a[name]['sha256'] == b[name]['sha256'] else changed).append(name)
    diff = {'identical':same, 'changed':changed, 'japanese_only':sorted(a.keys() - b.keys()),
            'english_only':sorted(b.keys() - a.keys())}
    (root / 'analysis/iso_comparison.json').write_text(json.dumps(diff, indent=2), encoding='utf-8')
    print(json.dumps(diff, indent=2))

if __name__ == '__main__':
    main()
