#!/usr/bin/env python3
"""probe-group.py ERRFILE PC : group probe hits at PC by caller PR; show R5/R6/R7 value sets (ids etc.)."""
import re, sys, collections
t = open(sys.argv[1], errors='replace').read(); pc = sys.argv[2].upper()
by = collections.defaultdict(list); first = {}
for m in re.finditer(r'\[probe\] PC=%s insn (\d+)\nPC=.*PR=([0-9A-F]+).*\n  R0-7\s+(\S+) (\S+) (\S+) (\S+) (\S+) (\S+) (\S+) (\S+)' % pc, t):
    pr = m.group(2); regs = [int(x, 16) for x in m.groups()[2:]]
    by[pr].append(regs); first.setdefault(pr, int(m.group(1)))
for pr, lst in sorted(by.items(), key=lambda kv: first[kv[0]]):
    r5 = sorted(set(r[5] for r in lst)); r6 = sorted(set(r[6] for r in lst)); r7 = sorted(set(r[7] for r in lst))
    print(f"PR={pr} first insn {first[pr]} calls {len(lst)}: R5(ids) {r5[:48]}{' ...' if len(r5) > 48 else ''} R6 {r6[:12]} R7 {r7[:12]}")
