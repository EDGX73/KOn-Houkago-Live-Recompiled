"""Read/extract CRI CPK archives without changing the source.

Independent implementation of CPK/@UTF/CRILAYLA binary formats. See
../docs/ASSET_FORMATS.md for references and supported cases.
"""
from __future__ import annotations
import argparse
from dataclasses import asdict, dataclass
import fnmatch
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import sys

MAX_METADATA_SIZE = 128 * 1024 * 1024
MAX_MEMBER_SIZE = 512 * 1024 * 1024

class FormatError(ValueError):
    """Invalid, truncated or unsupported archive data."""


def span(data: bytes, offset: int, size: int, label: str = "data") -> bytes:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise FormatError(f"{label}: range {offset:#x}+{size:#x} exceeds {len(data):#x}")
    return data[offset:offset + size]


def safe_relative(name: str) -> str:
    name = name.replace("\\", "/")
    if not name or name.startswith("/") or any(ord(c) < 32 for c in name):
        raise FormatError(f"Unsafe archive path: {name!r}")
    parts = name.split("/")
    while parts and parts[0] == ".":
        parts.pop(0)
    reserved = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}
    if not parts or any(p in ("", ".", "..") or ":" in p or p.endswith((" ", ".")) or p.split(".")[0].upper() in reserved for p in parts):
        raise FormatError(f"Unsafe archive path: {name!r}")
    return str(PurePosixPath(*parts))


def safe_destination(root: Path, name: str) -> Path:
    root = root.resolve()
    target = root.joinpath(*safe_relative(name).split("/"))
    try:
        target.resolve().relative_to(root)
    except ValueError as exc:
        raise FormatError(f"Archive path escapes output folder: {name!r}") from exc
    return target


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def utf_xor(data: bytes) -> bytes:
    key = 0x5F
    out = bytearray(len(data))
    for i, value in enumerate(data):
        out[i] = value ^ key
        key = (key * 0x15) & 0xFF
    return bytes(out)


class UtfTable:
    """CPK table; offsets are relative to byte 8, scalar values big endian."""
    _scalars = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "Q", 7: "q", 8: "f", 9: "d"}

    def __init__(self, data: bytes):
        if span(data, 0, 4, "UTF magic") != b"@UTF":
            raise FormatError("Expected @UTF table")
        total, row_off, str_off, blob_off, name_off, cols, row_size, rows = struct.unpack(">IIIIIHHI", span(data, 4, 28, "UTF header"))
        self.data = span(data, 0, total + 8, "UTF table length")
        self.row_off, self.str_off, self.blob_off = row_off + 8, str_off + 8, blob_off + 8
        self.row_size = row_size
        if not (32 <= self.row_off <= self.str_off <= self.blob_off <= len(self.data)):
            raise FormatError("Invalid UTF region order")
        if cols > 4096 or rows > 1_000_000:
            raise FormatError("UTF table counts exceed limits")
        if self.row_off + rows * row_size > self.str_off:
            raise FormatError("UTF rows overlap the string table")
        self.name = self.string(name_off)
        self.columns = []
        cursor = 32
        for _ in range(cols):
            flags = span(self.data, cursor, 1, "UTF column flags")[0]
            name_offset = struct.unpack(">I", span(self.data, cursor + 1, 4, "UTF column name"))[0]
            cursor += 5
            storage, typ = flags & 0xF0, flags & 0x0F
            if typ not in self._scalars and typ not in (10, 11):
                raise FormatError(f"Unsupported UTF value type {typ:#x}")
            value = None
            if storage == 0x30:
                value, cursor = self.value(typ, cursor)
            elif storage == 0x10:
                value = b"" if typ == 11 else "" if typ == 10 else 0
            elif storage not in (0x00, 0x50):
                raise FormatError(f"Unsupported UTF storage {storage:#x}")
            self.columns.append((self.string(name_offset), storage, typ, value))
        if cursor > self.row_off:
            raise FormatError("UTF column schema overlaps rows")
        self.rows = []
        for index in range(rows):
            cursor = self.row_off + index * row_size
            row = {}
            for name, storage, typ, value in self.columns:
                if storage == 0x50:
                    value, cursor = self.value(typ, cursor)
                row[name] = value
            if cursor > self.row_off + (index + 1) * row_size:
                raise FormatError("UTF values exceed declared row size")
            self.rows.append(row)

    def string(self, relative: int) -> str:
        start = self.str_off + relative
        if relative < 0 or start >= self.blob_off:
            raise FormatError("UTF string pointer outside string region")
        stop = self.data.find(b"\0", start, self.blob_off)
        if stop < 0:
            raise FormatError("Unterminated UTF string")
        raw = self.data[start:stop]
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            return raw.decode("cp932", errors="strict")

    def value(self, typ: int, offset: int):
        if typ in self._scalars:
            fmt = ">" + self._scalars[typ]
            length = struct.calcsize(fmt)
            return struct.unpack(fmt, span(self.data, offset, length, "UTF scalar"))[0], offset + length
        if typ == 10:
            pointer = struct.unpack(">I", span(self.data, offset, 4, "UTF string pointer"))[0]
            return self.string(pointer), offset + 4
        pointer, length = struct.unpack(">II", span(self.data, offset, 8, "UTF blob pointer"))
        return span(self.data, self.blob_off + pointer, length, "UTF blob"), offset + 8


def decompress_crilayla(data: bytes, max_size: int = MAX_MEMBER_SIZE) -> bytes:
    if span(data, 0, 8, "compression magic") != b"CRILAYLA":
        raise FormatError("Expected CRILAYLA")
    tail_size, stream_size = struct.unpack("<II", span(data, 8, 8, "CRILAYLA header"))
    total = tail_size + 256
    if total > max_size:
        raise FormatError(f"CRILAYLA output {total} exceeds {max_size}")
    prefix = span(data, 16 + stream_size, 256, "CRILAYLA preserved prefix")
    if 16 + stream_size + 256 != len(data):
        raise FormatError("CRILAYLA length differs from compressed payload length")
    out = bytearray(total)
    out[:256] = prefix
    stream = span(data, 16, stream_size, "CRILAYLA bitstream")
    byte_index, pool, available = len(stream) - 1, 0, 0
    def bits(count: int) -> int:
        nonlocal byte_index, pool, available
        while available < count:
            if byte_index < 0:
                raise FormatError("Truncated CRILAYLA bitstream")
            pool = (pool << 8) | stream[byte_index]
            available += 8
            byte_index -= 1
        available -= count
        value = pool >> available
        pool &= (1 << available) - 1
        return value
    cursor = total - 1
    while cursor >= 256:
        if bits(1) == 0:
            out[cursor] = bits(8)
            cursor -= 1
            continue
        distance = bits(13) + 3
        length = 3
        for width in (2, 3, 5, 8):
            value = bits(width)
            length += value
            if value != (1 << width) - 1:
                break
        else:
            while True:
                value = bits(8)
                length += value
                if value != 255:
                    break
        if length > cursor - 255 or cursor + distance >= total:
            raise FormatError("Invalid CRILAYLA backreference")
        for _ in range(length):
            out[cursor] = out[cursor + distance]
            cursor -= 1
    return bytes(out)


@dataclass(frozen=True)
class Entry:
    path: str
    offset: int
    stored_size: int
    extracted_size: int
    id: int | None = None
    table: str = "TOC"


class CpkArchive:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        header = self.packet(0, b"CPK ")
        if len(header.rows) != 1:
            raise FormatError("CPK header must have exactly one row")
        self.header = header.rows[0]
        self.entries: list[Entry] = []
        content = self.header.get("ContentOffset")
        toc = self.header.get("TocOffset")
        if self.valid_offset(toc):
            table = self.packet(toc, b"TOC ")
            base = min(content, toc) if self.valid_offset(content) else toc
            for row in table.rows:
                directory = row.get("DirName") or ""
                if directory == "<NULL>":
                    directory = ""
                filename = row.get("FileName")
                if not isinstance(filename, str) or not filename:
                    raise FormatError("CPK TOC entry is missing its filename")
                full = f"{directory}/{filename}" if directory else filename
                stored = self.integer(row, "FileSize")
                extracted = row.get("ExtractSize")
                extracted = stored if extracted is None else int(extracted)
                self.entries.append(Entry(safe_relative(full), base + self.integer(row, "FileOffset"), stored, extracted, row.get("ID")))
        else:
            itoc = self.header.get("ItocOffset")
            if not self.valid_offset(itoc) or not self.valid_offset(content):
                raise FormatError("CPK has neither readable TOC nor supported ITOC")
            table = self.packet(itoc, b"ITOC")
            if len(table.rows) != 1:
                raise FormatError("ITOC table must have one row")
            records = {}
            for key in ("DataL", "DataH"):
                blob = table.rows[0].get(key)
                if not blob:
                    continue
                for row in UtfTable(blob).rows:
                    index = self.integer(row, "ID")
                    if index in records:
                        raise FormatError(f"Duplicate ITOC ID {index}")
                    records[index] = row
            cursor = content
            alignment = int(self.header.get("Align") or 1)
            if alignment <= 0 or alignment > 1024 * 1024:
                raise FormatError("Invalid CPK alignment")
            for index, row in sorted(records.items()):
                stored = self.integer(row, "FileSize")
                extracted = int(row.get("ExtractSize") or stored)
                self.entries.append(Entry(f"{index:04d}", cursor, stored, extracted, index, "ITOC"))
                cursor += (stored + alignment - 1) // alignment * alignment
        names = set()
        for entry in self.entries:
            if entry.path.casefold() in names:
                raise FormatError(f"Duplicate output path: {entry.path}")
            names.add(entry.path.casefold())
            if entry.offset < 0 or entry.stored_size < 0 or entry.extracted_size < 0 or entry.offset + entry.stored_size > self.size:
                raise FormatError(f"Member outside CPK: {entry.path}")
        expected = self.header.get("Files")
        if expected is not None and int(expected) != len(self.entries):
            raise FormatError(f"CPK expected {expected} files; TOC has {len(self.entries)}")

    @staticmethod
    def integer(row, key):
        value = row.get(key)
        if not isinstance(value, int) or value < 0:
            raise FormatError(f"Invalid CPK field {key}: {value!r}")
        return value

    @staticmethod
    def valid_offset(value) -> bool:
        return isinstance(value, int) and 0 < value < 0xFFFFFFFFFFFFFFFF

    def packet(self, offset: int, magic: bytes) -> UtfTable:
        if offset < 0 or offset + 16 > self.size:
            raise FormatError("CPK packet header outside file")
        with self.path.open("rb") as source:
            source.seek(offset)
            got, flags, length = struct.unpack("<4sIQ", source.read(16))
            if got != magic:
                raise FormatError(f"Expected {magic!r} packet at {offset:#x}, got {got!r}")
            if length > MAX_METADATA_SIZE or offset + 16 + length > self.size:
                raise FormatError("CPK packet metadata exceeds bounds")
            data = source.read(length)
        if data[:4] != b"@UTF":
            data = utf_xor(data)
        return UtfTable(data)

    def read(self, entry: Entry, max_size: int = MAX_MEMBER_SIZE) -> bytes:
        if max(entry.stored_size, entry.extracted_size) > max_size:
            raise FormatError(f"{entry.path} exceeds maximum member size {max_size}")
        with self.path.open("rb") as source:
            source.seek(entry.offset)
            data = source.read(entry.stored_size)
        if len(data) != entry.stored_size:
            raise FormatError(f"Member truncated: {entry.path}")
        if data.startswith(b"CRILAYLA"):
            data = decompress_crilayla(data, max_size)
        if len(data) != entry.extracted_size:
            raise FormatError(f"Extracted size mismatch for {entry.path}: {len(data)} != {entry.extracted_size}")
        return data

    def metadata(self, hashes: bool = False):
        members = []
        for entry in self.entries:
            record = asdict(entry)
            with self.path.open("rb") as source:
                source.seek(entry.offset)
                prefix = source.read(min(entry.stored_size, 16))
            record["compressed"] = prefix.startswith(b"CRILAYLA")
            record["stored_magic_hex"] = prefix.hex()
            if hashes:
                record["sha256"] = hashlib.sha256(self.read(entry)).hexdigest()
            members.append(record)
        return {"format": "CPK", "source": str(self.path.resolve()), "source_size": self.size, "source_sha256": sha256_file(self.path) if hashes else None, "entry_count": len(members), "entries": members}


def extract_entries(archive, output: Path, patterns: list[str], max_size: int = MAX_MEMBER_SIZE):
    output = output.resolve()
    selected = [e for e in archive.entries if not patterns or any(fnmatch.fnmatchcase(e.path, p) for p in patterns)]
    if not selected:
        raise FormatError("No members match the requested patterns")
    targets = [(e, safe_destination(output, e.path)) for e in selected]
    if any(target == archive.path.resolve() for _, target in targets):
        raise FormatError("Refusing to replace the source archive")
    records = []
    for entry, target in targets:
        data = archive.read(entry, max_size)
        digest = hashlib.sha256(data).hexdigest()
        reused = False
        if target.exists():
            if not target.is_file() or sha256_file(target) != digest:
                raise FormatError(f"Refusing to overwrite existing different content: {target}")
            reused = True
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            # Exclusive creation prevents replacing a file introduced during extraction.
            with target.open("xb") as sink:
                sink.write(data)
        records.append({**asdict(entry), "sha256": digest, "reused": reused})
    return {"format": archive.__class__.__name__, "source": str(archive.path.resolve()), "source_sha256": sha256_file(archive.path), "output": str(output), "entry_count": len(records), "entries": records}


def write_json(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def archive_cli(archive_type, description: str):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("command", choices=("list", "extract"))
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path, help="JSON file for list; destination folder for extract")
    parser.add_argument("--manifest", type=Path, help="Write extraction hashes to this JSON file")
    parser.add_argument("--hash", action="store_true", help="Include decoded member/source SHA-256 in a listing")
    parser.add_argument("--pattern", action="append", default=[], help="Archive path glob; repeat for multiple patterns")
    parser.add_argument("--max-file-size", type=int, default=MAX_MEMBER_SIZE)
    args = parser.parse_args()
    try:
        archive = archive_type(args.archive)
        if args.command == "list":
            result = archive.metadata(args.hash)
            if args.pattern:
                result["entries"] = [e for e in result["entries"] if any(fnmatch.fnmatchcase(e["path"], p) for p in args.pattern)]
                result["entry_count"] = len(result["entries"])
            if args.output:
                if args.output.resolve() == args.archive.resolve():
                    raise FormatError("Listing output cannot replace source archive")
                write_json(args.output, result)
            else:
                print(json.dumps(result, indent=2, ensure_ascii=False))
        else:
            if not args.output:
                parser.error("extract requires --output folder")
            if args.manifest and args.manifest.resolve() == args.archive.resolve():
                raise FormatError("Manifest cannot replace source archive")
            result = extract_entries(archive, args.output, args.pattern, args.max_file_size)
            if args.manifest:
                if any(args.manifest.resolve() == safe_destination(args.output, e["path"]).resolve() for e in result["entries"]):
                    raise FormatError("Manifest path conflicts with extracted member")
                write_json(args.manifest, result)
            print(json.dumps({"extracted": result["entry_count"], "output": result["output"], "manifest": str(args.manifest) if args.manifest else None}))
    except (FormatError, OSError, UnicodeError, struct.error) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(archive_cli(CpkArchive, "List/extract CPK; preserve originals and record hashes."))

