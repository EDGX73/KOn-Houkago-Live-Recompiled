"""Original synthetic PSP import-call fixture; bounded auto generation regression."""
from pathlib import Path
import json, struct, subprocess
root=Path(__file__).resolve().parents[1]
folder=root/'work/import_fixture';folder.mkdir(parents=True,exist_ok=True)
base=0x08804000
buf=bytearray(0x5A0)
buf[:16]=b'\x7fELF\x01\x01\x01'+b'\0'*9
struct.pack_into('<HHIIIIIHHHHHH',buf,16,2,8,1,base,52,0x500,0,52,32,1,40,4,3)
struct.pack_into('<IIIIIIII',buf,52,1,0x100,base,base,0x300,0x300,7,4)
# Preserve the caller RA and SP across a genuine generated import boundary.
words=[0x14A0000F,0,0x27BDFFF0,0xAFBF0000,0x0C000000|((base+0x100)>>2),0x2404002A,0x8FBF0000,0x03E00008,0x27BD0010]
struct.pack_into('<'+'I'*len(words),buf,0x100,*words)
# A genuine indirect call to the same import; target captured before delay slot.
indirect=[0x27BDFFF0,0xAFBF0000,0x3C190880,0x37394100,0x0320F809,0x2404002A,0x8FBF0000,0x03E00008,0x27BD0010]
struct.pack_into('<'+'I'*len(indirect),buf,0x140,*indirect)
struct.pack_into('<II',buf,0x200,0x03E00008,0)
struct.pack_into('<HBB28sIIIII',buf,0x300,0,0,1,b'original_fixture',0,0,0,base+0x240,base+0x254)
struct.pack_into('<IHHBBHII',buf,0x340,base+0x260,0x100,0,5,0,1,base+0x274,base+0x100)
buf[0x360:0x369]=b'ProbeLib\0'
struct.pack_into('<I',buf,0x374,0x12345678)
names=b'\0.text\0.rodata.sceModuleInfo\0.shstrtab\0';buf[0x480:0x480+len(names)]=names
struct.pack_into('<IIIIIIIIII',buf,0x528,1,1,6,base,0x100,0x108,0,0,4,0)
struct.pack_into('<IIIIIIIIII',buf,0x550,7,1,2,base+0x200,0x300,52,0,0,4,0)
struct.pack_into('<IIIIIIIIII',buf,0x578,27,3,0,0,0x480,len(names),0,0,1,0)
elf=folder/'fixture.elf';elf.write_bytes(buf)
result=subprocess.run([str(root/'build/framework/psp_recomp.exe'),str(elf),'--auto',str(folder/'generated'),hex(base),'128'],capture_output=True,text=True,timeout=20)
if result.returncode: raise RuntimeError(result.stdout+result.stderr)
report=json.loads((folder/'generated/auto_codegen_report.json').read_text())
if report['import_wrappers']!=1: raise RuntimeError('Synthetic import was not recognized')
print('PASS: bounded automatic generation with a real import call')
local=folder/'local';local.mkdir(exist_ok=True)
result=subprocess.run([str(root/'build/framework/psp_recomp.exe'),str(elf),'--auto',str(local),hex(base),'512'],capture_output=True,text=True,timeout=20)
if result.returncode: raise RuntimeError(result.stdout+result.stderr)
print('PASS: local direct and indirect import generation')
