#!/usr/bin/env python3
"""param-table.py DUMP.bin ID... : decode entries of the firmware's runtime parameter table (0x34-byte records at the
address held in slot 0x0C1AC24C; built in SDRAM) from an SDRAM dump; prints pointer-looking words and strings."""
import sys, struct
BASE = 0x0C000000
fw = open('/mnt/d/rio3224-emu/fw/d2/RIO2_PROG.BIN', 'rb').read(); FB = 0x0C0F0FF0
d = open(sys.argv[1], 'rb').read()
def wd(a): return struct.unpack_from('>I', d, a - BASE)[0]
def fs(a):
    try:
        o = a - FB; e = fw.index(b'\0', o); t = fw[o:e]
        return t.decode('ascii') if 0 < len(t) < 80 and all(32 <= c < 127 for c in t) else None
    except Exception: return None
def ds(a):
    try:
        o = a - BASE; e = d.index(b'\0', o); t = d[o:e]
        return t.decode('ascii') if 0 < len(t) < 80 and all(32 <= c < 127 for c in t) else None
    except Exception: return None
tab = struct.unpack_from('>I', fw, 0x0c1ac24c - FB)[0]
print('table at %08x' % tab)
for a in sys.argv[2:]:
    idx = int(a); e = tab + idx * 0x34
    words = [wd(e + k * 4) for k in range(13)]
    print('entry %3d @%08x: %s' % (idx, e, ' '.join('%08x' % x for x in words)))
    for k, x in enumerate(words):
        if 0x0C0F0000 <= x < 0x0C400000 and fs(x): print('   [%d] str "%s"' % (k, fs(x)))
        elif 0x0C500000 <= x < 0x0CB00000:
            print('   [%d] RAM %08x -> %08x %08x  %s' % (k, x, wd(x), wd(x + 4), ds(x) or ''))
