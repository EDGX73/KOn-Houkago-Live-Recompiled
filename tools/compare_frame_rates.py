"""Find guest values that still advance once per frame in the 60 FPS mode.

Inputs: two folders of consecutive RAM snapshots (see find_frame_steps.py) of
the same scene, one at 30 FPS (N snapshots) and one at 60 FPS (2N snapshots),
so both cover the same game time.  For values that move steadily, the net
change over that time is the same in both runs when the 60 FPS patch scales
them (ratio ~1) and doubles when they still advance once per frame (ratio ~2),
which makes that part of the game run twice as fast.
"""
from pathlib import Path
import argparse
import numpy as np

BASE=0x08800000

def load(folder):
    return np.stack([np.fromfile(f,dtype='<u4') for f in sorted(Path(folder).glob('frame_*.bin'))])

def main():
    parser=argparse.ArgumentParser();parser.add_argument('rate30');parser.add_argument('rate60');parser.add_argument('--factor',type=float,default=2.0,help='frames of the fast run per 30 FPS frame')
    args=parser.parse_args()
    a=load(args.rate30);b=load(args.rate60);k=args.factor;n=min(len(a)-1,int((len(b)-1)/k));m=int(round(n*k))
    for kind in ('i4','f4'):
        with np.errstate(all='ignore'):
            x=a[:n+1].view('<'+kind).astype(np.float64);y=b[:m+1].view('<'+kind).astype(np.float64)
            d30=x[n]-x[0];d60=y[m]-y[0]
            steady30=(np.sign(np.diff(x,axis=0))==np.sign(d30)).all(0)
            steady60=((np.sign(np.diff(y,axis=0))==np.sign(d60))|(np.diff(y,axis=0)==0)).all(0)
            ok=np.isfinite(x).all(0)&np.isfinite(y).all(0)&(np.abs(x).max(0)<1e6)&(d30!=0)&(d60!=0)&steady30&steady60
            if kind=='f4':ok&=(np.abs(x).max(0)>1e-3)&(np.abs(d30)>np.abs(x[0])*1e-5)
            ratio=d60/d30
        same=ok&(ratio>0.75)&(ratio<1.33);double=ok&(ratio>1.6)&(ratio<2.5)
        print(f'{kind}: steady={int(ok.sum())} scaled={int(same.sum())} double={int(double.sum())} other={int((ok&~same&~double).sum())}')
        for i in np.nonzero(double)[0]:
            print(f'  0x{BASE+4*int(i):08X} v30={x[0,i]:<12.6g} d30={d30[i]:<10.4g} d60={d60[i]:<10.4g} ratio={ratio[i]:.2f}')

if __name__=='__main__':main()
