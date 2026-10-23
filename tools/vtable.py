#!/usr/bin/env python3
"""vtable.py DUMP.bin OBJADDR [OFFSETS...] : read an object's vtable pointer from an SDRAM dump and print the method
pointers at the given offsets (hex), e.g. vtable.py /tmp/pd-base.bin 0c764c6c 44 50 5c 68 74 80 8c 98 a4"""
import sys, struct
BASE = 0x0C000000
d = open(sys.argv[1], 'rb').read()
obj = int(sys.argv[2], 16)
def w(a): return struct.unpack_from('>I', d, a - BASE)[0]
vt = w(obj)
print('object %08x vtable %08x' % (obj, vt))
for o in sys.argv[3:]:
    off = int(o, 16)
    if 0x0C000000 <= vt < 0x0C000000 + len(d):
        print('  +%02x -> %08x  (fn @ %08x)' % (off, w(vt + off), w(vt + off + 8) if False else w(vt + off)))
