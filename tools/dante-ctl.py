#!/usr/bin/env python3
"""dante-ctl.py : control tool for the emulator's UDP virtual Dante network (a minimal "Dante Controller").

  dante-ctl.py list [SECONDS]                 listen for device announcements and print the devices on the network
  dante-ctl.py subscribe TARGET RX DEVICE TX  make device TARGET's receive channel RX (1-based) take DEVICE's transmit channel TX
  dante-ctl.py unsubscribe TARGET RX          clear a subscription
Options: --group G (default 239.69.83.1) --port P (default 14336) --iface IP (local interface for multicast)

Packet format: see emu/src/audio/dante_virtual.h.  This is the emulator's own protocol, not Audinate's Dante."""
import socket, struct, sys, time

args = [a for a in sys.argv[1:]]
group, port, iface = '239.69.83.1', 14336, '0.0.0.0'
for opt in ('--group', '--port', '--iface'):
    if opt in args:
        i = args.index(opt); v = args[i + 1]; del args[i:i + 2]
        if opt == '--group': group = v
        elif opt == '--port': port = int(v)
        else: iface = v
if not args: print(__doc__); sys.exit(1)

def name32(s): return s.encode()[:31].ljust(32, b'\0')

def listen(seconds):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group) + socket.inet_aton(iface))
    s.settimeout(0.2)
    seen = {}; t0 = time.time()
    while time.time() - t0 < seconds:
        try: d, addr = s.recvfrom(65535)
        except socket.timeout: continue
        if len(d) >= 57 and d[:4] == b'RIOV' and d[4] == 1:
            nm = d[8:40].split(b'\0')[0].decode(errors='replace'); fs, tx, rx, st, leader = struct.unpack_from('<IHHQB', d, 40)
            seen[nm] = (addr[0], fs, tx, rx, leader, st)
    print('%-24s %-16s %6s %3s %3s  %s' % ('device', 'address', 'fs', 'tx', 'rx', 'clock'))
    for nm, (ip, fs, tx, rx, leader, st) in sorted(seen.items()):
        print('%-24s %-16s %6d %3d %3d  %s' % (nm, ip, fs, tx, rx, 'leader' if leader else 'follower'))
    if not seen: print('(no devices announced on %s:%d within %.0f s)' % (group, port, seconds))

def send(pkt):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    if iface != '0.0.0.0': s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))
    s.sendto(pkt, (group, port)); s.close()

cmd = args[0]
if cmd == 'list':
    listen(float(args[1]) if len(args) > 1 else 2.5)
elif cmd == 'subscribe' and len(args) >= 5:
    target, rx, dev, tx = args[1], int(args[2]), args[3], int(args[4])
    send(b'RIOV' + bytes([3, 1, 0, 0]) + name32(target) + struct.pack('<H', rx) + name32(dev) + struct.pack('<H', tx))
    print('sent: %s RX %d <- %s@%d' % (target, rx, dev, tx))
elif cmd == 'unsubscribe' and len(args) >= 3:
    target, rx = args[1], int(args[2])
    send(b'RIOV' + bytes([3, 1, 0, 0]) + name32(target) + struct.pack('<H', rx) + name32('') + struct.pack('<H', 0))
    print('sent: %s RX %d <- (none)' % (target, rx))
else:
    print(__doc__); sys.exit(1)
