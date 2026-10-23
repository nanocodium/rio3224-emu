#!/usr/bin/env python3
"""flash-records.py FLASH.bin [REGION_HEX [LEN_HEX]] : list the firmware's parameter-store records in a flash region
(default 0x700000, 0x80000): a record starts with {c1, seq, ?, ?, be32 size, be32 version 6, ff fe|ff, be16 check}
and the check is the two's complement of the byte sum of the payload.  Prints every record (newest = highest seq)
and, for the big SETUP/NonVolatile store, the SETUP values and channel 1 of the NonVolatile bank."""
import sys, struct
d = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x700000
ln = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x80000
off = base
while off + 16 <= min(len(d), base + ln):
    if d[off] == 0xC1 and d[off + 12] == 0xFF and struct.unpack_from('>I', d, off + 8)[0] == 6:
        size = struct.unpack_from('>I', d, off + 4)[0]; chk = struct.unpack_from('>H', d, off + 14)[0]
        calc = (-sum(d[off + 16:off + size])) & 0xFFFF
        print('record @%06x: seq %d hdr %s size %#x check %04x (%s)' % (off, d[off + 1], d[off:off+16].hex(' '), size, chk, 'negsum16 OK' if calc == chk else 'calc %04x' % calc))
        if size > 0x600:
            v = d[off + 0x35C: off + 0x370]; n = d[off + 0x580: off + 0x58B]
            print('   SETUP: LastScreen %d PanelLock %d Fan %d UnitId %d StartupMode %d SwitchMode %d NetMode %d Dimmer %d ErrMsg %d Bright %d IP %s' % (
                v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], '.'.join(str(x) for x in v[10:14])))
            print('   NonVolatile ch1: 48V %d gain %.2f dB gcOn %d gcStart %.2f dB hpfOn %d hpf %.1f Hz' % (
                n[0], struct.unpack_from('>h', n, 1)[0] / 100.0, n[3], struct.unpack_from('>h', n, 4)[0] / 100.0, n[6], struct.unpack_from('>I', n, 7)[0] / 10.0))
        off += max(16, size)
    else:
        off += 2
