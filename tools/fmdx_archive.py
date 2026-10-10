"""Read/extract K-On FMDX named containers. Original archives remain unchanged."""
from __future__ import annotations
from dataclasses import asdict
import hashlib
from pathlib import Path
import struct
from cpk_archive import Entry, FormatError, MAX_MEMBER_SIZE, archive_cli, safe_relative, sha256_file


class FmdxArchive:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        if self.size < 0x54:
            raise FormatError("Truncated FMDX header")
        with self.path.open("rb") as source:
            magic, declared_size, count, version = struct.unpack("<4sIII", source.read(16))
            if magic != b"FMDX":
                raise FormatError("Expected FMDX container")
            if count > 1_000_000 or 0x54 + count * 0x90 > self.size:
                raise FormatError("FMDX directory exceeds container size")
            self.header = {"declared_size": declared_size, "entry_count": count, "version_field": version}
            source.seek(0x54)
            self.entries = []
            self.unknown_fields = []
            names = set()
            for index in range(count):
                raw = source.read(0x90)
                path_bytes, offset, length, unknown1, unknown2 = struct.unpack("<128sIIII", raw)
                name = safe_relative(path_bytes.split(b"\0", 1)[0].decode("ascii"))
                if name.casefold() in names:
                    raise FormatError(f"Duplicate output path: {name}")
                names.add(name.casefold())
                if offset < 0x54 + count * 0x90 or offset + length > self.size:
                    raise FormatError(f"FMDX member outside payload region: {name}")
                self.entries.append(Entry(name, offset, length, length, index, "FMDX"))
                self.unknown_fields.append((unknown1, unknown2))

    def read(self, entry: Entry, max_size: int = MAX_MEMBER_SIZE) -> bytes:
        if entry.stored_size > max_size:
            raise FormatError(f"{entry.path} exceeds maximum member size {max_size}")
        with self.path.open("rb") as source:
            source.seek(entry.offset)
            data = source.read(entry.stored_size)
        if len(data) != entry.stored_size:
            raise FormatError(f"Member truncated: {entry.path}")
        return data

    def metadata(self, hashes: bool = False):
        records = []
        with self.path.open("rb") as source:
            for entry, unknown in zip(self.entries, self.unknown_fields):
                source.seek(entry.offset)
                prefix = source.read(min(entry.stored_size, 16))
                record = {**asdict(entry), "unknown_0x88": unknown[0], "unknown_0x8c": unknown[1], "stored_magic_hex": prefix.hex()}
                if hashes:
                    record["sha256"] = hashlib.sha256(self.read(entry)).hexdigest()
                records.append(record)
        return {"format": "FMDX", "source": str(self.path.resolve()), "source_size": self.size, "source_sha256": sha256_file(self.path) if hashes else None, "header": self.header, "entry_count": len(records), "entries": records}


if __name__ == "__main__":
    raise SystemExit(archive_cli(FmdxArchive, "List/extract FMDX; preserve originals and record hashes."))
