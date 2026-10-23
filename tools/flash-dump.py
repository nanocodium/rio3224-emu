#!/usr/bin/env python3
"""flash-dump.py FLASH.bin START END [find-hdr] : hex dump of a flash range; with 'find-hdr' lists c1 01 00 01 headers in the range."""
import sys, struct
d = open(sys.argv[1], 'rb').read(); s = int(sys.argv[2], 16); e = int(sys.argv[3], 16)
if len(sys.argv) > 4 and sys.argv[4] == 'find-hdr':
    off = s
    while off < e:
        if d[off:off+4] == b'\xc1\x01\x00\x01':
            size = struct.unpack_from('>I', d, off + 4)[0]
            print('hdr at %06x: %s size %#x' % (off, d[off:off+16].hex(' '), size))
        off += 2
else:
    for off in range(s, e, 16):
        row = d[off:off+16]; print('%06x: %s  %s' % (off, row.hex(' '), ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))
