#!/usr/bin/env python3
"""uartb-decode.py ERRFILE : decode the conmon_haremote frames the host writes on DNT UART B ([DNTB] -> XX lines):
   len | C0 'M' 'B' 'C' 01 00 LL | payload | ~sum"""
import sys, re
b = bytes(int(m.group(1), 16) for m in re.finditer(r'^\[DNTB\] -> ([0-9A-F]{2})', open(sys.argv[1], encoding='utf-8', errors='replace').read(), re.M))
print('host wrote %d bytes on UART B' % len(b))
i = 0; n = 0
while i < len(b):
    L = b[i]
    if i + 1 + L > len(b): print('trailing %d bytes: %s' % (len(b) - i, b[i:i+16].hex(' '))); break
    f = b[i+1:i+1+L]; i += 1 + L; n += 1
    hdr = f[:7].hex(' '); pl = f[7:-1]; cs = f[-1] if f else 0
    txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in pl[:48])
    print('frame %2d len %3d hdr %s cs %02X payload[%d]: %s  %s' % (n, L, hdr, cs, len(pl), pl[:24].hex(' '), txt))
