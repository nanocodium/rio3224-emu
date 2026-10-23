#!/usr/bin/env python3
"""dante-arc.py HOST[:PORT] COMMAND ... : talk to a Dante device the way a controller does (ARC request/response on
UDP 4440, start code 0x2729), to exercise the emulator's Controller-interoperable endpoint without a GUI:
  info                       channel counts (0x1000), device name (0x1002), names (0x1003)
  tx                         transmit channel pages (0x2000) + friendly names (0x2010)
  rx                         receive channel pages with subscriptions and status (0x3000)
  flows                      transmit flows (0x2200) and receive flows (0x3200)
  sub RX TXCHAN TXHOST       subscribe receive channel RX (1-based) to TXCHAN@TXHOST (0x3010)
  unsub RX                   clear the subscription (0x3010 with zero offsets)
  rename-tx N NAME / rename-rx N NAME   channel friendly names (0x2013 / 0x3001)
  raw OPCODE [HEX]           any opcode with a raw payload; prints the reply
The port defaults to 4440; the emulator prints the port it actually uses (another program may own 4440 on the host).
"""
import socket, struct, sys

HDR = 10
def req(sock, addr, op, payload=b'', start=0x2729, seq=1, timeout=2.0):
    pkt = struct.pack('>HHHHH', start, HDR + len(payload), seq, op, 0) + payload
    sock.sendto(pkt, addr)
    sock.settimeout(timeout)
    while True:
        data, src = sock.recvfrom(4096)
        if len(data) >= HDR:
            s, ln, sq, o, code = struct.unpack('>HHHHH', data[:HDR])
            if sq == seq and o == op: return code, data
def zstr(data, off):
    if off == 0 or off >= len(data): return ''
    e = data.index(b'\0', off) if b'\0' in data[off:] else len(data)
    return data[off:e].decode('utf-8', 'replace')
def pages(sock, addr, op, extra=b''):
    start = 1; out = []
    while True:
        code, data = req(sock, addr, op, struct.pack('>HHH', 1, start, 0) + extra)   # exactly what Dante Controller sends: u16 1, u16 first channel (1-based), u16 0
        if code not in (1, 0x8112): print('  error 0x%04x' % code); break
        space, count = data[HDR], data[HDR + 1]
        out.append(data)
        if code != 0x8112 or count == 0: break
        start += count
    return out

def main():
    if len(sys.argv) < 3: print(__doc__); return
    host, _, port = sys.argv[1].partition(':'); addr = (host, int(port or 4440)); cmd = sys.argv[2]; a = sys.argv[3:]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if cmd == 'info':
        code, d = req(sock, addr, 0x1000); c = d[HDR:]
        print('channels: tx %d rx %d, max per flow %d, max tx flows %d, max rx flows %d, flags 0x%02x (code 0x%04x)' % (struct.unpack('>H', c[2:4])[0], struct.unpack('>H', c[4:6])[0], struct.unpack('>H', c[8:10])[0], struct.unpack('>H', c[12:14])[0], struct.unpack('>H', c[14:16])[0], c[1], code))
        code, d = req(sock, addr, 0x1002); print('device name: %s' % zstr(d, HDR))
        code, d = req(sock, addr, 0x1003); h = struct.unpack('>22H', d[HDR:HDR + 44])
        print('names: friendly %s | factory %s | board %s | revision %s' % (zstr(d, h[6]), zstr(d, h[7]), zstr(d, h[3]), zstr(d, h[4])))
    elif cmd == 'tx':
        for d in pages(sock, addr, 0x2000):
            n = d[HDR + 1]
            for i in range(n):
                cid, k, common, name = struct.unpack('>HHHH', d[HDR + 2 + i * 8:HDR + 10 + i * 8]); fs, = struct.unpack('>I', d[common:common + 4]); bits, = struct.unpack('>H', d[common + 6:common + 8])
                print('  TX %2d  %-16s %d Hz %d bit' % (cid, zstr(d, name), fs, bits))
        for d in pages(sock, addr, 0x2010):
            n = d[HDR + 1]
            for i in range(n):
                cid, cid2, name = struct.unpack('>HHH', d[HDR + 2 + i * 6:HDR + 8 + i * 6]); print('  TX %2d friendly %s' % (cid, zstr(d, name)))
    elif cmd == 'rx':
        for d in pages(sock, addr, 0x3000):
            n = d[HDR + 1]
            for i in range(n):
                cid, k, common, txn, txh, fr, status, z = struct.unpack('>HHHHHHII', d[HDR + 2 + i * 20:HDR + 22 + i * 20])
                stat = {0: '-', 1: 'unresolved', 8: 'in progress', 0x14: 'tx busy', 0x15: 'tx error', 0x01010009: 'RECEIVING unicast', 0x0101000a: 'RECEIVING multicast'}.get(status, '0x%08x' % status)
                sub = ('%s@%s' % (zstr(d, txn), zstr(d, txh))) if txn else ''
                print('  RX %2d  %-16s <- %-28s %s' % (cid, zstr(d, fr), sub, stat))
    elif cmd == 'flows':
        for op, label in ((0x2200, 'TX flow'), (0x3200, 'RX flow')):
            for d in pages(sock, addr, op):
                n = d[HDR + 1]
                for i in range(n):
                    off, = struct.unpack('>H', d[HDR + 2 + i * 2:HDR + 4 + i * 2])
                    fid, ftype, fs, z, bits, one, nch, sockoff = struct.unpack('>HHIHHHHH', d[off:off + 18])
                    port, = struct.unpack('>H', d[sockoff + 2:sockoff + 4]); ip = '.'.join(map(str, d[sockoff + 4:sockoff + 8]))
                    print('  %s %d: type 0x%x, %d Hz %d bit, %d channels, socket %s:%d' % (label, fid, ftype, fs, bits, nch, ip, port))
    elif cmd == 'sub' and len(a) >= 3:
        rx = int(a[0]); body = bytearray(b'\x00\x01') + b'\0' * 6          # [space][count] then 6-byte items {rx, name offset, host offset}; offsets include the 10-byte header
        nameoff = HDR + len(body); body += a[1].encode() + b'\0'; hostoff = HDR + len(body); body += a[2].encode() + b'\0'
        body[2:8] = struct.pack('>HHH', rx, nameoff, hostoff)
        code, d = req(sock, addr, 0x3010, bytes(body)); print('subscribe RX %d <- %s@%s: code 0x%04x' % (rx, a[1], a[2], code))
    elif cmd == 'unsub' and a:
        code, d = req(sock, addr, 0x3010, b'\x00\x01' + struct.pack('>HHH', int(a[0]), 0, 0)); print('unsubscribe RX %s: code 0x%04x' % (a[0], code))
    elif cmd in ('rename-tx', 'rename-rx') and len(a) >= 2:
        n = int(a[0]); name = a[1].encode() + b'\0'
        if cmd == 'rename-tx': body = struct.pack('>HH', 0, 1) + struct.pack('>HHH', 0, n, HDR + 10) + name; op = 0x2013
        else: body = struct.pack('>HH', 0, 1) + struct.pack('>HH', n, HDR + 8) + name; op = 0x3001
        code, d = req(sock, addr, op, body); print('%s %d -> %s: code 0x%04x' % (cmd, n, a[1], code))
    elif cmd == 'raw' and a:
        op = int(a[0], 16); payload = bytes.fromhex(a[1]) if len(a) > 1 else b''
        try:
            code, d = req(sock, addr, op, payload); print('code 0x%04x, %d bytes: %s' % (code, len(d) - HDR, d[HDR:].hex()))
        except socket.timeout: print('no reply')
    else: print(__doc__)

if __name__ == '__main__': main()
