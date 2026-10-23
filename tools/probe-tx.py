#!/usr/bin/env python3
"""probe-tx.py RAWFILE : from a merged scbp-probe2 output file, print each injection marker followed by the UART B frames
the unit transmitted before the next injection (decoded: header, command id, count, index, value)."""
import re, sys
lines = open(sys.argv[1], 'rb').read().decode('shift_jis', 'replace').replace('\r', '').split('\n')
buf = []
def flush():
    global buf
    b = bytes(buf); i = 0
    while i < len(b):
        L = b[i]; f = b[i + 1:i + 1 + L]; i += 1 + L
        if len(f) < 24: print('      trailing', f.hex(' ')); break
        msg = f[20:-1]
        if len(msg) >= 10:
            mlen = int.from_bytes(msg[0:2], 'big'); hdr = msg[2]; cmd = msg[4:6].hex(); count = int.from_bytes(msg[6:8], 'big'); idx = int.from_bytes(msg[8:10], 'big'); val = msg[10:mlen]
            print('      <- unit: hdr %02x cmd %s count %d index %d value %s%s' % (hdr, cmd, count, idx, val.hex(' ') or '-', '' if len(f) < 40 else '  (frame padded to %d)' % len(f)))
        else: print('      <- unit raw', f.hex(' '))
    buf = []
for l in lines:
    m = re.match(r'\[DNTB\] -> ([0-9A-F]{2})', l)
    if m: buf.append(int(m.group(1), 16)); continue
    if 'injected' in l or 'HAP' in l or 'MCBP' in l:
        flush(); print(l[:120])
flush()
