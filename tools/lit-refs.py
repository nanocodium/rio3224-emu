#!/usr/bin/env python3
"""lit-refs.py ADDR... : list literal-pool / table slots holding each 32-bit value and the decompiled functions that reference the slots"""
import re, sys, bisect, os
here = os.path.dirname(os.path.abspath(__file__))
d = open(os.path.join(here, '..', 'fw', 'd2', 'RIO2_PROG.BIN'), 'rb').read(); B = 0x0C0F0FF0
src = open(os.path.join(here, '..', 're', 'RIO2_PROG.decomp.c'), encoding='utf-8', errors='replace').read()
starts = [(m.start(), m.group(1), m.group(2)) for m in re.finditer(r'// ==== ([0-9a-f]{8}) (\S+)', src)]
pos = [s for s, _, _ in starts]
def fns(addr):
    out = set()
    for m in re.finditer(r'_%08x\b' % addr, src):
        k = bisect.bisect_right(pos, m.start()) - 1
        if k >= 0: out.add(starts[k][2])
    return sorted(out)
for a in sys.argv[1:]:
    v = int(a, 16); pat = v.to_bytes(4, 'big'); i = 0; hits = []
    while True:
        i = d.find(pat, i)
        if i < 0: break
        if i % 2 == 0: hits.append(i + B)
        i += 1
    print('%08x:' % v, ', '.join('%08x %s' % (h, fns(h)) for h in hits) or 'no slots')
