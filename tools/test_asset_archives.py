"""Small independent fixtures verify archive boundaries and extraction semantics."""
import pathlib
import struct
import tempfile
import unittest
from cpk_archive import CpkArchive, FormatError, UtfTable, decompress_crilayla, extract_entries, safe_destination, utf_xor
from fmdx_archive import FmdxArchive


def make_utf(columns, rows, name="fixture"):
    strings = bytearray(b"\0")
    cache = {"": 0}
    blobs = bytearray()
    def string(value):
        if value not in cache:
            cache[value] = len(strings)
            strings.extend(value.encode("utf-8") + b"\0")
        return cache[value]
    def encoded(typ, value):
        if typ == 10:
            return struct.pack(">I", string(value))
        if typ == 11:
            offset = len(blobs)
            blobs.extend(value)
            return struct.pack(">II", offset, len(value))
        return struct.pack(">" + UtfTable._scalars[typ], value)
    name_offset = string(name)
    schema = bytearray()
    for col, typ, storage, constant in columns:
        schema.extend(struct.pack(">BI", typ | storage, string(col)))
        if storage == 0x30:
            schema.extend(encoded(typ, constant))
    body = bytearray()
    row_size = 0
    for row in rows:
        start = len(body)
        for col, typ, storage, constant in columns:
            if storage == 0x50:
                body.extend(encoded(typ, row[col]))
        row_size = len(body) - start
    row_offset = 32 + len(schema)
    string_offset = row_offset + len(body)
    data_offset = string_offset + len(strings)
    total = data_offset + len(blobs)
    header = b"@UTF" + struct.pack(">IIIIIHHI", total - 8, row_offset - 8, string_offset - 8, data_offset - 8, name_offset, len(columns), row_size, len(rows))
    return header + schema + body + strings + blobs


def packet(magic, table, encrypted=False):
    body = utf_xor(table) if encrypted else table
    return struct.pack("<4sIQ", magic, 0xFF, len(body)) + body


def make_cpk(payload=b"original", path="folder/file.bin", encrypted=False):
    dirname, filename = path.rsplit("/", 1)
    hdr = make_utf([(n, 6, 0x50, None) for n in ("ContentOffset", "TocOffset", "Files")], [{"ContentOffset": 2048, "TocOffset": 1024, "Files": 1}])
    toc = make_utf([(n, typ, 0x50, None) for n, typ in [("DirName", 10), ("FileName", 10), ("FileOffset", 6), ("FileSize", 4), ("ExtractSize", 4)]], [{"DirName": dirname, "FileName": filename, "FileOffset": 1024, "FileSize": len(payload), "ExtractSize": len(payload)}])
    first, second = packet(b"CPK ", hdr, encrypted), packet(b"TOC ", toc, encrypted)
    return first.ljust(1024, b"\0") + second.ljust(1024, b"\0") + payload


def make_fmdx(path="folder/file.bin", payload=b"original"):
    header = struct.pack("<4sIII", b"FMDX", 0x54 + 0x90 + len(payload), 1, 1).ljust(0x54, b"\0")
    entry = struct.pack("<128sIIII", path.encode(), 0x54 + 0x90, len(payload), 0, 0)
    return header + entry + payload


class ArchiveTests(unittest.TestCase):
    def test_utf_zero_constant_blob_and_numeric_rows(self):
        nested = b"blob"
        cols = [("empty", 4, 0x10, None), ("same", 10, 0x30, "fixed"), ("payload", 11, 0x30, nested), ("value", 5, 0x50, None)]
        table = UtfTable(make_utf(cols, [{"value": -1}, {"value": 99}]))
        self.assertEqual(table.rows, [{"empty": 0, "same": "fixed", "payload": nested, "value": -1}, {"empty": 0, "same": "fixed", "payload": nested, "value": 99}])
        self.assertEqual(utf_xor(utf_xor(b"abcdef")), b"abcdef")

    def test_encrypted_cpk_offset_base_and_safe_resume(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            source = root / "source.cpk"
            original = make_cpk(encrypted=True)
            source.write_bytes(original)
            archive = CpkArchive(source)
            self.assertEqual(archive.entries[0].offset, 2048)
            self.assertEqual(archive.read(archive.entries[0]), b"original")
            result = extract_entries(archive, root / "output", [])
            self.assertFalse(result["entries"][0]["reused"])
            self.assertTrue(extract_entries(archive, root / "output", ["folder/*"])["entries"][0]["reused"])
            (root / "output/folder/file.bin").write_bytes(b"preserve me")
            with self.assertRaises(FormatError):
                extract_entries(archive, root / "output", [])
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual((root / "output/folder/file.bin").read_bytes(), b"preserve me")

    def test_truncated_and_traversal_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            source = root / "source.cpk"
            source.write_bytes(make_cpk()[:-1])
            with self.assertRaises(FormatError):
                CpkArchive(source)
            source.write_bytes(make_cpk(path="../escaped.bin"))
            with self.assertRaises(FormatError):
                CpkArchive(source)
            for path in ("../escape", "C:/file", "/absolute", "folder/CON.txt", "x/../a", "a\\..\\b"):
                with self.assertRaises(FormatError):
                    safe_destination(root, path)
            with self.assertRaises(FormatError):
                UtfTable(b"@UTF")

    def test_itoc_sort_and_alignment(self):
        nested = make_utf([(n, 4, 0x50, None) for n in ("ID", "FileSize", "ExtractSize")], [{"ID": 2, "FileSize": 3, "ExtractSize": 3}, {"ID": 1, "FileSize": 4, "ExtractSize": 4}])
        hdr = make_utf([(n, 6, 0x50, None) for n in ("ContentOffset", "ItocOffset", "Files", "Align")], [{"ContentOffset": 2048, "ItocOffset": 1024, "Files": 2, "Align": 16}])
        itoc = make_utf([("DataL", 11, 0x50, None)], [{"DataL": nested}])
        fixture = packet(b"CPK ", hdr).ljust(1024, b"\0") + packet(b"ITOC", itoc).ljust(1024, b"\0") + b"ABCD".ljust(16, b"\0") + b"XYZ"
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / "ids.cpk"
            source.write_bytes(fixture)
            archive = CpkArchive(source)
            self.assertEqual([e.path for e in archive.entries], ["0001", "0002"])
            self.assertEqual([archive.read(e) for e in archive.entries], [b"ABCD", b"XYZ"])

    def test_crilayla_overlapping_history(self):
        forward = "".join("0" + f"{v:08b}" for v in b"cba") + "1" + "0" * 13 + "11000"
        forward += "0" * ((-len(forward)) % 8)
        stream = bytes(int(forward[i:i+8], 2) for i in range(0, len(forward), 8))[::-1]
        fixture = b"CRILAYLA" + struct.pack("<II", 9, len(stream)) + stream + bytes(256)
        self.assertEqual(decompress_crilayla(fixture), bytes(256) + b"abcabcabc")
    def test_fmdx_and_invalid_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / "test.bin"
            source.write_bytes(make_fmdx())
            archive = FmdxArchive(source)
            self.assertEqual(archive.read(archive.entries[0]), b"original")
            source.write_bytes(make_fmdx()[:-1])
            with self.assertRaises(FormatError):
                FmdxArchive(source)
            source.write_bytes(make_fmdx(path="../escaped"))
            with self.assertRaises(FormatError):
                FmdxArchive(source)

    def test_crilayla_literal_and_backreference_bounds(self):
        # Stream is consumed from last byte backwards, most significant bit first.
        forward = "".join("0" + f"{v:08b}" for v in b"ZYX")
        forward += "0" * ((-len(forward)) % 8)
        stream = bytes(int(forward[i:i+8], 2) for i in range(0, len(forward), 8))[::-1]
        fixture = b"CRILAYLA" + struct.pack("<II", 3, len(stream)) + stream + bytes(range(256))
        self.assertEqual(decompress_crilayla(fixture), bytes(range(256)) + b"XYZ")
        with self.assertRaises(FormatError):
            decompress_crilayla(fixture, 258)
        with self.assertRaises(FormatError):
            decompress_crilayla(fixture[:-1])
        malformed = b"CRILAYLA" + struct.pack("<II", 3, 2) + b"\0\x80" + bytes(256)
        with self.assertRaises(FormatError):
            decompress_crilayla(malformed)


if __name__ == "__main__":
    unittest.main()


