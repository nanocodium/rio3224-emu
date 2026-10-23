#!/usr/bin/env python3
"""mmsx-crc.py FLASH.bin BANKHDR_HEX : dump the bank header area and brute-force the 16-bit check word in the bank
header (bytes +0xE..+0xF) against CRC-16 variants and simple sums over plausible data ranges."""
import sys, struct
d = open(sys.argv[1], 'rb').read(); h = int(sys.argv[2], 16)
magic, size, ver, flags, crcw = struct.unpack_from('>IIIHH', d, h)
print('header %06x: magic %08x size %#x ver %d flags %04x check %04x' % (h, magic, size, ver, flags, crcw))
for off in range(h, h + 0x30, 16):
    row = d[off:off+16]; print('  %06x: %s  %s' % (off, row.hex(' '), ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))

def crc16(data, poly, init, refin, refout, xorout):
    crc = init
    for b in data:
        if refin: b = int('{:08b}'.format(b)[::-1], 2)
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    if refout: crc = int('{:016b}'.format(crc)[::-1], 2)
    return crc ^ xorout
algs = (('ccitt-false', 0x1021, 0xFFFF, 0, 0, 0), ('xmodem', 0x1021, 0, 0, 0, 0), ('kermit', 0x1021, 0, 1, 1, 0), ('arc', 0x8005, 0, 1, 1, 0),
        ('modbus', 0x8005, 0xFFFF, 1, 1, 0), ('x25', 0x1021, 0xFFFF, 1, 1, 0xFFFF), ('genibus', 0x1021, 0xFFFF, 0, 0, 0xFFFF),
        ('buypass', 0x8005, 0, 0, 0, 0), ('dnp', 0x3D65, 0, 1, 1, 0xFFFF), ('maxim', 0x8005, 0, 1, 1, 0xFFFF), ('usb', 0x8005, 0xFFFF, 1, 1, 0xFFFF),
        ('aug-ccitt', 0x1021, 0x1D0F, 0, 0, 0), ('mcrf4xx', 0x1021, 0xFFFF, 1, 1, 0), ('cdma2000', 0xC867, 0xFFFF, 0, 0, 0), ('t10-dif', 0x8BB7, 0, 0, 0, 0),
        ('teledisk', 0xA097, 0, 0, 0, 0), ('dect-x', 0x0589, 0, 0, 0, 0), ('en13757', 0x3D65, 0, 0, 0, 0xFFFF), ('tms37157', 0x1021, 0x89EC, 1, 1, 0), ('riello', 0x1021, 0xB2AA, 1, 1, 0))
starts = [h + k for k in (0x10, 0x14, 0x18, 0x1C, 0x20, 0x3C, 0x40)]
ends = sorted(set([h + size, h + 0x10 + size, h + 0x14 + size, h + size - 2, h + size + 0x10] + [h + k for k in (0x35C, 0x36C, 0x374, 0x378, 0x368 + 0x14)]))
found = False
for s in starts:
    for e in ends:
        if e <= s or e > len(d): continue
        data = d[s:e]
        for nm, p, ini, ri, ro, xo in algs:
            if crc16(data, p, ini, ri, ro, xo) == crcw: print('MATCH %s over [%06x,%06x)' % (nm, s, e)); found = True
        # 16-bit additive / xor sums
        if (sum(data) & 0xFFFF) == crcw: print('MATCH sum16 over [%06x,%06x)' % (s, e)); found = True
        if ((-sum(data)) & 0xFFFF) == crcw: print('MATCH negsum16 over [%06x,%06x)' % (s, e)); found = True
        w = 0
        for i in range(0, len(data) - 1, 2): w = (w + struct.unpack_from('>H', data, i)[0]) & 0xFFFF
        if w == crcw: print('MATCH wordsum over [%06x,%06x)' % (s, e)); found = True
if not found: print('no match')
