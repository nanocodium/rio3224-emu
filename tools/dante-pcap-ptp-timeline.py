#!/usr/bin/env python3
"""dante-pcap-ptp-timeline.py FILE.pcap : per source uuid and message type, when the first and last PTPv1 message was
seen in an endpoint trace and how many - shows who was master when, and when a device went quiet."""
import struct, sys, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from importlib import import_module
pcap = import_module('dante-pcap-summary')
CTRL = {0: 'Sync', 1: 'Delay_Req', 2: 'Follow_Up', 3: 'Delay_Resp'}
stats = {}
t0 = None
for t, ip in pcap.pcap_packets(sys.argv[1]):
    if len(ip) < 28 or ip[9] != 17: continue
    ihl = (ip[0] & 15) * 4; udp = ip[ihl:]; sp, dp = struct.unpack('>HH', udp[:4]); p = udp[8:]
    if dp not in (319, 320) or len(p) < 40 or struct.unpack('>H', p[:2])[0] != 1: continue
    t0 = t if t0 is None else t0
    key = (':'.join('%02x' % x for x in p[22:28]), CTRL.get(p[32], p[32]), sp == dp)
    st = stats.setdefault(key, [t, t, 0]); st[1] = t; st[2] += 1
for (uuid, ctrl, own), (a, b, n) in sorted(stats.items()):
    print('%-18s %-10s %-9s first +%7.1f s  last +%7.1f s  %5d msgs' % (uuid, ctrl, 'sent' if own else 'heard', a - t0, b - t0, n))
