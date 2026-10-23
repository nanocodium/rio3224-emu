#!/usr/bin/env python3
"""led-probe.py ERRFILE : summarise probe hits at the panel setLed method (PC 0C1C75F0): caller PR -> ids/states."""
import re, sys, collections
t = open(sys.argv[1], errors='replace').read()
by = collections.defaultdict(lambda: collections.Counter())
first = {}
for m in re.finditer(r'\[probe\] PC=0C1C75F0 insn (\d+)\nPC=.*PR=([0-9A-F]+).*\n  R0-7\s+(\S+) (\S+) (\S+) (\S+) (\S+) (\S+) (\S+) (\S+)', t):
    pr = m.group(2); rid = int(m.group(8), 16); st = int(m.group(9), 16)
    by[pr][(rid, st)] += 1
    first.setdefault(pr, int(m.group(1)))
for pr, c in sorted(by.items(), key=lambda kv: first[kv[0]]):
    ids = sorted(set(k[0] for k in c))
    ons = sorted(set(k[0] for k in c if k[1]))
    print(f"caller PR={pr} first at insn {first[pr]}: {sum(c.values())} calls, ids {ids[0]}..{ids[-1]} ({len(ids)} distinct), set-to-1 ids: {ons[:40]}")
