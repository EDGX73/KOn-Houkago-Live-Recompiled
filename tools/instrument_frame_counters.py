"""Temporarily instrument the +/-1 memory updates found by scan_frame_counters.py.

`apply` backs up every touched unit to work/frame_counter_backup and inserts
`kon_counter_site(id, address)` before each candidate store; the game then
reports, per site, how many logic frames executed it (see boot_probe.cpp,
native_counter_site lines).  `restore` puts the original units back.
"""
from pathlib import Path
import json,shutil,sys

ROOT=Path(__file__).resolve().parents[1]
CORPUS=ROOT/'work/recompiled_corrected'
BACKUP=ROOT/'work/frame_counter_backup'
DECL='extern "C" void kon_counter_site(std::uint32_t, std::uint32_t); // TEMP COUNTER DIAGNOSTIC\n'

def apply():
    if BACKUP.exists():raise SystemExit('backup already exists: restore first')
    sites=json.loads((ROOT/'analysis/frame_counter_candidates.json').read_text())
    BACKUP.mkdir(parents=True)
    by_unit={}
    for i,s in enumerate(sites):by_unit.setdefault(s['unit'],[]).append((i,s))
    for unit,items in by_unit.items():
        path=CORPUS/f'generated_unit_{unit}.cpp';shutil.copy2(path,BACKUP/path.name)
        raw=path.read_bytes().decode('utf-8');crlf='\r\n' in raw;lines=raw.replace('\r\n','\n').split('\n')
        for i,s in sorted(items,key=lambda x:-x[1]['line']):
            n=s['line']-1;indent=lines[n][:len(lines[n])-len(lines[n].lstrip())]
            lines.insert(n,f"{indent}kon_counter_site({i}u, ctx.gpr[{s['base']}] + static_cast<std::uint32_t>({s['offset']})); // TEMP COUNTER DIAGNOSTIC")
        text='\n'.join(lines);first=text.index('namespace psprecomp');text=text[:first]+DECL+text[first:]
        if crlf:text=text.replace('\n','\r\n')
        path.write_bytes(text.encode('utf-8'))
    print(f'instrumented {len(sites)} sites in {len(by_unit)} units')

def restore():
    # plain copy: a fresh timestamp makes the build recompile the restored units
    for f in BACKUP.glob("*.cpp"):shutil.copy(f,CORPUS/f.name)
    shutil.rmtree(BACKUP);print('restored')

if __name__=='__main__':{'apply':apply,'restore':restore}[sys.argv[1]]()
