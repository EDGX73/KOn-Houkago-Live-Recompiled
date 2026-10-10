"""Decode/encode K-On GBIX/UVRT textures (.uvr) used for 2D sprites and text.

Supported here: palette datatype 3 (ABGR8888) with indexed8 (0x8A/0x8C) or
indexed4 (0x86/0x88) PSP-swizzled pixels -- the forms used by the menus.
See docs/ASSET_FORMATS.md.  `decode` writes a PNG; `encode` rebuilds a .uvr
from an edited PNG, keeping the original header and palette layout (the PNG
colours must exist in the original palette, or a new palette is built when
the image has at most the original palette size).
"""
from __future__ import annotations
import argparse,struct
from pathlib import Path
from PIL import Image

def parse(data:bytes):
    if data[:4]!=b'GBIX' or data[16:20]!=b'UVRT':raise ValueError('not a GBIX/UVRT texture')
    datatype,imagetype=data[0x18],data[0x19];width,height=struct.unpack_from('<HH',data,0x1C)
    if datatype!=3 or imagetype not in (0x86,0x88,0x8A,0x8C):raise ValueError(f'unsupported texture type {datatype:#x}/{imagetype:#x}')
    bits=8 if imagetype in (0x8A,0x8C) else 4;colors=1<<bits
    palette=[struct.unpack_from('<4B',data,0x20+i*4) for i in range(colors)]
    return width,height,bits,palette,0x20+colors*4

def unswizzle(raw:bytes,width:int,height:int,bits:int)->bytes:
    row=width*bits//8;out=bytearray(row*height);i=0
    for by in range(0,height,8):
        for bx in range(0,row,16):
            for y in range(8):
                start=(by+y)*row+bx;out[start:start+16]=raw[i:i+16];i+=16
    return bytes(out)

def swizzle(linear:bytes,width:int,height:int,bits:int)->bytes:
    row=width*bits//8;out=bytearray();
    for by in range(0,height,8):
        for bx in range(0,row,16):
            for y in range(8):
                start=(by+y)*row+bx;out+=linear[start:start+16]
    return bytes(out)

def decode(path:Path)->Image.Image:
    data=path.read_bytes();width,height,bits,palette,offset=parse(data)
    linear=unswizzle(data[offset:offset+width*height*bits//8],width,height,bits)
    indices=list(linear) if bits==8 else [v for b in linear for v in (b&15,b>>4)]
    image=Image.new('RGBA',(width,height));image.putdata([tuple(palette[i]) for i in indices]);return image

def encode(original:Path,png:Path,output:Path):
    data=bytearray(original.read_bytes());width,height,bits,palette,offset=parse(bytes(data))
    image=Image.open(png).convert('RGBA')
    if image.size!=(width,height):raise ValueError('edited image must keep the original size')
    pixels=list(image.getdata());lookup={tuple(c):i for i,c in reversed(list(enumerate(palette)))}
    missing=sorted({p for p in pixels if p not in lookup})
    if missing:
        used=sorted(set(pixels))
        if len(used)>len(palette):raise ValueError(f'{len(used)} colours do not fit a {len(palette)}-entry palette')
        palette=used+[(0,0,0,0)]*(len(palette)-len(used));lookup={c:i for i,c in enumerate(palette)}
        for i,c in enumerate(palette):struct.pack_into('<4B',data,0x20+i*4,*c)
    indices=[lookup[p] for p in pixels]
    linear=bytes(indices) if bits==8 else bytes(indices[i]|(indices[i+1]<<4) for i in range(0,len(indices),2))
    data[offset:offset+len(linear)]=swizzle(linear,width,height,bits);output.write_bytes(bytes(data))

def main():
    parser=argparse.ArgumentParser();sub=parser.add_subparsers(dest='command',required=True)
    d=sub.add_parser('decode');d.add_argument('uvr',type=Path);d.add_argument('png',type=Path)
    e=sub.add_parser('encode');e.add_argument('original',type=Path);e.add_argument('png',type=Path);e.add_argument('output',type=Path)
    args=parser.parse_args()
    if args.command=='decode':decode(args.uvr).save(args.png)
    else:encode(args.original,args.png,args.output)

if __name__=='__main__':main()
