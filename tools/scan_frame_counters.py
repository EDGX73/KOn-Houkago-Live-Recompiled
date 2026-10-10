"""List read-modify-write +/-1 updates of guest memory in the generated code.

Pattern (same basic block, same base register and offset):
    ctx.set_gpr(R, rt.memory().aot_loadN(ctx.gpr[B] + static_cast<std::uint32_t>(OFF)));
    ctx.set_gpr(S, ctx.gpr[R] + static_cast<std::uint32_t>(+1|-1));
    rt.memory().aot_storeN(ctx.gpr[B] + static_cast<std::uint32_t>(OFF), ...S...);
These are the candidates for per-frame timers authored in 30 FPS frames.
Output: one line per site with unit, the store line number and the enclosing
guest label, as JSON (analysis/frame_counter_candidates.json).
"""
from pathlib import Path
import json,re

ROOT=Path(__file__).resolve().parents[1]
CORPUS=ROOT/'work/recompiled_corrected'
LOAD=re.compile(r'^\s*ctx\.set_gpr\((\d+), (?:static_cast<std::uint32_t>\(static_cast<std::int32_t>\(static_cast<std::int(?:8|16)_t>\()?rt\.memory\(\)\.aot_load(8|16|32)\(ctx\.gpr\[(\d+)\] \+ static_cast<std::uint32_t>\((-?\d+)\)\)')
ADD=re.compile(r'^\s*ctx\.set_gpr\((\d+), ctx\.gpr\[(\d+)\] \+ static_cast<std::uint32_t>\((-?1)\)\);')
STORE=re.compile(r'^\s*rt\.memory\(\)\.aot_store(8|16|32)\(ctx\.gpr\[(\d+)\] \+ static_cast<std::uint32_t>\((-?\d+)\), (?:static_cast<std::uint(?:8|16)_t>\()?ctx\.gpr\[(\d+)\]')
LABEL=re.compile(r'^L_([0-9A-F]{8}):')

def scan(path):
    lines=path.read_text(encoding='utf-8').split('\n');sites=[];label=None
    loads={};adds={}
    for n,line in enumerate(lines,1):
        m=LABEL.match(line)
        if m:label=m.group(1);loads={};adds={};continue
        m=LOAD.match(line)
        if m:loads[int(m.group(1))]=(m.group(2),int(m.group(3)),int(m.group(4)));adds.pop(int(m.group(1)),None);continue
        m=ADD.match(line)
        if m and int(m.group(2)) in loads:adds[int(m.group(1))]=(loads[int(m.group(2))],int(m.group(3)));continue
        m=STORE.match(line)
        if m and int(m.group(4)) in adds:
            (width,base,offset),step=adds[int(m.group(4))]
            if width==m.group(1) and base==int(m.group(2)) and offset==int(m.group(3)) and base!=29:
                sites.append({'unit':path.stem[-4:],'line':n,'label':label,'width':int(width),'base':base,'offset':offset,'step':step})
        if line.strip().startswith(('goto ','return;','if (')):loads={};adds={}
    return sites

def main():
    sites=[s for f in sorted(CORPUS.glob('generated_unit_*.cpp')) for s in scan(f)]
    out=ROOT/'analysis/frame_counter_candidates.json'
    out.write_text(json.dumps(sites,indent=1),encoding='utf-8')
    print(f'{len(sites)} candidate +/-1 memory updates -> {out.relative_to(ROOT)}')
    print('by step:',{k:sum(1 for s in sites if s["step"]==k) for k in (1,-1)},'by width:',{k:sum(1 for s in sites if s["width"]==k) for k in (8,16,32)})

if __name__=='__main__':main()
