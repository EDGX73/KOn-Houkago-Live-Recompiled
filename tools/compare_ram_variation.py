"""Compare per-word RAM variation of a 30 FPS run with a PC-rate run.

Inputs: two files written by `kon_boot_probe --ram-variation file start_ms
duration_ms` for the same scene (same save and inputs).  For every user-RAM
word the variation per real second (sum of |delta| as integer and as float) is
compared: a value the game advances by elapsed time varies at the same rate in
both runs; a value still advanced per logic frame varies `fps/30` times faster.
Prints the words whose rate grew by more than --ratio, grouped into runs of
neighbouring addresses, with the comma-separated address list ready for
PSPRECOMP_WATCH_WRITE.
"""
from pathlib import Path
import argparse
import numpy as np

BASE=0x08800000

def load(path):
    raw=path.read_bytes();frames,duration,words=np.frombuffer(raw[:24],dtype='<u8')
    words=int(words);at=24
    changes=np.frombuffer(raw,dtype='<u4',count=words,offset=at);at+=4*words
    ints=np.frombuffer(raw,dtype='<f8',count=words,offset=at);at+=8*words
    floats=np.frombuffer(raw,dtype='<f8',count=words,offset=at)
    seconds=duration/1e6
    return {'frames':int(frames),'seconds':seconds,'changes':changes/seconds,'int':ints/seconds,'float':floats/seconds}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('base',type=Path);parser.add_argument('fast',type=Path)
    parser.add_argument('--ratio',type=float,default=1.8);parser.add_argument('--min-changes',type=float,default=4.0,help='changes per second in the 30 FPS run')
    parser.add_argument('--limit',type=int,default=80)
    args=parser.parse_args()
    a,b=load(args.base),load(args.fast)
    fps_a,fps_b=a['frames']/a['seconds'],b['frames']/b['seconds']
    print(f'base {fps_a:.1f} FPS over {a["seconds"]:.1f} s; fast {fps_b:.1f} FPS over {b["seconds"]:.1f} s; per-frame values would vary {fps_b/fps_a:.2f}x faster')
    active=a['changes']>=args.min_changes
    # Float view when the word reads as a float that moves (small magnitudes),
    # integer view otherwise.
    use_float=(a['float']>0)&(a['float']<a['int']*1e-3)
    rate_a=np.where(use_float,a['float'],a['int']);rate_b=np.where(use_float,b['float'],b['int'])
    with np.errstate(all='ignore'):ratio=rate_b/rate_a
    expected=fps_b/fps_a
    # Per-frame values land near the frame-rate ratio; far larger ratios are
    # scratch buffers (vertex data, garbage floats).
    hit=active&(rate_a>0)&(ratio>=args.ratio)&(ratio<=expected*1.6)
    idx=np.nonzero(hit)[0]
    print(f'active words={int(active.sum())} faster by >={args.ratio}x: {len(idx)}')
    # Group neighbouring words (structures) together.
    groups=[];
    for i in idx:
        if groups and i-groups[-1][-1]<=8:groups[-1].append(i)
        else:groups.append([i])
    for g in groups[:args.limit]:
        cells=', '.join(f'0x{BASE+4*i:08X}:{"f" if use_float[i] else "i"} {ratio[i]:.1f}x ({a["changes"][i]:.0f}/s)' for i in g[:6])
        print(f'  {len(g):3} words  {cells}{" ..." if len(g)>6 else ""}')
    print('stayed (ratio<1.3):',int((active&(rate_a>0)&(ratio<1.3)).sum()))
    first=[g[0] for g in groups]
    print('watch:',','.join(f'0x{BASE+4*i:08X}' for i in first[:64]))

if __name__=='__main__':main()
