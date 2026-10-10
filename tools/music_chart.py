"""Own reader for the observed KSD layout; preserve every unknown field.

Field names describe structure observed in supplied game files. Timing units,
button meanings and hold semantics require separate runtime verification.
Never modifies an original archive or chart.
"""
from __future__ import annotations
from dataclasses import asdict, dataclass
import argparse
import hashlib
import json
from pathlib import Path
import struct


class ChartError(ValueError):
    pass


@dataclass(frozen=True)
class Measure:
    count: int
    first: int
    tick: int


@dataclass(frozen=True)
class Note:
    flags: int
    kind: int
    measure: int
    subdivision: int
    lane: int
    duration: int
    unknown: int
    tick: int


@dataclass(frozen=True)
class Chart:
    identifier: int
    header_field: int
    measures: tuple[Measure, ...]
    notes: tuple[Note, ...]

    @classmethod
    def read(cls, data: bytes) -> Chart:
        if len(data) < 8:
            raise ChartError("Truncated chart header")
        identifier, count, field = struct.unpack_from("<IHH", data)
        table_size = len(data) - 8 - count * 12
        if table_size < 8 or table_size % 8:
            raise ChartError("Chart tables do not fit the observed layout")
        measure_count = table_size // 8
        if measure_count > 256:
            raise ChartError("Measure indices exceed their stored byte")
        measures = tuple(Measure(*struct.unpack_from("<HHI", data, 8 + i*8)) for i in range(measure_count))
        start = 8 + table_size
        notes = tuple(Note(*struct.unpack_from("<HBBBBHHH", data, start + i*12)) for i in range(count))
        consumed = 0
        previous_active_tick = -1
        for index, measure in enumerate(measures):
            if measure.first != consumed or measure.first + measure.count > count:
                raise ChartError("Noncontiguous note index or count outside note table")
            # Some original charts reset timing in unused trailing rows.
            # Only rows containing notes must maintain active-time order.
            if measure.count:
                if measure.tick < previous_active_tick:
                    raise ChartError("Active measure ticks are not ordered")
                previous_active_tick = measure.tick
            for note in notes[measure.first:measure.first+measure.count]:
                if note.measure != index or note.subdivision >= 16:
                    raise ChartError("Note does not belong to its measure or subdivision")
            consumed += measure.count
        if consumed != count:
            raise ChartError("Measure counts do not cover the note table")
        # Original measures group records by kind/lane, not globally by tick.
        # Keep that order exactly; test playback must sort its own events.
        return cls(identifier, field, measures, notes)

    def write(self) -> bytes:
        # Exact structural round-trip, including unknown flags and identifiers.
        output = bytearray(struct.pack("<IHH", self.identifier, len(self.notes), self.header_field))
        for m in self.measures:
            output.extend(struct.pack("<HHI", m.count, m.first, m.tick))
        for n in self.notes:
            output.extend(struct.pack("<HBBBBHHH", n.flags, n.kind, n.measure, n.subdivision, n.lane, n.duration, n.unknown, n.tick))
        return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="Original FMDX bundle")
    parser.add_argument("member", help="Exact .ksd member name")
    parser.add_argument("output", type=Path, help="New JSON analysis file")
    args = parser.parse_args()
    from fmdx_archive import FmdxArchive
    archive = FmdxArchive(args.archive)
    entry = next((e for e in archive.entries if e.path == args.member), None)
    if entry is None:
        raise ChartError("Chart member is absent")
    data = archive.read(entry)
    chart = Chart.read(data)
    if chart.write() != data:
        raise ChartError("Structural round-trip differs")
    if args.output.exists():
        raise ChartError("Output already exists; choose a new analysis file")
    args.output.write_text(json.dumps({"source": str(args.archive.resolve()), "member": args.member,
        "sha256": hashlib.sha256(data).hexdigest(), "layout": asdict(chart),
        "byte_exact_roundtrip": True, "timing_unit_runtime_verified": False,
        "button_mapping_runtime_verified": False}, indent=2) + "\n", encoding="utf-8")
    print(f"Read {len(chart.notes)} records and {len(chart.measures)} measures; exact bytes preserved. Runtime semantics remain unverified.")


if __name__ == "__main__":
    main()
