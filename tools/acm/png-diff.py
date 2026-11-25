"""Compare the non-interlaced RGB/RGBA PNGs written by the native test tools."""
import argparse,struct,zlib
from pathlib import Path
def read(path):
    blob=Path(path).read_bytes();assert blob[:8]==b'\x89PNG\r\n\x1a\n'
    at=8;packed=bytearray();w=h=channels=0
    while at<len(blob):
        n=struct.unpack_from('>I',blob,at)[0];kind=blob[at+4:at+8];data=blob[at+8:at+8+n];at+=12+n
        if kind==b'IHDR':
            w,h,depth,colour,compression,filtering,interlace=struct.unpack('>IIBBBBB',data)
            assert depth==8 and colour in (2,6) and not interlace
            channels=3 if colour==2 else 4
        elif kind==b'IDAT':packed.extend(data)
    raw=zlib.decompress(packed);stride=w*channels;out=bytearray(h*stride);at=0
    for y in range(h):
        mode=raw[at];at+=1;row=y*stride
        for x in range(stride):
            left=out[row+x-channels] if x>=channels else 0
            above=out[row+x-stride] if y else 0
            corner=out[row+x-stride-channels] if y and x>=channels else 0
            if mode==0:predict=0
            elif mode==1:predict=left
            elif mode==2:predict=above
            elif mode==3:predict=(left+above)//2
            elif mode==4:
                p=left+above-corner;dl,da,dc=abs(p-left),abs(p-above),abs(p-corner)
                predict=left if dl<=da and dl<=dc else above if da<=dc else corner
            else:raise ValueError(mode)
            out[row+x]=(raw[at]+predict)&255;at+=1
    return w,h,channels,out
p=argparse.ArgumentParser();p.add_argument('before');p.add_argument('after');p.add_argument('--crop',help='x,y,width,height');a=p.parse_args()
w,h,c,first=read(a.before);w2,h2,c2,second=read(a.after);assert (w,h)==(w2,h2)
x0,y0,cw,ch=(map(int,a.crop.split(',')) if a.crop else (0,0,w,h));hist=[0]*256
for y in range(y0,y0+ch):
    for x in range(x0,x0+cw):
        at=(y*w+x)*c;bt=(y*w+x)*c2;delta=max(abs(first[at+k]-second[bt+k]) for k in range(3));hist[delta]+=1
n=sum(hist);print(f'{cw}x{ch}, {n} pixels; identical {hist[0]*100/n:.4f}%; within8 {sum(hist[:9])*100/n:.4f}%; max {max(i for i,v in enumerate(hist) if v)}')
