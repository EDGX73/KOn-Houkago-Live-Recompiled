"""Synthetic ABI and hostile-input tests; no game data is required."""
import importlib.util
from pathlib import Path
import struct
import unittest

script = Path(__file__).resolve().parents[1] / "executable_audit.py"
spec = importlib.util.spec_from_file_location("executable_audit", script)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def fixture():
    data = bytearray(720)
    data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 0xFFA0, 8, 1, 0, 52, 600, 0, 52, 32, 1, 40, 2, 0)
    struct.pack_into("<8I", data, 52, 1, 160, 0, 512, 408, 448, 5, 16)
    struct.pack_into("<IHHBBHII", data, 224, 0x80, 0x101, 0, 5, 0, 2, 0xA0, 0xA8)
    data[288:297] = b"sceTest\x00\x00"
    struct.pack_into("<II", data, 320, 0x12345678, 0xABCDEF01)
    struct.pack_into("<IIII", data, 328, 0x03E00008, 0, 0x03E00008, 0)
    struct.pack_into("<HBB28sIIIII", data, 512, 0, 1, 1, b"test", 0, 0, 0, 0x40, 0x54)
    struct.pack_into("<II", data, 576, 0x10, 2)
    struct.pack_into("<10I", data, 640, 0, 0x700000A0, 0, 0, 576, 8, 0, 0, 4, 8)
    return bytes(data)


class AuditTests(unittest.TestCase):
    def test_valid_bounded_imports_and_relocations(self):
        image = audit.ElfImage(fixture())
        info = image.module_and_imports()
        self.assertTrue(info["table_complete"])
        self.assertEqual(info["function_count"], 2)
        self.assertEqual(info["libraries"][0]["name"], "sceTest")
        self.assertEqual(info["libraries"][0]["functions"][1]["nid"], "0xABCDEF01")
        self.assertEqual(image.relocations()[0]["records"], 1)
        self.assertEqual(image.symbol_tables(), [])

    def test_rejects_truncated_program_table(self):
        with self.assertRaises(audit.FormatError):
            audit.ElfImage(fixture()[:75])

    def test_rejects_non_mips(self):
        data = bytearray(fixture())
        struct.pack_into("<H", data, 18, 3)
        with self.assertRaises(audit.FormatError):
            audit.ElfImage(data)

    def test_import_zero_stride_stops_safely(self):
        data = bytearray(fixture())
        data[232] = 0
        info = audit.ElfImage(data).module_and_imports()
        self.assertFalse(info["table_complete"])
        self.assertEqual(info["function_count"], 0)
        self.assertEqual(len(info["errors"]), 1)

    def test_import_nid_pointer_into_bss_rejected(self):
        data = bytearray(fixture())
        struct.pack_into("<I", data, 236, 416)
        info = audit.ElfImage(data).module_and_imports()
        self.assertEqual(info["function_count"], 0)
        self.assertTrue(info["errors"])

    def test_relb_minimal_stream_and_truncation(self):
        image = audit.ElfImage(fixture())
        # One flag (base), one flag (relocate), one type: R_MIPS_32.
        stream = bytes([0, 0, 2, 1, 3, 0, 1, 2, 2]) + struct.pack("<HH", 1, 0x12)
        good = image._relb(stream, 1)
        self.assertTrue(good["decoded"])
        self.assertEqual(good["records"], 1)
        self.assertEqual(good["types"], {"32": 1})
        bad = image._relb(stream[:-1], 1)
        self.assertFalse(bad["decoded"])
        self.assertTrue(bad["errors"])


if __name__ == "__main__":
    unittest.main()
