#!/usr/bin/env python3
"""param-arrays.py DUMP.bin [DUMP2.bin ...] : print the first channels of the firmware's NonVolatile per-channel
parameter arrays (32 x int32 each) from SDRAM dumps (see param-hunt*.sh), to identify which array is which."""
import sys, struct
BASE = 0x0C000000
arrays = [('0c7a15c8', 0x0c7a15c8), ('0c7a1848', 0x0c7a1848), ('0c7a1ac8', 0x0c7a1ac8), ('0c7a1d48', 0x0c7a1d48),
          ('0c7a1fc8', 0x0c7a1fc8), ('0c7a2248', 0x0c7a2248)]
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    print('==', path)
    for name, a in arrays:
        o = a - BASE
        vals = struct.unpack_from('>8i', d, o)
        print('  %s: %s' % (name, ' '.join('%6d' % v for v in vals)))
