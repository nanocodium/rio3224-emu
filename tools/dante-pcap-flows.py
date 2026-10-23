#!/usr/bin/env python3
"""dante-pcap-flows.py FILE.pcap [PORT...] : print every flow-control datagram (requests and replies on the given UDP
ports, default 4455 4465 4475) from an endpoint trace, decoded field by field, so two implementations' requests can
be compared byte for byte."""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from importlib import import_module
pcap = import_module('dante-pcap-summary')

def dump(pay, src, dst, ts):
    if len(pay) < 10: return
    start, ln, seq, op, code = struct.unpack('>HHHHH', pay[:10]); c = pay[10:]
    print('%.3f %s -> %s  start %04x len %d seq %d op %04x code %04x' % (ts, src, dst, start, ln, seq, op, code))
    print('   raw: %s' % pay.hex())
    if op == 0x0100 and code == 0 and len(c) >= 16:
        hostoff = struct.unpack('>H', c[0:2])[0]; fs, bits = struct.unpack('>II', c[2:10]); one, nch, remoff = struct.unpack('>HHH', c[10:16])
        ids = struct.unpack('>%dH' % nch, c[16:16 + 2 * nch]); o = 16 + 2 * nch
        tail = c[o:o + 10]
        print('   request: hostname@%d fs %d bits %d one %d nch %d remote@%d ids %s tail %s' % (hostoff, fs, bits, one, nch, remoff, ids, tail.hex()))
        if len(c) >= o + 10:
            u1, u2, u3, fpp, nameoff = struct.unpack('>HHHHH', c[o:o + 10]); print('   fields after ids: %04x %04x %04x fpp %d flowname@%d, then %s' % (u1, u2, u3, fpp, nameoff, c[o + 10:].hex()))
        def z(off):
            if off < 10 or off >= len(pay): return '?'
            e = pay.index(b'\0', off) if b'\0' in pay[off:] else len(pay); return pay[off:e].decode('utf-8', 'replace')
        print('   hostname "%s"' % z(hostoff), end='')
        if len(c) >= o + 10: print(', flow name "%s"' % z(nameoff), end='')
        if remoff + 8 <= len(pay): print(', remote %04x port %d ip %s' % (struct.unpack('>H', pay[remoff:remoff + 2])[0], struct.unpack('>H', pay[remoff + 2:remoff + 4])[0], '.'.join(map(str, pay[remoff + 4:remoff + 8]))))
        else: print()
    elif code != 0:
        print('   reply content: %s' % c.hex())

def main():
    ports = set(int(p) for p in sys.argv[2:]) or {4455, 4465, 4475}
    for ts, ip in pcap.pcap_packets(sys.argv[1]):
        if len(ip) < 28 or ip[9] != 17: continue
        ihl = (ip[0] & 15) * 4; src = '.'.join(map(str, ip[12:16])); dst = '.'.join(map(str, ip[16:20]))
        udp = ip[ihl:]; sp, dp = struct.unpack('>HH', udp[:4]); pay = udp[8:]
        if sp in ports or dp in ports: dump(pay, '%s:%d' % (src, sp), '%s:%d' % (dst, dp), ts)

if __name__ == '__main__': main()
