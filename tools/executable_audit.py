#!/usr/bin/env python3
"""Read-only PSP executable inventory; never decrypts or executes game code.

ABI references: pspsdk pspmoduleinfo.h; PPSSPP PSPElfTypes.h,
PrxDecrypter.h, sceKernelModule.cpp and ElfReader.cpp. This is an independent
file parser, not a port of the emulator loader or a complete disassembler.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

SOURCE_URLS = {
    "elf": "https://github.com/hrydgard/ppsspp/blob/master/Core/ELF/PSPElfTypes.h",
    "module_info": "https://github.com/pspdev/pspsdk/blob/master/src/user/pspmoduleinfo.h",
    "imports": "https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelModule.cpp",
    "relocations": "https://github.com/hrydgard/ppsspp/blob/master/Core/ELF/ElfReader.cpp",
    "encrypted_header": "https://github.com/hrydgard/ppsspp/blob/master/Core/ELF/PrxDecrypter.h",
    "instruction_patterns": "https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/MIPSTables.cpp",
}
REL_NAMES = {0: "NONE", 1: "16", 2: "32", 3: "REL32", 4: "26", 5: "HI16", 6: "LO16", 7: "GPREL16", 8: "LITERAL", 9: "GOT16", 10: "PC16", 11: "CALL16", 12: "GPREL32"}
RELB_NAMES = {0: "NONE", 1: "16", 2: "32", 3: "26", 4: "HI16", 5: "LO16", 6: "J26", 7: "JAL26"}

class FormatError(ValueError):
    pass


def span(data: bytes, offset: int, size: int) -> bytes:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise FormatError(f"file range outside image: offset=0x{offset:X}, size=0x{size:X}, file=0x{len(data):X}")
    return data[offset:offset + size]


def unpack(data: bytes, fmt: str, offset: int):
    return struct.unpack(fmt, span(data, offset, struct.calcsize(fmt)))


def ascii_z(data: bytes, offset: int, max_size: int = 256) -> str:
    end = min(len(data), offset + max_size)
    if offset < 0 or offset >= len(data):
        raise FormatError("string address outside file")
    nul = data.find(b"\x00", offset, end)
    if nul < 0:
        raise FormatError("string lacks terminator within bounded range")
    try:
        return data[offset:nul].decode("ascii")
    except UnicodeDecodeError as exc:
        raise FormatError("library/section name is not ASCII") from exc


def hex32(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:08X}"


class ElfImage:
    def __init__(self, data: bytes):
        self.data = data
        ident = span(data, 0, 16)
        if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 1:
            raise FormatError("requires ELF32 little-endian")
        fields = unpack(data, "<HHIIIIIHHHHHH", 16)
        names = ("type", "machine", "version", "entry", "phoff", "shoff", "flags", "ehsize", "phentsize", "phnum", "shentsize", "shnum", "shstrndx")
        self.header = dict(zip(names, fields))
        h = self.header
        if h["machine"] != 8:
            raise FormatError(f"ELF machine is {h['machine']}, not MIPS (8)")
        if h["ehsize"] < 52 or h["version"] != 1 or ident[6] != 1:
            raise FormatError("invalid ELF header size/version")
        if h["phnum"] and h["phentsize"] < 32:
            raise FormatError("program header stride smaller than ELF32 ABI")
        if h["shnum"] and h["shentsize"] < 40:
            raise FormatError("section header stride smaller than ELF32 ABI")
        span(data, h["phoff"], h["phnum"] * h["phentsize"])
        span(data, h["shoff"], h["shnum"] * h["shentsize"])
        self.segments = []
        self.sections = []
        for index in range(h["phnum"]):
            item = dict(zip(("type", "offset", "vaddr", "paddr", "filesz", "memsz", "flags", "align"), unpack(data, "<8I", h["phoff"] + index * h["phentsize"])))
            item["index"] = index
            span(data, item["offset"], item["filesz"])
            if item["type"] == 1 and item["filesz"] > item["memsz"]:
                raise FormatError("PT_LOAD filesz exceeds memsz")
            self.segments.append(item)
        for index in range(h["shnum"]):
            item = dict(zip(("name_index", "type", "flags", "addr", "offset", "size", "link", "info", "align", "entsize"), unpack(data, "<10I", h["shoff"] + index * h["shentsize"])))
            item["index"] = index
            if item["type"] not in (0, 8):
                span(data, item["offset"], item["size"])
            self.sections.append(item)
        if h["shstrndx"] == 0xFFFF:
            raise FormatError("extended section numbering not supported")
        names_data = b""
        if self.sections and h["shstrndx"]:
            if h["shstrndx"] >= len(self.sections):
                raise FormatError("section-name table index outside table")
            names_section = self.sections[h["shstrndx"]]
            names_data = span(data, names_section["offset"], names_section["size"])
        for section in self.sections:
            section["name"] = ascii_z(names_data, section["name_index"], len(names_data)) if names_data and section["name_index"] else ""

    def address(self, value: int, size: int = 1) -> int:
        matches = []
        for segment in self.segments:
            delta = value - segment["vaddr"]
            if segment["type"] == 1 and 0 <= delta <= segment["filesz"] and size <= segment["filesz"] - delta:
                matches.append(segment["offset"] + delta)
        if not matches:
            raise FormatError(f"virtual range {hex32(value)} + {size} not backed by a PT_LOAD file range")
        if len(set(matches)) != 1:
            raise FormatError(f"ambiguous virtual address mapping {hex32(value)}")
        return matches[0]

    def address_string(self, address: int) -> str:
        offset = self.address(address)
        for segment in self.segments:
            if segment["type"] == 1 and segment["offset"] <= offset < segment["offset"] + segment["filesz"]:
                return ascii_z(self.data, offset, min(256, segment["offset"] + segment["filesz"] - offset))
        raise FormatError("unmapped library name")

    def module_and_imports(self) -> dict:
        info_sections = [s for s in self.sections if s["name"] == ".rodata.sceModuleInfo"]
        if info_sections:
            offset = info_sections[0]["offset"]
            if info_sections[0]["size"] < 52:
                raise FormatError("module-info section smaller than 52 bytes")
            locator = ".rodata.sceModuleInfo section"
        elif self.segments and self.segments[0]["type"] == 1:
            offset = self.segments[0]["paddr"] & 0x7FFFFFFF
            # The PSP p_paddr encodes the module-info FILE offset, not a VA.
            first = self.segments[0]
            if not first["offset"] <= offset or offset + 52 > first["offset"] + first["filesz"]:
                raise FormatError("module-info p_paddr not backed by first PT_LOAD")
            locator = "first program header p_paddr & 0x7FFFFFFF"
        else:
            raise FormatError("module-info location unavailable")
        attr, minor, major, name_raw, gp, ent_top, ent_end, stub_top, stub_end = unpack(self.data, "<HBB28sIIIII", offset)
        name = name_raw.split(b"\x00", 1)[0].decode("ascii", "replace")
        result = {"locator": locator, "file_offset": offset, "attribute": attr, "version": [major, minor], "name": name, "gp": gp, "exports_top": ent_top, "exports_end": ent_end, "imports_top": stub_top, "imports_end": stub_end, "libraries": [], "function_count": 0, "variable_count": 0, "errors": []}
        if stub_end < stub_top:
            raise FormatError("module import table end precedes start")
        if stub_end == stub_top:
            return result
        self.address(stub_top, stub_end - stub_top)
        current = stub_top
        while current < stub_end:
            try:
                if stub_end - current < 20:
                    raise FormatError("final import header is truncated")
                off = self.address(current, 20)
                library_addr, version, flags, words, variables, funcs, nid_addr, stubs_addr = unpack(self.data, "<IHHBBHII", off)
                size = words * 4
                if words < 5 or size > stub_end - current:
                    raise FormatError(f"import entry has invalid word size {words}")
                self.address(current, size)
                library = {"name": self.address_string(library_addr), "entry_address": current, "entry_file_offset": off, "version": version, "flags": flags, "entry_words": words, "function_count": funcs, "variable_count": variables, "nid_address": nid_addr, "stub_address": stubs_addr, "functions": [], "variables": []}
                if funcs:
                    nids_off = self.address(nid_addr, funcs * 4)
                    self.address(stubs_addr, funcs * 8)
                    for index in range(funcs):
                        nid = unpack(self.data, "<I", nids_off + 4 * index)[0]
                        stub_va = stubs_addr + 8 * index
                        stub_off = self.address(stub_va, 8)
                        library["functions"].append({"nid": hex32(nid), "stub_address": stub_va, "stub_file_offset": stub_off, "stub_words": list(unpack(self.data, "<II", stub_off))})
                if variables:
                    if words < 6:
                        raise FormatError("variable imports require optional varData word")
                    var_addr = unpack(self.data, "<I", off + 20)[0]
                    var_off = self.address(var_addr, variables * 8)
                    for index in range(variables):
                        refs_addr, nid = unpack(self.data, "<II", var_off + index * 8)
                        library["variables"].append({"nid": hex32(nid), "relocation_list_address": refs_addr})
                result["libraries"].append(library)
                result["function_count"] += funcs
                result["variable_count"] += variables
                current += size
            except FormatError as exc:
                result["errors"].append({"entry_address": current, "reason": str(exc)})
                break  # Never invent another table entry after an invalid header.
        result["library_count"] = len(result["libraries"])
        result["table_complete"] = current == stub_end
        return result

    def symbol_tables(self) -> list:
        result = []
        for section in self.sections:
            if section["type"] not in (2, 11):
                continue
            stride = section["entsize"] or 16
            if stride < 16 or section["size"] % stride:
                raise FormatError("invalid ELF32 symbol-table stride/size")
            if section["link"] >= len(self.sections):
                raise FormatError("symbol string table outside section headers")
            strings = self.sections[section["link"]]
            if strings["type"] != 3:
                raise FormatError("symbol-table link does not name SHT_STRTAB")
            name_data = span(self.data, strings["offset"], strings["size"])
            symbols = []
            kinds = Counter()
            for local in range(0, section["size"], stride):
                name_index, value, size, info, other, shndx = unpack(self.data, "<IIIBBH", section["offset"] + local)
                name = ascii_z(name_data, name_index, len(name_data)) if name_index else ""
                kinds[str(info & 15)] += 1
                symbols.append({"name": name, "value": value, "size": size, "type": info & 15, "binding": info >> 4, "section_index": shndx})
            result.append({"section_index": section["index"], "count": len(symbols), "kinds": dict(kinds), "symbols": symbols})
        return result

    def relocations(self) -> list:
        tables = []
        seen = set()
        for source, items in (("section", self.sections), ("segment", self.segments)):
            for item in items:
                kind = item["type"]
                if kind not in (0x700000A0, 0x700000A1):
                    continue
                size = item["size"] if source == "section" else item["filesz"]
                key = (item["offset"], size, kind)
                if key in seen:
                    continue
                seen.add(key)
                table = {"source": source, "index": item["index"], "file_offset": item["offset"], "size": size, "kind": "PSP_REL_A" if kind == 0x700000A0 else "PSP_REL_B", "errors": []}
                raw = span(self.data, item["offset"], size)
                if kind == 0x700000A0:
                    types = Counter()
                    invalid_targets = 0
                    if len(raw) % 8:
                        table["errors"].append("Type-A table has trailing bytes")
                    for offset in range(0, len(raw) - 7, 8):
                        rel_offset, info = unpack(raw, "<II", offset)
                        rtype, write_segment, relative_segment = info & 15, (info >> 8) & 255, (info >> 16) & 255
                        types[REL_NAMES.get(rtype, f"UNKNOWN_{rtype}")] += 1
                        if write_segment >= len(self.segments) or self.segments[write_segment]["type"] != 1 or rel_offset + 4 > self.segments[write_segment]["memsz"]:
                            invalid_targets += 1
                        if relative_segment >= len(self.segments) or self.segments[relative_segment]["type"] != 1:
                            table["errors"].append(f"relative segment {relative_segment} is not PT_LOAD") if len(table["errors"]) < 10 else None
                    table.update({"records": len(raw) // 8, "types": dict(types), "invalid_write_targets": invalid_targets, "decoded": True})
                elif source == "segment":
                    table.update(self._relb(raw, item["index"]))
                else:
                    table.update({"decoded": False, "reason": "Type-B stream requires its program-header index for segment bit width"})
                tables.append(table)
        return tables

    def _relb(self, raw: bytes, program_index: int) -> dict:
        result = {"decoded": False, "errors": [], "records": 0}
        try:
            span(raw, 0, 4)
            flag_bits, type_bits = raw[2], raw[3]
            seg_bits = max(1, (max(1, program_index) - 1).bit_length())
            if not (1 <= flag_bits <= 8 and 1 <= type_bits <= 8 and flag_bits + type_bits + seg_bits < 16):
                raise FormatError("Type-B bit widths are invalid")
            pos = 4
            flag_size = span(raw, pos, 1)[0]
            if not flag_size:
                raise FormatError("empty Type-B flag table")
            flags = span(raw, pos, flag_size)
            pos += flag_size
            type_size = span(raw, pos, 1)[0]
            if not type_size:
                raise FormatError("empty Type-B type table")
            types = span(raw, pos, type_size)
            pos += type_size
            counts = Counter()
            invalid_targets = 0
            write_segment, rel_base = 0, 0
            while pos < len(raw):
                cmd = unpack(raw, "<H", pos)[0]
                pos += 2
                flag_index = cmd & ((1 << flag_bits) - 1)
                type_index = (cmd >> (flag_bits + seg_bits)) & ((1 << type_bits) - 1)
                segment = (cmd >> flag_bits) & ((1 << seg_bits) - 1)
                if flag_index >= len(flags) or type_index >= len(types):
                    raise FormatError("Type-B table index outside bounds")
                flag, rtype = flags[flag_index], types[type_index]
                mode = flag & 6
                if not flag & 1:
                    write_segment = segment
                    if mode == 0:
                        rel_base = cmd >> (flag_bits + seg_bits)
                    elif mode == 4:
                        rel_base = unpack(raw, "<I", pos)[0]
                        pos += 4
                    else:
                        raise FormatError("invalid Type-B base command width")
                    continue
                shift = flag_bits + seg_bits + type_bits
                signed_cmd = cmd - 0x10000 if cmd & 0x8000 else cmd
                if mode == 0:
                    rel_base += signed_cmd >> shift
                elif mode == 2:
                    low = unpack(raw, "<H", pos)[0]
                    pos += 2
                    rel_base += ((signed_cmd >> shift) << 16) | low
                elif mode == 4:
                    rel_base = unpack(raw, "<I", pos)[0]
                    pos += 4
                else:
                    raise FormatError("invalid Type-B relocation command width")
                low_mode = flag & 0x38
                if low_mode == 0x10:
                    span(raw, pos, 2)
                    pos += 2
                elif low_mode not in (0, 8):
                    raise FormatError("invalid Type-B low16 extension width")
                counts[RELB_NAMES.get(rtype, f"UNKNOWN_{rtype}")] += 1
                result["records"] += 1
                if write_segment >= len(self.segments) or self.segments[write_segment]["type"] != 1 or rel_base < 0 or rel_base + 4 > self.segments[write_segment]["memsz"]:
                    invalid_targets += 1
                if segment >= len(self.segments) or self.segments[segment]["type"] != 1:
                    invalid_targets += 1
            result.update({"decoded": True, "flag_bits": flag_bits, "type_bits": type_bits, "segment_bits": seg_bits, "types": dict(counts), "invalid_targets": invalid_targets, "bytes_consumed": pos})
        except FormatError as exc:
            result["errors"].append(str(exc))
        return result

    def instruction_patterns(self) -> dict:
        regions = [(s["offset"], s["size"], s["addr"], s["name"]) for s in self.sections if s["flags"] & 4 and s["type"] == 1]
        method = "SHF_EXECINSTR sections"
        if not regions:
            regions = [(s["offset"], s["filesz"], s["vaddr"], f"PT_LOAD[{s['index']}]") for s in self.segments if s["type"] == 1 and s["flags"] & 1]
            method = "executable PT_LOAD fallback; includes possible data"
        count = Counter({key: 0 for key in ("syscall", "break", "vfpu_family_candidate", "branch_likely", "indirect_jump_or_call", "cop1_fpu_family_candidate", "cache_or_atomic")})
        samples = {}
        scanned = set()
        vfpu_primary = {24, 25, 27, 50, 52, 53, 54, 55, 58, 60, 61, 62, 63}
        for offset, size, va, name in regions:
            for local in range(0, size - 3, 4):
                file_offset = offset + local
                if file_offset in scanned:
                    continue
                scanned.add(file_offset)
                word = unpack(self.data, "<I", file_offset)[0]
                opcode = word >> 26
                keys = []
                if word == 0:
                    keys.append("zero_word_or_nop")
                if opcode == 0 and word & 63 == 12:
                    keys.append("syscall")
                if opcode == 0 and word & 63 == 13:
                    keys.append("break")
                if opcode == 0 and word & 63 in (8, 9):
                    keys.append("indirect_jump_or_call")
                if opcode in (20, 21, 22, 23) or (opcode == 1 and ((word >> 16) & 31) in (2, 3, 18, 19)):
                    keys.append("branch_likely")
                if opcode in vfpu_primary or opcode == 18:
                    keys.append("vfpu_family_candidate")
                if opcode == 55:
                    sub = (word >> 23) & 7
                    keys.append(("vpfxs", "vpfxs", "vpfxt", "vpfxt", "vpfxd", "vpfxd", "viim", "vfim")[sub])
                if opcode in (17, 49, 57):
                    keys.append("cop1_fpu_family_candidate")
                if opcode in (47, 48, 56):
                    keys.append("cache_or_atomic")
                for key in keys:
                    count[key] += 1
                    if len(samples.setdefault(key, [])) < 12:
                        samples[key].append({"address": hex32(va + local), "file_offset": file_offset, "word": hex32(word), "region": name})
        return {"method": method, "word_count": len(scanned), "counts": dict(count), "samples": samples, "limits": "Opcode patterns only; executable ranges may contain literal pools. Counts do not prove reachability or execution and are not full VFPU decoding."}


def useful_strings(data: bytes, limit: int) -> dict:
    found = []
    pattern = re.compile(rb"[\x20-\x7E]{6,}")
    keywords = re.compile(r"sce[A-Z]|disc0:|ms0:|flash0:|\.prx|\.pmf|\.at3|\.bin|\.pak|\.dat|\.png|\.tm2|\.gim|\.pmo|\.pmp|\.ogg|\.wav|\.fnt|\.font|error|failed|assert|language|english|japanese", re.I)
    total = 0
    for match in pattern.finditer(data):
        value = match.group().decode("ascii")
        if keywords.search(value):
            total += 1
            if len(found) < limit:
                found.append({"file_offset": match.start(), "text": value[:512], "truncated": len(value) > 512})
    return {"encoding": "ASCII only", "matching_count": total, "sample_count": len(found), "samples": found, "limits": "No Shift-JIS/UTF-16 localization extraction; accidental strings in encrypted data are not evidence of plaintext."}


def audit(path: Path, string_limit: int = 200) -> dict:
    data = path.read_bytes()
    result = {"path": str(path.resolve()), "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "magic_hex": data[:16].hex(), "sources": SOURCE_URLS, "warnings": []}
    if not data:
        result.update({"format": "EMPTY", "plaintext_code_available": False})
    elif not any(data):
        result.update({"format": "ZERO_FILLED_PLACEHOLDER", "plaintext_code_available": False, "nonzero_bytes": 0, "reason": "Every byte is zero; not an executable image."})
    elif data[:4] == b"~PSP":
        result.update({"format": "PSP_ENCRYPTED_CONTAINER", "plaintext_code_available": False, "reason": "~PSP container; no ELF code, imports or instruction counts are inferred from ciphertext."})
        if len(data) >= 0x150:
            result["psp_header"] = {"module_name": data[10:38].split(b"\x00", 1)[0].decode("ascii", "replace"), "attribute": unpack(data, "<H", 4)[0], "compression_attribute": unpack(data, "<H", 6)[0], "version": [data[9], data[8]], "segments": data[0x27], "declared_elf_size": unpack(data, "<I", 0x28)[0], "declared_container_size": unpack(data, "<I", 0x2C)[0], "entry": hex32(unpack(data, "<I", 0x30)[0]), "module_info_offset": hex32(unpack(data, "<I", 0x34)[0]), "devkit_version": hex32(unpack(data, "<I", 0x78)[0]), "decrypt_mode": hex32(unpack(data, "<I", 0x7C)[0]), "tag": hex32(unpack(data, "<I", 0xD0)[0])}
        else:
            result["warnings"].append("~PSP header shorter than 0x150 bytes")
    elif data[:4] == b"~SCE":
        result.update({"format": "SCE_WRAPPED_CONTAINER", "plaintext_code_available": False, "reason": "Outer ~SCE container; this tool does not unwrap or decrypt it."})
    elif data[:4] == b"\x7fELF":
        result["format"] = "ELF"
        try:
            image = ElfImage(data)
            result.update({"format": "ELF32LE_MIPS_PRX" if image.header["type"] == 0xFFA0 else "ELF32LE_MIPS", "plaintext_code_available": True, "elf": {"header": image.header, "sections": image.sections, "segments": image.segments, "relocations": image.relocations(), "symbol_tables": image.symbol_tables()}, "instruction_patterns": image.instruction_patterns()})
            try:
                result["module_info"] = image.module_and_imports()
            except FormatError as exc:
                result["warnings"].append(f"Module/import audit: {exc}")
            result["useful_strings"] = useful_strings(data, string_limit)
        except (FormatError, struct.error) as exc:
            result.update({"plaintext_code_available": False, "parse_error": str(exc)})
    else:
        result.update({"format": "UNKNOWN", "plaintext_code_available": False, "reason": "No recognized signature at offset zero."})
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path, help="Extracted EBOOT.BIN, BOOT.BIN, PRX or ELF; inputs are never modified")
    parser.add_argument("--out", type=Path, help="JSON report path; otherwise writes JSON to stdout")
    parser.add_argument("--string-limit", type=int, default=200)
    args = parser.parse_args()
    reports = []
    for path in args.files:
        try:
            reports.append(audit(path, max(0, args.string_limit)))
        except OSError as exc:
            parser.error(f"Cannot read {path}: {exc}")
    payload = {"schema": "kon-executable-audit-v1", "read_only": True, "files": reports}
    encoded = json.dumps(payload, ensure_ascii=False, indent=2) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")
        for item in reports:
            print(f"{Path(item['path']).name}: {item['format']}; plaintext={item['plaintext_code_available']}; {item['size']} bytes")
        print(f"Report: {args.out.resolve()}")
    else:
        sys.stdout.write(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
