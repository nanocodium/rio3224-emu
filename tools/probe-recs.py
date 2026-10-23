#!/usr/bin/env python3
"""probe-recs.py ERRFILE [MAXLINES] : one line per probe hit: insn, PC, PR and the first dumped memory line; DANTE lines are kept in order"""
import sys, re
n = int(sys.argv[2]) if len(sys.argv) > 2 else 200
out = []
blk = None
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if line.startswith('[probe]'):
        blk = {'insn': line.split('insn ')[1].strip(), 'pc': line.split('PC=')[1].split()[0], 'mem': None, 'pr': ''}
        out.append(blk); continue
    if line.startswith('[DANTE]'):
        out.append(line.rstrip()[:96]); blk = None; continue
    if blk is None: continue
    if line.startswith('PC='): blk['pr'] = line.split('PR=')[1].split()[0]
    m = re.match(r'  ([0-9A-F]{8}): (.*)', line)
    if m and blk['mem'] is None: blk['mem'] = m.group(1) + ': ' + m.group(2).strip()
for o in out[:n]:
    print(o if isinstance(o, str) else 'insn %s PC %s PR %s  %s' % (o['insn'], o['pc'], o['pr'], o['mem']))
