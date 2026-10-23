#!/usr/bin/env python3
"""probe-filter.py ERRFILE REG VALUEHEX : print the probe blocks whose register REG (0-15) equals VALUEHEX."""
import sys, re
reg = int(sys.argv[2]); val = int(sys.argv[3], 16)
blk = []
def flush():
    if not blk: return
    regs = []
    for l in blk:
        m = re.match(r'  R0-7 (.*)', l) or re.match(r'  R8-15 (.*)', l)
        if m: regs += [int(x, 16) for x in m.group(1).split()]
    if len(regs) >= 16 and regs[reg] == val: print(''.join(blk).rstrip())
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if line.startswith('[probe]'):
        flush(); blk.clear()
    if blk or line.startswith('[probe]'): blk.append(line)
flush()
