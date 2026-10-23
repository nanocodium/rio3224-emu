#!/usr/bin/env python3
"""str-refs.py PATTERN... : for each firmware string matching PATTERN (regex), list its address, the pointer
slots that hold that address, and the decompiled functions referencing either (by _<addr> suffix)."""
import re, struct, sys, bisect, os
here = os.path.dirname(os.path.abspath(__file__))
d = open(os.path.join(here, '..', 'fw', 'd2', 'RIO2_PROG.BIN'), 'rb').read(); BASE = 0x0C0F0FF0
src = open(os.path.join(here, '..', 're', 'RIO2_PROG.decomp.c'), encoding='utf-8', errors='replace').read()
starts = [(m.start(), m.group(1), m.group(2)) for m in re.finditer(r'// ==== ([0-9a-f]{8}) (\S+)', src)]
pos = [s for s, _, _ in starts]
def fns_for(addr):
    out = set()
    for m in re.finditer(r'_%08x\b' % addr, src):
        k = bisect.bisect_right(pos, m.start()) - 1
        if k >= 0: out.add(starts[k][2])
    return sorted(out)
# all 32-bit values, indexed (big endian, 2-byte aligned)
words = {}
for off in range(0, len(d) - 3, 2):
    v = struct.unpack_from('>I', d, off)[0]
    if 0x0C0F0000 <= v < 0x0C400000: words.setdefault(v, []).append(off + BASE)
for pat in sys.argv[1:]:
    rx = re.compile(pat.encode())
    for m in re.finditer(rb'[\x20-\x7e]{4,}', d):
        if not rx.search(m.group()): continue
        sa = m.start() + BASE
        slots = words.get(sa, [])
        print(f"{sa:08x} {m.group()[:70].decode()!r}")
        print(f"    direct refs: {fns_for(sa)}")
        for s in slots: print(f"    slot {s:08x}: {fns_for(s)}")
