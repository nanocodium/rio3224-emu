#!/usr/bin/env python3
"""mdns-sniff.py IFACE_IP [SECONDS] : join the mDNS group on the given interface and print every mDNS query/answer that
mentions Dante (_netaudio-*) or that carries SRV/TXT records, decoded (names, service types, SRV ports, TXT key=value).
Used to see what Dante Controller asks for and what a real Dante device answers with.  No admin rights needed."""
import socket, struct, sys, time

iface = sys.argv[1]; seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
except (AttributeError, OSError): pass
s.bind(('', 5353))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton('224.0.0.251') + socket.inet_aton(iface))
s.settimeout(0.5)

def name_at(d, off, depth=0):
    labels = []
    while True:
        if off >= len(d): return '.'.join(labels), off
        l = d[off]
        if l == 0: return '.'.join(labels), off + 1
        if l & 0xC0 == 0xC0:
            ptr = struct.unpack_from('>H', d, off)[0] & 0x3FFF
            if depth > 8: return '.'.join(labels) + '.<loop>', off + 2
            sub, _ = name_at(d, ptr, depth + 1); labels.append(sub); return '.'.join(labels), off + 2
        labels.append(d[off + 1:off + 1 + l].decode('utf-8', 'replace')); off += 1 + l

TYPES = {1: 'A', 12: 'PTR', 16: 'TXT', 28: 'AAAA', 33: 'SRV', 47: 'NSEC', 255: 'ANY', 41: 'OPT'}
def parse(d, src):
    if len(d) < 12: return
    tid, flags, qd, an, ns, ar = struct.unpack_from('>HHHHHH', d, 0)
    off = 12; out = []
    for i in range(qd):
        nm, off = name_at(d, off); qt, qc = struct.unpack_from('>HH', d, off); off += 4
        out.append('  Q %s %s%s' % (TYPES.get(qt, qt), nm, ' (QU)' if qc & 0x8000 else ''))
    for i in range(an + ns + ar):
        if off + 10 > len(d): break
        nm, off = name_at(d, off); rt, rc, ttl, rl = struct.unpack_from('>HHIH', d, off); off += 10; rd = d[off:off + rl]; off += rl
        if rt == 12: tgt, _ = name_at(d, off - rl); out.append('  PTR %s -> %s (ttl %d)' % (nm, tgt, ttl))
        elif rt == 33:
            pri, wt, port = struct.unpack_from('>HHH', rd, 0); tgt, _ = name_at(d, off - rl + 6); out.append('  SRV %s -> %s:%d (ttl %d)' % (nm, tgt, port, ttl))
        elif rt == 16:
            kv = []; p = 0
            while p < len(rd): l = rd[p]; kv.append(rd[p + 1:p + 1 + l].decode('utf-8', 'replace')); p += 1 + l
            out.append('  TXT %s: %s (ttl %d)' % (nm, ' | '.join(kv), ttl))
        elif rt == 1: out.append('  A %s -> %s (ttl %d)' % (nm, socket.inet_ntoa(rd[:4]), ttl))
        else: out.append('  %s %s (%d bytes, ttl %d)' % (TYPES.get(rt, rt), nm, rl, ttl))
    txt = '\n'.join(out)
    if 'netaudio' in txt or 'SRV' in txt or 'TXT' in txt or 'Q ' in txt:
        print('%s %s from %s (flags %04x, %d q, %d an, %d ns, %d ar)\n%s' % (time.strftime('%H:%M:%S'), 'RESPONSE' if flags & 0x8000 else 'QUERY', src, flags, qd, an, ns, ar, txt))

t0 = time.time(); n = 0
while time.time() - t0 < seconds:
    try: d, addr = s.recvfrom(9000)
    except socket.timeout: continue
    n += 1
    if b'netaudio' in d or b'_dante' in d.lower(): parse(d, '%s:%d' % addr)
print('(%d mDNS packets seen in %.0f s on %s)' % (n, seconds, iface))
