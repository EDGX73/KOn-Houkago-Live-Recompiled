"""Classify instrumented +/-1 memory updates from a game log.

A site is a per-frame timer when, for some guest address, it ran once per
logic frame for a run of consecutive frames (hits ~= frames, long max_run).
Prints those sites with their guest label so they can be reviewed and added to
the 60 FPS half-step patch.
"""
from pathlib import Path
import argparse,json,re,collections

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('log',type=Path);parser.add_argument('--min-run',type=int,default=15)
    args=parser.parse_args()
    sites=json.loads((ROOT/'analysis/frame_counter_candidates.json').read_text())
    rows=collections.defaultdict(list)
    for line in args.log.read_text(encoding='utf-8',errors='replace').splitlines():
        if not line.startswith('native_counter_site '):continue
        d=dict(kv.split('=',1) for kv in line.split()[1:])
        rows[int(d['id'])].append({k:(int(v,16) if k=='address' else int(v)) for k,v in d.items() if k!='id'})
    timers=[];other=[]
    for i,entries in rows.items():
        best=max(entries,key=lambda e:e['max_run'])
        per_frame=sum(e['hits'] for e in entries)/max(1,sum(e['frames'] for e in entries))
        row=(i,sites[i],len(entries),best['max_run'],per_frame,sum(e['frames'] for e in entries))
        (timers if best['max_run']>=args.min_run and per_frame<=1.25 else other).append(row)
    print(f'executed sites={len(rows)} of {len(sites)}; per-frame timers={len(timers)}')
    for i,s,addresses,run,per_frame,frames in sorted(timers,key=lambda r:-r[3]):
        print(f"  id={i:3} unit={s['unit']} L_{s['label']} line={s['line']} off={s['offset']:<6} step={s['step']:+d} w={s['width']:2} addrs={addresses:<4} max_run={run:<6} hits/frame={per_frame:.2f}")
    print('not per-frame (loops or events):',len(other))

if __name__=='__main__':main()
