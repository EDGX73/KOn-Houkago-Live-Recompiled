"""Classify the stores instrumented by scan_step_sites.py from a game log.

With --speed-audit the logic frames alternate 0.6 and 1.4 periods.  A store
that advances its value by game time moves it ~2.3x more on long frames; one
that advances it per logic frame moves it the same on both.  Prints the second
kind (long/short between --low and --high), with the generated label, so each
can be reviewed and patched in tools/apply_logic_rate_patch.py.
"""
from pathlib import Path
import argparse,json

ROOT=Path(__file__).resolve().parents[1]
# Allocator and display-list pointers (advance by bytes written, not by time).
IGNORE={'0x08B0261C','0x08B0222C','0x08AF2250','0x08AF2254','0x08AF2258','0x08B96304','0x08B96308','0x08B02220','0x08AFEF38','0x08AFEF20'}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('log',type=Path)
    parser.add_argument('--low',type=float,default=0.8);parser.add_argument('--high',type=float,default=1.25)
    parser.add_argument('--min-moved',type=int,default=30,help='frames that changed the value, short and long together')
    args=parser.parse_args()
    sites=json.loads((ROOT/'analysis/step_site_candidates.json').read_text())
    frames=0;rows=[]
    for line in args.log.read_text(encoding='utf-8',errors='replace').splitlines():
        if line.startswith('native_step_audit '):frames=int(line.split('frames=')[1]);continue
        if not line.startswith('native_step_site '):continue
        d=dict(kv.split('=',1) for kv in line.split()[1:])
        moved=[int(x) for x in d['moved'].split('+')];hits=[int(x) for x in d['hits'].split('+')]
        short,long_=float(d['short']),float(d['long'])
        # Total movement per frame type (both types have as many frames): a step
        # applied only on some frames (30 FPS timers) still counts as game time.
        total=(short*moved[0],long_*moved[1]);ratio=total[1]/total[0] if total[0] else float('inf')
        if int(d['address'],16)>=0x09F00000 or d['address'] in IGNORE:continue
        s=sites[int(d['id'])]
        rows.append((s,d,moved,hits,short,long_,ratio))
    per_frame=[r for r in rows if sum(r[2])>=args.min_moved and args.low<=r[6]<=args.high]
    timed=[r for r in rows if sum(r[2])>=args.min_moved and r[6]>1.6]
    print(f'audit frames={frames} sites that moved values={len(rows)} per-frame={len(per_frame)} game-time={len(timed)}')
    for s,d,moved,hits,short,long_,ratio in sorted(per_frame,key=lambda r:-sum(r[2])):
        print(f"  unit={s['unit']} L_{s['label']} line={s['line']} {s['kind']:5} w={s['width']:2} off={s['offset']:<5} same_addr={int(s['same_address'])} ra={d['ra']} moved={moved[0]}+{moved[1]} step={short:.6g}/{long_:.6g} ({ratio:.2f}) same={d['same']} at={d['address']} n={d['addresses']}")

if __name__=='__main__':main()
