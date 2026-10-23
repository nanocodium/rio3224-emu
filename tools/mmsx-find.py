#!/usr/bin/env python3
"""mmsx-find.py FLASH.bin NAME : find a parameter NAME in the firmware's serialized parameter banks ("MMSXBIG"), show the
record, the enclosing bank header, and try common CRC-16 algorithms against the header's CRC field."""
import sys, struct

def crc16(data, poly, init, refin, refout, xorout):
    crc = init
    for b in data:
        if refin:
            b = int('{:08b}'.format(b)[::-1], 2)
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    if refout:
        crc = int('{:016b}'.format(crc)[::-1], 2)
    return crc ^ xorout

d = open(sys.argv[1], 'rb').read(); name = sys.argv[2].encode()
banks = []
i = 0
while True:
    i = d.find(b'MMSXBIG', i)
    if i < 0: break
    banks.append(i - 12); i += 7
print('banks (header at):', ['%06x' % b for b in banks])
for b in banks:
    size, ver, crcw = struct.unpack_from('>IIHH', d, b)[0:3] if False else (struct.unpack_from('>I', d, b)[0], struct.unpack_from('>I', d, b + 4)[0], struct.unpack_from('>H', d, b + 10)[0])
    print('bank %06x: size %#x ver %d crc %04x' % (b, size, ver, crcw))
pos = d.find(name)
while pos >= 0:
    bank = max([b for b in banks if b < pos] or [0])
    print('\n"%s" at %06x (bank %06x, offset %#x):' % (name.decode(), pos, bank, pos - bank))
    for off in range(pos - 0x30, pos + 0x40, 16):
        row = d[off:off + 16]
        print('  %06x: %s  %s' % (off, row.hex(' '), ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))
    pos = d.find(name, pos + 1)
# CRC trials on the first bank
if banks:
    b = banks[0]; size = struct.unpack_from('>I', d, b)[0]; want = struct.unpack_from('>H', d, b + 10)[0]
    for start in (b, b + 12, b + 16):
        for end in (b + size, b + 12 + size, b + 16 + size, b + size + 16):
            payload = d[start:end]
            for nm, p, ini, ri, ro, xo in (('ccitt-false', 0x1021, 0xFFFF, 0, 0, 0), ('xmodem', 0x1021, 0, 0, 0, 0), ('kermit', 0x1021, 0, 1, 1, 0), ('arc', 0x8005, 0, 1, 1, 0), ('modbus', 0x8005, 0xFFFF, 1, 1, 0), ('x25', 0x1021, 0xFFFF, 1, 1, 0xFFFF), ('genibus', 0x1021, 0xFFFF, 0, 0, 0xFFFF)):
                c = crc16(payload, p, ini, ri, ro, xo)
                if c == want: print('CRC match: %s over [%06x,%06x) = %04x' % (nm, start, end, c))
