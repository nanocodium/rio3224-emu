#!/usr/bin/env python3
"""switch-ids.py : list the 16-bit command ids and handler offsets in the firmware's HA-remote message executor switch
(0x0C175514..0x0C1757BA): the code loads an id into r5/r6 with movi20 (or increments it with add #n) and, on a match,
adds a handler offset to r7 before branching to the common tail."""
import re, subprocess, os
dis = subprocess.run([os.path.expanduser('~/rio3224/build/rio-emu'), '--disasm', '0C175514,250'], cwd='/mnt/d/rio3224-emu/emu', capture_output=True, text=True).stdout
cur = None; out = []
for l in dis.split('\n'):
    m = re.search(r'movi20 #0x([0-9A-F]+),r[56]', l)
    if m: cur = int(m.group(1), 16); continue
    m = re.search(r'add #(\d+),r([56])', l)
    if m and cur is not None: cur += int(m.group(1)); continue
    m = re.search(r'add #(\d+),r7', l)
    if m and cur is not None: out.append((cur, int(m.group(1)))); continue
    if 'bra 0x0C1757BA' in l and out and out[-1][0] != cur and cur is not None: out.append((cur, -1))   # accepted without a handler offset
print(' '.join('%04x->%s' % (i, ('+%d' % o) if o >= 0 else 'ok') for i, o in out))
