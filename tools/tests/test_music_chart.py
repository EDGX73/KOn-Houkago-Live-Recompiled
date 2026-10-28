"""Verify the own KSD layout against the supplied original bundle and damage."""
import struct
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from fmdx_archive import FmdxArchive
from music_chart import Chart, ChartError


class MusicChartTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        bundle = ROOT / "assets/source/en/std/PSP_GAME/USRDIR/bind/play_m03_single.bin"
        if not bundle.is_file():  # the game's own data: only on a PC with the installed disc
            raise unittest.SkipTest("needs the original chart bundle (assets/source)")
        cls.archive = FmdxArchive(bundle)
        cls.samples = [(e.path, cls.archive.read(e)) for e in cls.archive.entries if e.path.endswith(".ksd")]
        cls.normal = next(b for name, b in cls.samples if name.endswith("hotchkiss_gt1_no.ksd"))

    def test_original_bundle_roundtrips(self):
        self.assertGreater(len(self.samples), 10)
        for name, data in self.samples:
            with self.subTest(name=name):
                chart = Chart.read(data)
                self.assertEqual(chart.write(), data)
                self.assertEqual(sum(m.count for m in chart.measures), len(chart.notes))

    def test_known_normal_table(self):
        chart = Chart.read(self.normal)
        self.assertEqual(len(chart.notes), 150)
        self.assertEqual(len(chart.measures), 80)
        self.assertEqual(chart.notes[0].tick, 513)
        self.assertEqual(chart.notes[-1].tick, 7035)
        self.assertEqual(chart.notes[-1].flags, 256)
        self.assertEqual(sum(n.duration != 0 for n in chart.notes), 3)

    def test_truncation_and_index_damage_rejected(self):
        for data in (b"", self.normal[:7], self.normal[:-1]):
            with self.assertRaises(ChartError):
                Chart.read(data)
        data = bytearray(self.normal)
        struct.pack_into("<H", data, 10, 1)
        with self.assertRaises(ChartError):
            Chart.read(data)
        data = bytearray(self.normal)
        struct.pack_into("<H", data, 8, 151)
        with self.assertRaises(ChartError):
            Chart.read(data)

    def test_invalid_note_measure_and_position_rejected(self):
        start = len(self.normal)-150*12
        for at, value in ((start+3, 250), (start+4, 16)):
            data = bytearray(self.normal);data[at] = value
            with self.assertRaises(ChartError):
                Chart.read(data)

    def test_unused_tail_tick_reset_is_preserved(self):
        data = bytearray(self.normal)
        # Last measure contains no notes; observed original files can reset it.
        struct.pack_into("<I", data, 8+79*8+4, 84)
        self.assertEqual(Chart.read(data).write(), data)
        # An active measure cannot move backwards in its timing table.
        struct.pack_into("<I", data, 8+5*8+4, 0)
        with self.assertRaises(ChartError):
            Chart.read(data)


if __name__ == "__main__":
    unittest.main()
