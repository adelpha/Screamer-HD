#!/usr/bin/env python3
"""Compares positions recorded in the original (record-replay.ps1, COMPACT records) with ours
(screamer --replay FILE --positions OUT). Exit code 1 when any frame differs.

Usage: py -3.14 tools/dosbox/cmp_positions.py ORIGINAL.trace OURS.bin [MAX_LINES] [OURS_OFFSET]
  OURS_OFFSET: our frame of the original's first record (a recording made with -Skip)
"""
import struct,sys
o=open(sys.argv[1],'rb').read(); u=open(sys.argv[2],'rb').read()
rs=struct.unpack_from('<I',o,12)[0]; O=[o[16+k*rs:16+(k+1)*rs] for k in range((len(o)-16)//rs)]
off=int(sys.argv[4]) if len(sys.argv)>4 else 0
U=[u[k*148:(k+1)*148] for k in range(off, len(u)//148)]
print('orig',len(O),'ours',len(U))
shown=0
for k in range(min(len(O),len(U))):
    a=O[k]; b=U[k]
    ft,=struct.unpack_from('<f',a,0)
    op=struct.unpack_from('<3i',a,28); oy=struct.unpack_from('<3f',a,28+24); osp=struct.unpack_from('<f',a,64)[0]
    oc=struct.unpack_from('<27i',a,68)
    up=struct.unpack_from('<3i',b,0); uy=struct.unpack_from('<3f',b,24); usp=struct.unpack_from('<f',b,36)[0]
    uc=struct.unpack_from('<27i',b,40)
    diffs=[]
    if op!=up: diffs.append(f'pos {op} vs {up}')
    if oy!=uy: diffs.append(f'pyr {oy} vs {uy}')
    if osp!=usp: diffs.append(f'speed {osp} vs {usp}')
    for c in range(9):
        if oc[3*c:3*c+3]!=uc[3*c:3*c+3]: diffs.append(f'cpu{c} {oc[3*c:3*c+3]} vs {uc[3*c:3*c+3]}')
    if diffs:
        print(k, f'ft {ft:.5f}', '; '.join(diffs[:4]))
        shown+=1
        if shown>=int(sys.argv[3] if len(sys.argv)>3 else 10): break
if len(U) < len(O):
    print(f'ours ends early ({len(U)} of {len(O)} frames)')
    shown += 1
print(f'{shown and "DIFFER" or "identical"}: {min(len(O),len(U))} frames compared')
sys.exit(1 if shown else 0)
