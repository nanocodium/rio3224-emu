#!/usr/bin/env python3
"""ptp-listen.py IFACE_IP [SECONDS] : join 224.0.1.129 on ports 319/320 (shared with whatever else is bound there) and
count the PTPv1 messages heard per source uuid and type.  Shows whether PTP multicast actually reaches a new socket
on this host - Windows shares a UDP port between processes only when every socket allows address reuse."""
import socket, struct, sys, time, collections

iface = sys.argv[1]; seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
socks = []
for port in (319, 320):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try: s.bind(('', port))
    except OSError as e: print('bind %d failed: %s' % (port, e)); continue
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton('224.0.1.129') + socket.inet_aton(iface))
    s.settimeout(0.2); socks.append((port, s))
counts = collections.Counter(); first = {}; last = {}
end = time.time() + seconds
CTRL = {0: 'Sync', 1: 'Delay_Req', 2: 'Follow_Up', 3: 'Delay_Resp'}
while time.time() < end:
    for port, s in socks:
        try: d, src = s.recvfrom(2048)
        except socket.timeout: continue
        if len(d) < 40 or d[0:2] != b'\0\x01': continue
        uuid = ':'.join('%02x' % x for x in d[22:28]); key = (uuid, CTRL.get(d[32], d[32]), src[0], port)
        counts[key] += 1; last[key] = time.time(); first.setdefault(key, time.time())
for k, n in sorted(counts.items()):
    print('%-18s %-10s from %s:%d  %3d msgs  (%.1f/s)' % (k[0], k[1], k[2], k[3], n, n / max(0.1, last[k] - first[k] + 0.25)))
if not counts: print('no PTP messages heard in %.0f s' % seconds)
