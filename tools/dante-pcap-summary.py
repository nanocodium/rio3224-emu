#!/usr/bin/env python3
"""dante-pcap-summary.py FILE.pcap : summarise the endpoint's packet trace (LINKTYPE_IPV4 pcap written by --dante-ctl
trace=FILE, or any pcap with raw IPv4 / Ethernet frames): requests per protocol port and opcode, who sent them, and
the opcodes we did not answer.  Used to study what Dante Controller actually asks a device."""
import struct, sys, collections

def pcap_packets(path):
    with open(path, 'rb') as f:
        hdr = f.read(24)
        if len(hdr) < 24: return
        magic, = struct.unpack('<I', hdr[:4]); endian = '<' if magic in (0xa1b2c3d4, 0xa1b23c4d) else '>'
        linktype, = struct.unpack(endian + 'I', hdr[20:24])
        while True:
            rh = f.read(16)
            if len(rh) < 16: break
            ts, tus, incl, orig = struct.unpack(endian + 'IIII', rh)
            data = f.read(incl)
            if linktype == 1: data = data[14:]
            yield ts + tus / 1e6, data

def main():
    if len(sys.argv) < 2: print(__doc__); return
    per_port = collections.Counter(); senders = collections.Counter(); ops = collections.Counter(); unanswered = collections.Counter()
    answered = set(); first = None; last = None
    pkts = list(pcap_packets(sys.argv[1]))
    for ts, ip in pkts:
        if len(ip) < 28 or ip[9] != 17: continue
        ihl = (ip[0] & 15) * 4; src = '.'.join(map(str, ip[12:16])); dst = '.'.join(map(str, ip[16:20]))
        udp = ip[ihl:]; sp, dp = struct.unpack('>HH', udp[:4]); pay = udp[8:]
        first = ts if first is None else first; last = ts
        per_port[(dp if dp in (5353, 4440, 4455, 8700, 8702, 8708, 8800, 319, 320) else sp if sp in (5353, 4440, 4455, 8700, 8702, 8708, 8800, 319, 320) else dp)] += 1
        if dp in (4440, 4455, 8800) or (8700 <= dp < 8800) or (4440 <= sp <= 4455) or (8700 <= sp < 8800) or (8800 <= sp < 8900) or (4440 <= dp <= 4499) or (8800 <= dp <= 8899):
            if len(pay) >= 10 and dp != 8702 and dp != 8708:
                start, ln, seq, op1, op2 = struct.unpack('>HHHHH', pay[:10])
                kind = 'ARC' if dp in range(4440, 4450) or sp in range(4440, 4450) else 'FLOW' if dp in range(4455, 4465) or sp in range(4455, 4465) else 'CMC' if dp in range(8800, 8900) or sp in range(8800, 8900) else 'INFO'
                if op2 == 0:
                    ops[(kind, '%04x' % op1, '%04x' % start)] += 1; senders[(kind, src)] += 1; unanswered[(kind, '%04x' % op1, seq, src)] += 1
                else:
                    answered.add((kind, '%04x' % op1, seq, dst))
        if dp in (8702, 8708) and len(pay) >= 32:
            ops[('MCAST%d' % dp, pay[24:32].hex(), '%04x' % struct.unpack('>H', pay[:2])[0])] += 1
    print('%d packets, %.1f s' % (len(pkts), (last or 0) - (first or 0)))
    print('\nper port:'); [print('  %5d : %d' % (p, n)) for p, n in sorted(per_port.items())]
    print('\nrequests / notifications (kind, opcode, start code):'); [print('  %-9s %s start %s : %d' % (k, o, s, n)) for (k, o, s), n in sorted(ops.items())]
    print('\nrequesters:'); [print('  %-5s %s : %d' % (k, s, n)) for (k, s), n in sorted(senders.items())]
    miss = collections.Counter()
    for (k, o, seq, src), n in unanswered.items():
        if (k, o, seq, src) not in answered: miss[(k, o)] += n
    print('\nrequests without a reply from us:'); [print('  %-5s %s : %d' % (k, o, n)) for (k, o), n in sorted(miss.items())] if miss else print('  none')

if __name__ == '__main__': main()
