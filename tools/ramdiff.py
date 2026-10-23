#!/usr/bin/env python3
"""ramdiff.py BASE.bin OTHER.bin [SDRAM_BASE_HEX] [MAX] : 32-bit words that differ between two SDRAM dumps
(address, base value, other value). Consecutive words are grouped into ranges."""
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x0C000000
mx = int(sys.argv[4]) if len(sys.argv) > 4 else 80
n = min(len(a), len(b)) & ~3
diffs = [i for i in range(0, n, 4) if a[i:i+4] != b[i:i+4]]
print('%d differing words' % len(diffs))
groups = []
for i in diffs:
    if groups and i == groups[-1][1] + 4: groups[-1][1] = i
    else: groups.append([i, i])
print('%d ranges' % len(groups))
for s, e in groups[:mx]:
    cnt = (e - s) // 4 + 1
    av = ' '.join(a[s+k:s+k+4].hex() for k in range(0, min(e - s + 4, 16), 4))
    bv = ' '.join(b[s+k:s+k+4].hex() for k in range(0, min(e - s + 4, 16), 4))
    print('%08x x%-4d  %s  ->  %s' % (base + s, cnt, av, bv))
