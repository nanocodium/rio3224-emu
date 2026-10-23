#!/usr/bin/env python3
"""cs4-hist.py ERRFILE [UPTO_MARKER_MS] : histogram of DSP register-transport writes (dev, reg, count) in a --log DSPIF trace,
optionally only up to the first '[console] <ms>' marker (i.e. the boot-time programming)."""
import sys, re, collections
lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
upto = sys.argv[2] if len(sys.argv) > 2 else None
h = collections.Counter(); first = {}
for i, l in enumerate(lines):
    if upto and l.startswith('[console] ' + upto): break
    m = re.match(r'\[CS4\] (write|read ) dev (\w+) reg (\w+) x(\d+):(.*)', l)
    if not m: continue
    key = (m.group(1).strip(), m.group(2), m.group(3), m.group(4))
    h[key] += 1
    first.setdefault(key, m.group(5).strip())
for k, n in sorted(h.items(), key=lambda kv: (kv[0][1], int(kv[0][2], 16), kv[0][0])):
    print('%-5s dev %s reg %s x%-3s %5d  first: %s' % (k[0], k[1], k[2], k[3], n, first[k][:60]))
