"""Find guest values that advance by a constant step every logic frame.

Input: a folder of consecutive user-RAM snapshots written by
`kon_boot_probe --ram-snapshots <dir> <start_ms> <count>` (base 0x08800000).
Reports 32-bit words whose per-frame delta is constant and non-zero, both as
integers and as floats. These are the candidates for the 60 FPS step patch.
"""
from pathlib import Path
import argparse,collections
import numpy as np

BASE=0x08800000

def main():
    parser=argparse.ArgumentParser();parser.add_argument('folder',type=Path);parser.add_argument('--limit',type=int,default=400)
    args=parser.parse_args()
    files=sorted(args.folder.glob('frame_*.bin'))
    if len(files)<4:raise SystemExit('need at least 4 consecutive snapshots')
    words=np.stack([np.fromfile(f,dtype='<u4') for f in files])
    ints=words.astype(np.int64);d=np.diff(ints,axis=0)
    const=(d==d[0]).all(axis=0)&(d[0]!=0)
    # Ignore code (text) region: values there never change anyway.
    with np.errstate(all='ignore'):floats=words.view('<f4').astype(np.float64)
    with np.errstate(all='ignore'):
        fd=np.diff(floats,axis=0)
        fconst=np.isfinite(fd).all(axis=0)&(np.abs(fd-fd[0])<=np.abs(fd[0])*1e-3+1e-7).all(axis=0)&(fd[0]!=0)&(np.abs(floats[0])<1e7)&(np.abs(fd[0])<1e4)&(np.abs(fd[0])>1e-5)&((floats[0]==0)|(np.abs(floats[0])>1e-6))
    fconst&=~const
    rows=[]
    for i in np.nonzero(const)[0]:rows.append((BASE+4*int(i),'int',int(d[0,i]),int(ints[0,i])))
    for i in np.nonzero(fconst)[0]:rows.append((BASE+4*int(i),'float',float(fd[0,i]),float(floats[0,i])))
    rows.sort()
    steps=collections.Counter((k,round(s,4)) for _,k,s,_ in rows)
    print(f'snapshots={len(files)} constant_int={int(const.sum())} constant_float={int(fconst.sum())}')
    print('most common steps:',', '.join(f'{k}:{s}x{n}' for (k,s),n in steps.most_common(16)))
    for address,kind,step,value in rows[:args.limit]:print(f'0x{address:08X} {kind:5} step={step:<12g} first={value:g}')

if __name__=='__main__':main()
