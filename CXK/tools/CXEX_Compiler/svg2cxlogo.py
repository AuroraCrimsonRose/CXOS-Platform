#!/usr/bin/env python3
# svg2cxlogo.py - convert the CX logo SVG into a fixed-point flattened-polygon
# table for the freestanding kernel renderer, AND rasterize it on the host so we
# can eyeball correctness before any kernel code runs.
#
# Output table model (matches what klogo.c will consume):
#   - logo defined in a VIEW x VIEW unit box (600), coords stored *FP (16) as
#     int16 so the kernel scales with pure 32-bit integer math.
#   - per path: RGB color + one or more subpaths; all subpaths of a path are
#     filled together with the EVEN-ODD rule (handles the cutout holes).
#
# Usage:
#   svg2cxlogo.py file.svg --emit logo_vec.h        # write kernel header
#   svg2cxlogo.py file.svg --render out.png --size 600 [--drop-bg]

import sys, re, argparse

VIEW = 600          # viewBox is 0 0 600 600
FP   = 16           # fixed-point scale (4 fractional bits); 600*16=9600 < int16
FLATNESS = 0.20     # cubic flattening tolerance in view units

# ---- tiny SVG path parser (M/L/C and Z, absolute + relative) ----------------
def tokenize(d):
    # numbers (incl. exponent / leading +-/.) and command letters
    return re.findall(r'[MmLlCcZzHhVv]|[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?', d)

def parse_path(d):
    """Return list of subpaths; each subpath is a list of (x,y) anchor/curve
    points with curves preserved as ('C',p1,p2,p3) and lines as ('L',p)."""
    toks = tokenize(d)
    i = 0
    subpaths = []
    cur = None
    start = (0.0, 0.0)
    pos = (0.0, 0.0)
    cmd = None
    def num():
        nonlocal i
        v = float(toks[i]); i += 1; return v
    while i < len(toks):
        t = toks[i]
        if re.match(r'[A-Za-z]', t):
            cmd = t; i += 1
        # implicit repeat: keep cmd
        if cmd in ('M', 'm'):
            x = num(); y = num()
            if cmd == 'm': x += pos[0]; y += pos[1]
            pos = (x, y); start = pos
            cur = [('M', pos)]
            subpaths.append(cur)
            cmd = 'L' if cmd == 'M' else 'l'   # subsequent coords are lineto
        elif cmd in ('L', 'l'):
            x = num(); y = num()
            if cmd == 'l': x += pos[0]; y += pos[1]
            pos = (x, y); cur.append(('L', pos)) # type: ignore
        elif cmd in ('H', 'h'):
            x = num()
            if cmd == 'h': x += pos[0]
            pos = (x, pos[1]); cur.append(('L', pos)) # type: ignore
        elif cmd in ('V', 'v'):
            y = num()
            if cmd == 'v': y += pos[1]
            pos = (pos[0], y); cur.append(('L', pos)) # type: ignore
        elif cmd in ('C', 'c'):
            x1=num();y1=num();x2=num();y2=num();x=num();y=num()
            if cmd == 'c':
                x1+=pos[0];y1+=pos[1];x2+=pos[0];y2+=pos[1];x+=pos[0];y+=pos[1]
            cur.append(('C',(x1,y1),(x2,y2),(x,y))) # type: ignore
            pos = (x, y)
        elif cmd in ('Z', 'z'):
            cur.append(('Z',)); pos = start; cmd = None # type: ignore
        else:
            i += 1   # skip anything unhandled (shouldn't happen for this file)
    return subpaths

# ---- cubic flattening (recursive subdivision) -------------------------------
def flatten_cubic(p0, p1, p2, p3, out, depth=0):
    # flatness = max distance of control points from the chord
    def dist(px, a, b):
        ax,ay=a; bx,by=b; px_,py_=px
        dx=bx-ax; dy=by-ay
        if dx==0 and dy==0: 
            return ((px_-ax)**2+(py_-ay)**2)**0.5
        t=((px_-ax)*dx+(py_-ay)*dy)/(dx*dx+dy*dy)
        cx=ax+t*dx; cy=ay+t*dy
        return ((px_-cx)**2+(py_-cy)**2)**0.5
    d1=dist(p1,p0,p3); d2=dist(p2,p0,p3)
    if depth>18 or max(d1,d2)<=FLATNESS:
        out.append(p3); return
    # de Casteljau split at t=0.5
    def mid(a,b): return ((a[0]+b[0])/2,(a[1]+b[1])/2)
    p01=mid(p0,p1); p12=mid(p1,p2); p23=mid(p2,p3)
    p012=mid(p01,p12); p123=mid(p12,p23); p0123=mid(p012,p123)
    flatten_cubic(p0,p01,p012,p0123,out,depth+1)
    flatten_cubic(p0123,p123,p23,p3,out,depth+1)

def subpath_to_polygon(sp):
    poly=[]
    pos=None
    for seg in sp:
        if seg[0]=='M':
            pos=seg[1]; poly.append(pos)
        elif seg[0]=='L':
            pos=seg[1]; poly.append(pos)
        elif seg[0]=='C':
            p1,p2,p3=seg[1],seg[2],seg[3]
            flatten_cubic(pos,p1,p2,p3,poly); pos=p3
        elif seg[0]=='Z':
            pass
    return poly

def load_svg(path):
    s=open(path).read()
    # paths in document (paint) order, with their fill
    items=re.findall(r'<path\b[^>]*?fill="(#[0-9A-Fa-f]+)"[^>]*?d="(.*?)"', s, re.S)
    if not items:  # fill may come after d; try the other order
        items=[]
        for m in re.finditer(r'<path\b(.*?)/?>', s, re.S):
            attrs=m.group(1)
            f=re.search(r'fill="(#[0-9A-Fa-f]+)"',attrs)
            d=re.search(r'd="(.*?)"',attrs,re.S)
            if f and d: items.append((f.group(1),d.group(1)))
    paths=[]
    for fill,d in items:
        r=int(fill[1:3],16); g=int(fill[3:5],16); b=int(fill[5:7],16)
        polys=[subpath_to_polygon(sp) for sp in parse_path(d)]
        polys=[p for p in polys if len(p)>=3]
        paths.append(((r,g,b),polys))
    return paths

# ---- even-odd scanline fill (the SAME algorithm the kernel will use) ---------
def fill_paths(paths, size, drop_bg=False, bg=(0,0,0)):
    import array
    img=[[bg for _ in range(size)] for _ in range(size)]
    scale=size/VIEW
    for color,polys in paths:
        if drop_bg and color==(0,0,0):
            continue
        # build edges (scaled to pixel space)
        edges=[]
        for poly in polys:
            n=len(poly)
            for k in range(n):
                x0,y0=poly[k]; x1,y1=poly[(k+1)%n]
                x0*=scale;y0*=scale;x1*=scale;y1*=scale
                if y0==y1: continue
                edges.append((x0,y0,x1,y1))
        for y in range(size):
            yc=y+0.5
            xs=[]
            for x0,y0,x1,y1 in edges:
                if (y0<=yc<y1) or (y1<=yc<y0):
                    t=(yc-y0)/(y1-y0)
                    xs.append(x0+t*(x1-x0))
            xs.sort()
            for k in range(0,len(xs)-1,2):
                xa=int(round(xs[k])); xb=int(round(xs[k+1]))
                if xb<0 or xa>=size: continue
                xa=max(xa,0); xb=min(xb,size-1)
                for x in range(xa,xb+1):
                    img[y][x]=color
    return img

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('svg')
    ap.add_argument('--render'); ap.add_argument('--size',type=int,default=600)
    ap.add_argument('--drop-bg',action='store_true')
    ap.add_argument('--emit')
    a=ap.parse_args()
    paths=load_svg(a.svg)
    tot=sum(len(p) for _,polys in paths for p in polys)
    print(f"paths={len(paths)} subpaths={sum(len(polys) for _,polys in paths)} "
          f"flattened_vertices={tot}",file=sys.stderr)
    for idx,(c,polys) in enumerate(paths):
        print(f"  path {idx:2d} rgb={c} subpaths={len(polys)} "
              f"verts={sum(len(p) for p in polys)}",file=sys.stderr)
    if a.render:
        from PIL import Image  # type: ignore
        img=fill_paths(paths,a.size,a.drop_bg)
        im=Image.new('RGB',(a.size,a.size))
        im.putdata([img[y][x] for y in range(a.size) for x in range(a.size)])
        im.save(a.render); print("wrote",a.render,file=sys.stderr)

if __name__=='__main__':
    main()