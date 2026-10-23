#!/usr/bin/env python3
"""fn-grep.py PATTERN FUNC... : grep PATTERN (regex) inside the decompiled bodies of the named functions (FUN_xxx or hex addr); -C N for context"""
import re, sys, os
args = sys.argv[1:]; ctx = 0
if args and args[0] == '-C': ctx = int(args[1]); args = args[2:]
pat = re.compile(args[0]); names = [a if a.startswith('FUN_') else 'FUN_' + a for a in args[1:]]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're', 'RIO2_PROG.decomp.c'), encoding='utf-8', errors='replace').read().split('\n')
starts = [(i, l.split()[3]) for i, l in enumerate(src) if l.startswith('// ==== ')]
for k, (i, n) in enumerate(starts):
    if n not in names: continue
    end = starts[k + 1][0] if k + 1 < len(starts) else len(src)
    for j in range(i, end):
        if pat.search(src[j]):
            for q in range(max(i, j - ctx), min(end, j + ctx + 1)): print('%s:%d: %s' % (n, q + 1, src[q]))
            if ctx: print('--')
