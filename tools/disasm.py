"""Readable Allegrex disassembly of the game executable (diagnostic).

Usage: python tools/disasm.py ADDRESS COUNT
Wraps build/framework/dump_function.exe (PSPRecomp decoder) and prints
register names, immediates and call/branch targets.
"""
import subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
REG = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
       's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']

def main():
    address, count = sys.argv[1], sys.argv[2]
    out = subprocess.run([str(ROOT / 'build/framework/dump_function.exe'), str(ROOT / 'analysis/executables/pkon.elf'), address, count],
                         capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        pc, word, mnem = parts[0], parts[1], parts[2]
        f = dict(p.split('=') for p in parts[3:])
        rs, rt, rd = REG[int(f['rs'])], REG[int(f['rt'])], REG[int(f['rd'])]
        imm = int(f['imm'])
        simm = imm - 0x10000 if imm >= 0x8000 and imm < 0x10000 else imm
        text = mnem
        if 'target' in f:
            text += ' ' + f['target']
        elif mnem in ('lw', 'sw', 'lb', 'lbu', 'sb', 'lh', 'lhu', 'sh', 'lwc1', 'swc1'):
            text += f' {rt}, {simm}({rs})'
        elif mnem in ('addiu', 'addi', 'slti', 'sltiu'):
            text += f' {rt}, {rs}, {simm}'
        elif mnem in ('andi', 'ori', 'xori'):
            text += f' {rt}, {rs}, 0x{imm:x}'
        elif mnem == 'lui':
            text += f' {rt}, 0x{imm:x}'
        elif mnem in ('beq', 'bne', 'beql', 'bnel'):
            target = int(pc, 16) + 4 + (simm << 2)
            text += f' {rs}, {rt}, 0x{target:08X}'
        elif mnem.startswith('b'):
            target = int(pc, 16) + 4 + (simm << 2)
            text += f' {rs}, 0x{target:08X}'
        elif mnem in ('sll', 'srl', 'sra'):
            text += f' {rd}, {rt}, {f["sa"]}'
        elif mnem in ('jr', 'jalr'):
            text += f' {rs}'
        elif mnem in ('mfhi', 'mflo'):
            text += f' {rd}'
        elif mnem in ('mult', 'multu', 'div', 'divu'):
            text += f' {rs}, {rt}'
        else:
            text += f' {rd}, {rs}, {rt}'
        print(f'{pc}  {text}')

if __name__ == '__main__':
    main()
