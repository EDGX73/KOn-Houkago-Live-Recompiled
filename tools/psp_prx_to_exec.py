"""PSP PRX -> relocated ELF32 ET_EXEC, for static analysis tools that only
accept executables (REA's PSP profile through Ghidra and ghidra-allegrex).

  python tools/psp_prx_to_exec.py analysis/executables/pkon.elf analysis/rea/pkon_exec_08804000.elf
  options: --base 0x08804000 (the load address the port uses)

The output is a derived analysis file of the player's own executable: it stays
in analysis/ (private) and is never published. What it contains:
  * every PT_LOAD segment, placed at base + p_vaddr and relocated exactly as
    PSPRecomp's Elf32Image::apply_relocations does when the game loads (R_MIPS_32,
    R_MIPS_26, HI16 paired with the next LO16 of the same segments, LO16);
    verified against it with prx_image_dump (src/prx_image_dump.cpp);
  * e_type ET_EXEC, the original e_machine/e_flags (0x10a23001: MIPS II,
    Allegrex, EABI32, NOREORDER) and e_entry = base + entry;
  * a MIPS ABI flags record, as PT_MIPS_ABIFLAGS and SHT_MIPS_ABIFLAGS, with the
    declaration PSPDEV's toolchain writes for Allegrex (ISA level 2 rev 0, 32-bit
    GPR and FPR, no CPR2, single-precision hard float, flags1=1). The original
    disc file has no such record: this declaration is added here, not observed;
  * the allocated sections at their relocated addresses (relocation sections are
    dropped); names are absent on the disc, so the module information, export,
    import and stub sections are named from the module information table
    (.rodata.sceModuleInfo, .lib.ent, .lib.stub, .rodata.sceNid, .sceStub.text)
    and the rest sec_<index>.
"""
import argparse, struct, sys
from pathlib import Path

PT_LOAD, PT_MIPS_ABIFLAGS = 1, 0x70000003
SHT_PROGBITS, SHT_NOBITS, SHT_STRTAB, SHT_PSP_REL, SHT_MIPS_ABIFLAGS = 1, 8, 3, 0x700000A0, 0x7000002A
SHF_ALLOC = 2
ET_EXEC, ET_SCE_PRX = 2, 0xFFA0
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
# Elf_MIPS_ABIFlags_v0: version, isa_level, isa_rev, gpr_size, cpr1_size, cpr2_size,
# fp_abi, isa_ext, ases, flags1, flags2 (AFL_REG_32 = 1, Val_GNU_MIPS_ABI_FP_SINGLE = 2)
ABIFLAGS = struct.pack('<HBBBBBBIIII', 0, 2, 0, 1, 1, 0, 2, 0, 0, 1, 0)

class Prx:
    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise SystemExit('not a little-endian ELF32 file')
        (self.type, self.machine, _, self.entry, phoff, shoff, self.flags, _, phes, phn, shes, shn,
         self.shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
        if self.type != ET_SCE_PRX or self.machine != 8:
            raise SystemExit(f'not a PSP PRX (e_type 0x{self.type:04X}, e_machine {self.machine})')
        self.data = data
        self.segments = [struct.unpack_from('<8I', data, phoff + i * phes) for i in range(phn)]
        self.sections = [struct.unpack_from('<10I', data, shoff + i * shes) for i in range(shn)]

    def relocated_image(self, base):
        """{segment index: bytearray} after PSPRecomp's relocation rules."""
        loads = {i: s for i, s in enumerate(self.segments) if s[0] == PT_LOAD}
        image = {i: bytearray(self.data[s[1]:s[1] + s[4]]) + bytearray(max(0, s[5] - s[4])) for i, s in loads.items()}
        addresses = [base + s[2] for s in self.segments]  # by program header index
        stats = dict(total=0, r32=0, r26=0, hi16=0, lo16=0, invalid=0, unsupported=0)
        for sec in self.sections:
            if sec[1] != SHT_PSP_REL:
                continue
            rels = [struct.unpack_from('<II', self.data, sec[4] + 8 * k) for k in range(sec[5] // 8)]
            original, valid = [0] * len(rels), [True] * len(rels)
            for k, (offset, info) in enumerate(rels):
                seg, kind = (info >> 8) & 0xFF, info & 0x0F
                if seg not in image or offset + 4 > len(image[seg]) or (offset & 3 and kind != R_MIPS_32):
                    valid[k] = False
                    stats['invalid'] += 1
                    continue
                original[k] = struct.unpack_from('<I', image[seg], offset)[0]
            for k, (offset, info) in enumerate(rels):
                stats['total'] += 1
                if not valid[k]:
                    continue
                kind, seg, target = info & 0x0F, (info >> 8) & 0xFF, (info >> 16) & 0xFF
                if target >= len(addresses):
                    stats['invalid'] += 1
                    continue
                to, op = addresses[target], original[k]
                if kind == R_MIPS_32:
                    op = (op + to) & 0xFFFFFFFF; stats['r32'] += 1
                elif kind == R_MIPS_26:
                    op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (to >> 2)) & 0x03FFFFFF); stats['r26'] += 1
                elif kind == R_MIPS_HI16:
                    for j in range(k + 1, len(rels)):
                        jt = rels[j][1] & 0x0F
                        if jt == R_MIPS_HI16:
                            continue
                        if jt != R_MIPS_LO16 or (rels[j][1] >> 8) != (info >> 8) or not valid[j]:
                            continue
                        low = original[j] & 0xFFFF
                        value = (((op & 0xFFFF) << 16) + (low - 0x10000 if low & 0x8000 else low) + to) & 0xFFFFFFFF
                        op = (op & 0xFFFF0000) | (((value >> 16) + ((value >> 15) & 1)) & 0xFFFF)
                        break
                    else:
                        stats['invalid'] += 1
                        continue
                    stats['hi16'] += 1
                elif kind == R_MIPS_LO16:
                    op = (op & 0xFFFF0000) | ((op + to) & 0xFFFF); stats['lo16'] += 1
                else:
                    stats['unsupported'] += 1
                    continue
                struct.pack_into('<I', image[seg], offset, op)
        return image, stats

    def section_names(self, image, base):
        """Names from the module information table (section names are stripped on the disc)."""
        names = {}
        first = next((s for s in self.segments if s[0] == PT_LOAD), None)
        info_offset = first[3] & 0x7FFFFFFF if first else None
        by_offset = {sec[4]: i for i, sec in enumerate(self.sections) if sec[2] & SHF_ALLOC}
        by_addr = {base + sec[3]: i for i, sec in enumerate(self.sections) if sec[2] & SHF_ALLOC}
        if info_offset in by_offset:
            i = by_offset[info_offset]
            names[i] = '.rodata.sceModuleInfo'
            seg0 = next(k for k, s in enumerate(self.segments) if s[0] == PT_LOAD)
            rel = self.sections[i][3] - self.segments[seg0][2]
            gp, ent_top, ent_end, stub_top, stub_end = struct.unpack_from('<5I', image[seg0], rel + 32)
            for address, name in ((ent_top, '.lib.ent'), (stub_top, '.lib.stub')):
                if address in by_addr:
                    names.setdefault(by_addr[address], name)
            if stub_top in by_addr and stub_end > stub_top:
                # Each import stub entry: name, version, attribute, entry size, var count,
                # function count, NID table, stub table (the first entry gives the sections).
                nids, stubs = struct.unpack_from('<II', image[seg0], stub_top - base - self.segments[seg0][2] + 12)
                for address, name in ((nids, '.rodata.sceNid'), (stubs, '.sceStub.text')):
                    if address in by_addr:
                        names.setdefault(by_addr[address], name)
        return names

def convert(source, target, base):
    prx = Prx(source.read_bytes())
    image, stats = prx.relocated_image(base)
    names = prx.section_names(image, base)
    loads = [(i, s) for i, s in enumerate(prx.segments) if s[0] == PT_LOAD]
    phnum = len(loads) + 1
    out = bytearray(52 + 32 * phnum)
    placed = {}
    for i, s in loads:
        out += bytes(-len(out) % 16)
        placed[i] = len(out)
        out += image[i][:s[4]]
    out += bytes(-len(out) % 8)
    abi_offset = len(out)
    out += ABIFLAGS
    # Section headers: null, allocated sections, .MIPS.abiflags, .shstrtab.
    strtab, sections = bytearray(b'\0'), [bytes(40)]
    def name_index(name):
        strtab.extend(name.encode() + b'\0')
        return len(strtab) - len(name) - 1
    for i, sec in enumerate(prx.sections):
        nm, typ, flg, addr, off, size, link, info, align, entsize = sec
        if not flg & SHF_ALLOC or typ not in (SHT_PROGBITS, SHT_NOBITS):
            continue
        seg = next((k for k, s in loads if s[2] <= addr < s[2] + max(s[5], 1)), None)
        if seg is None:
            continue
        new_offset = placed[seg] + (addr - prx.segments[seg][2]) if typ == SHT_PROGBITS else 0
        sections.append(struct.pack('<10I', name_index(names.get(i, f'sec_{i}')), typ, flg, base + addr,
                                    new_offset, size, 0, 0, align, entsize))
    sections.append(struct.pack('<10I', name_index('.MIPS.abiflags'), SHT_MIPS_ABIFLAGS, 0, 0, abi_offset, 24, 0, 0, 8, 24))
    shstr_name = name_index('.shstrtab')
    shstr_offset = len(out)
    out += strtab
    sections.append(struct.pack('<10I', shstr_name, SHT_STRTAB, 0, 0, shstr_offset, len(strtab), 0, 0, 1, 0))
    out += bytes(-len(out) % 4)
    shoff = len(out)
    for header in sections:
        out += header
    struct.pack_into('<4sBBBB8xHHIIIIIHHHHHH', out, 0, b'\x7fELF', 1, 1, 1, 0, ET_EXEC, 8, 1, base + prx.entry,
                     52, shoff, prx.flags, 52, 32, phnum, 40, len(sections), len(sections) - 1)
    for n, (i, s) in enumerate(loads):
        address = base + s[2]
        struct.pack_into('<8I', out, 52 + 32 * n, PT_LOAD, placed[i], address, address, s[4], s[5], s[6], 16)
    struct.pack_into('<8I', out, 52 + 32 * len(loads), PT_MIPS_ABIFLAGS, abi_offset, 0, 0, 24, 24, 4, 8)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(out)
    return stats, names, loads

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('source', type=Path)
    p.add_argument('target', type=Path)
    p.add_argument('--base', type=lambda v: int(v, 0), default=0x08804000)
    p.add_argument('--dump-segments', type=Path, help='also write the relocated PT_LOAD bytes (for prx_image_dump comparisons)')
    a = p.parse_args()
    stats, names, loads = convert(a.source, a.target, a.base)
    if a.dump_segments:
        prx = Prx(a.source.read_bytes())
        image, _ = prx.relocated_image(a.base)
        a.dump_segments.write_bytes(b''.join(bytes(image[i][:s[4]]) for i, s in loads))
    print(f'{a.target}: ET_EXEC at 0x{a.base:08X}; relocations {stats}; named sections: {sorted(set(names.values()))}')
    return 1 if stats['invalid'] or stats['unsupported'] else 0

if __name__ == '__main__':
    sys.exit(main())
