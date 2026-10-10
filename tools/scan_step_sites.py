"""List read-modify-write additions to guest memory in the generated code.

Generalises scan_frame_counters.py (+/-1 only) to any addition or subtraction,
integer or float, applied to a value loaded from the same address in the same
basic block:
    load  R <- [B+OFF]            (aot_loadN, or a float load)
    R2 = R + X  /  R2 = R - X     (possibly through several such steps)
    store [B+OFF] <- R2
These are every candidate for state advanced by a fixed amount per logic
frame (timers, positions, phases).  `apply` instruments them with
kon_step_site(id, address, old, new, kind, ra) and backs up the units; `restore` puts them
back.  The game reports, in speed-audit mode (alternating short and long
frames), how much each site moved the value on short and on long frames
(tools/classify_step_sites.py).
"""
from pathlib import Path
import json,re,shutil,sys

ROOT=Path(__file__).resolve().parents[1]
CORPUS=ROOT/'work/recompiled_corrected'
BACKUP=ROOT/'work/step_site_backup'
LIST=ROOT/'analysis/step_site_candidates.json'
DECL='extern "C" void kon_step_site(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t); // TEMP STEP DIAGNOSTIC\n'
ADDR=r'ctx\.gpr\[(\d+)\] \+ static_cast<std::uint32_t>\((-?\d+)\)'
ILOAD=re.compile(r'^\s*ctx\.set_gpr\((\d+), (?:static_cast<std::uint32_t>\(static_cast<std::int32_t>\(static_cast<std::int(?:8|16)_t>\()?rt\.memory\(\)\.aot_load(8|16|32)\('+ADDR+r'\)')
FLOAD=re.compile(r'^\s*ctx\.fpr\[(\d+)\] = std::bit_cast<float>\(rt\.memory\(\)\.aot_load32\('+ADDR+r'\)\);')
IADD=re.compile(r'^\s*ctx\.set_gpr\((\d+), ctx\.gpr\[(\d+)\] ([+-]) (?:ctx\.gpr\[(\d+)\]|static_cast<std::uint32_t>\((-?\d+)\))\);')
FADD=re.compile(r'^\s*ctx\.fpr\[(\d+)\] = ctx\.fpr\[(\d+)\] ([+-]) ctx\.fpr\[(\d+)\];')
ISTORE=re.compile(r'^\s*rt\.memory\(\)\.aot_store(8|16|32)\('+ADDR+r', (?:static_cast<std::uint(?:8|16)_t>\()?ctx\.gpr\[(\d+)\]')
FSTORE=re.compile(r'^\s*rt\.memory\(\)\.aot_store32\('+ADDR+r', std::bit_cast<std::uint32_t>\(ctx\.fpr\[(\d+)\]\)\);')
SETG=re.compile(r'^\s*ctx\.set_gpr\((\d+),')
SETF=re.compile(r'^\s*ctx\.fpr\[(\d+)\] =')
LABEL=re.compile(r'^L_([0-9A-F]{8}):')
import os
LOOSE=os.environ.get('KON_STEP_LOOSE')=='1'

def scan(path):
    lines=path.read_text(encoding='utf-8').split('\n');sites=[];label=None
    gl={};fl={}  # register -> (width, base, offset, added)
    for n,line in enumerate(lines,1):
        m=LABEL.match(line)
        if m:
            label=m.group(1)
            if not LOOSE:gl={};fl={}
            continue
        if LOOSE:
            # Loose mode: follow values across branches and labels (clamps after
            # the addition); forget them only at calls and function entries.
            if 'invoke_' in line or line.strip().startswith('ctx.pc =') or re.match(r'^\s*ctx\.set_gpr\(29, ctx\.gpr\[29\] \+ static_cast<std::uint32_t>\(-',line):gl={};fl={};continue
            if line.strip().startswith(('goto ','return;','if (','{ const bool')):continue
        elif line.strip().startswith(('goto ','return;','if (','{ const bool','ctx.pc =')) or 'invoke_' in line:gl={};fl={};continue
        m=ISTORE.match(line)
        if m:
            w,b,o,r=m.group(1),int(m.group(2)),int(m.group(3)),int(m.group(4));e=gl.get(r)
            if e and e[3] and b!=29:sites.append({'unit':path.stem[-4:],'line':n,'label':label,'kind':'int','width':int(w),'base':b,'offset':o,'same_address':e[0]==w and e[1]==b and e[2]==o})
            continue
        m=FSTORE.match(line)
        if m:
            b,o,r=int(m.group(1)),int(m.group(2)),int(m.group(3));e=fl.get(r)
            if e and e[3] and b!=29:sites.append({'unit':path.stem[-4:],'line':n,'label':label,'kind':'float','width':32,'base':b,'offset':o,'same_address':e[1]==b and e[2]==o})
            continue
        m=ILOAD.match(line)
        if m:
            d=int(m.group(1));gl={k:v for k,v in gl.items() if v[1]!=d};gl[d]=(m.group(2),int(m.group(3)),int(m.group(4)),False);continue
        m=FLOAD.match(line)
        if m:fl[int(m.group(1))]=('32',int(m.group(2)),int(m.group(3)),False);continue
        m=IADD.match(line)
        if m:
            d,a=int(m.group(1)),int(m.group(2));src=gl.get(a)
            if src is None and m.group(3)=='+' and m.group(4) is not None:src=gl.get(int(m.group(4)))
            gl={k:v for k,v in gl.items() if v[1]!=d}
            gl={k:v for k,v in gl.items() if v[1]!=d}
            gl[d]=(src[0],src[1],src[2],True) if src else (None,None,None,True)
            continue
        m=FADD.match(line)
        if m:
            d,a,b=int(m.group(1)),int(m.group(2)),int(m.group(4));src=fl.get(a) or (fl.get(b) if m.group(3)=='+' else None)
            fl[d]=(src[0],src[1],src[2],True) if src else (None,None,None,True)
            continue
        m=SETG.match(line)
        if m and LOOSE and not line.rstrip().endswith('? 1u : 0u);'):
            # min/max and moves keep a derived value derived
            used=[int(r) for r in re.findall(r'ctx\.gpr\[(\d+)\]',line.split(',',1)[1])]
            src=next((gl[r] for r in used if r in gl and gl[r][3]),None)
            if src:d=int(m.group(1));gl[d]=src;continue
        if m:d=int(m.group(1));gl.pop(d,None);gl={k:v for k,v in gl.items() if v[1]!=d};fl={k:v for k,v in fl.items() if v[1]!=d};continue
        m=SETF.match(line)
        if m:fl.pop(int(m.group(1)),None)
    return sites

def find():
    sites=[s for f in sorted(CORPUS.glob('generated_unit_*.cpp')) for s in scan(f)]
    LIST.write_text(json.dumps(sites,indent=1),encoding='utf-8')
    print(f'{len(sites)} stores of an addition -> {LIST.relative_to(ROOT)}',{k:sum(1 for s in sites if s['kind']==k) for k in ('int','float')},'same address:',sum(s['same_address'] for s in sites))

def apply():
    if BACKUP.exists():raise SystemExit('backup already exists: restore first')
    sites=json.loads(LIST.read_text());BACKUP.mkdir(parents=True);by_unit={}
    for i,s in enumerate(sites):by_unit.setdefault(s['unit'],[]).append((i,s))
    for unit,items in by_unit.items():
        path=CORPUS/f'generated_unit_{unit}.cpp';shutil.copy2(path,BACKUP/path.name)
        raw=path.read_bytes().decode('utf-8');crlf='\r\n' in raw;lines=raw.replace('\r\n','\n').split('\n')
        for i,s in sorted(items,key=lambda x:-x[1]['line']):
            n=s['line']-1;line=lines[n];indent=line[:len(line)-len(line.lstrip())]
            address=f"ctx.gpr[{s['base']}] + static_cast<std::uint32_t>({s['offset']})"
            if s['kind']=='float':new=re.search(r'std::bit_cast<std::uint32_t>\(ctx\.fpr\[(\d+)\]\)',line).group(0)
            else:new=f"ctx.gpr[{ISTORE.match(line).group(4)}]"
            old=f"rt.memory().aot_load{s['width']}({address})"
            lines.insert(n,f"{indent}kon_step_site({i}u, {address}, {old}, {new}, {1 if s['kind']=='float' else s['width']}u, ctx.gpr[31]); // TEMP STEP DIAGNOSTIC")
        text='\n'.join(lines);first=text.index('namespace psprecomp');text=text[:first]+DECL+text[first:]
        if crlf:text=text.replace('\n','\r\n')
        path.write_bytes(text.encode('utf-8'))
    print(f'instrumented {len(sites)} sites in {len(by_unit)} units')

def restore():
    # plain copy: a fresh timestamp makes the build recompile the restored units
    for f in BACKUP.glob("*.cpp"):shutil.copy(f,CORPUS/f.name)
    shutil.rmtree(BACKUP);print('restored')

if __name__=='__main__':{'find':find,'apply':apply,'restore':restore}[sys.argv[1]]()
