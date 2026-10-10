"""Create an original ELF fixture for compiled MADD and BREAK execution tests."""
import struct
from pathlib import Path
root = Path(__file__).resolve().parents[1] / 'work/instruction_fixture'
root.mkdir(parents=True, exist_ok=True)
buf = bytearray(0x230)
buf[:16] = b'\x7fELF\x01\x01\x01' + b'\0' * 9
struct.pack_into('<HHIIIIIHHHHHH', buf, 16, 2, 8, 1, 0x08804000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
struct.pack_into('<IIIIIIII', buf, 52, 1, 0x100, 0x08804000, 0x08804000, 0x130, 0x130, 5, 4)
# madd a0,a1; jr ra; nop. BREAK includes a nonzero code to verify reporting.
struct.pack_into('<III', buf, 0x100, 0x0085001C, 0x03E00008, 0)
struct.pack_into('<I', buf, 0x200, (0x12345 << 6) | 0xD)
struct.pack_into('<IIII', buf, 0x220, 0x50800002, (7 << 6) | 0xD, 0x03E00008, 0)
(root / 'fixture.elf').write_bytes(buf)
(root / 'functions.csv').write_text('name,address,size\nfixture_madd,0x08804000,0xC\nfixture_break,0x08804100,0x4\nfixture_delay_break,0x08804120,0x10\n')
