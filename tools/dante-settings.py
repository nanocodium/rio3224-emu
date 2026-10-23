#!/usr/bin/env python3
"""dante-settings.py HOST:INFOPORT COMMAND [VALUE] : send the ConMon "settings" messages a controller uses on a Dante
device's information port (UDP 8700 on hardware; the emulator prints the port it uses) and print what comes back on
the information multicast group (224.0.0.231:8702):
  rate [HZ]        probe the sample rate (no value) or set it (44100 / 48000 / 88200 / 96000)
  enc [BITS]       probe / set the PCM encoding (16 / 24 / 32)
  identify         ask the device to identify itself (front-panel LEDs blink on a real unit)
  board | product | clock | net   request the board-info / product-info / clock-status / network-status messages
Message layout (observed on the wire, documented by the netaudio project): 0xFFFF, length, message id, 0, 6-byte
requester MAC, 0, "Audinate", 0x07, family (0x3A system / 0x27 audio config), u16 message type, u32 100, then the
request body.  The device answers with a 0x072A message of type (request type - 1) on the multicast group."""
import socket, struct, sys, time

def main():
    if len(sys.argv) < 3: print(__doc__); return
    host, _, port = sys.argv[1].partition(':'); port = int(port or 8700); cmd = sys.argv[2]; val = int(sys.argv[3]) if len(sys.argv) > 3 else None
    mac = bytes([0x3e, 0x42, 0x27, 0x4c, 0xff, 0x24]) if cmd not in ('rate', 'enc') or val is None else b'RT\0\0\0\0'
    kinds = {'rate': 0x0081, 'enc': 0x0083, 'identify': 0x0063, 'board': 0x0061, 'product': 0x00c1, 'clock': 0x0021, 'net': 0x0013}
    if cmd not in kinds: print(__doc__); return
    kind = kinds[cmd]; family = 0x27 if cmd in ('rate', 'enc') and val is not None else 0x3a
    if cmd in ('rate', 'enc'): body = struct.pack('>II', 1, val) if val is not None else struct.pack('>II', 0, 0)
    elif cmd == 'identify': body = b''
    else: body = b''; family = 0x38
    pkt = bytearray(struct.pack('>HHHH', 0xFFFF, 0, 0x4242, 0)) + mac + b'\0\0' + b'Audinate' + bytes([0x07, family]) + struct.pack('>HI', kind, 100) + body
    struct.pack_into('>H', pkt, 2, len(pkt))
    # listen on the information multicast group for the answer
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(('', 8702)); rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton('224.0.0.231') + socket.inet_aton(host)); rx.settimeout(0.3)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tx.sendto(bytes(pkt), (host, port))
    print('sent %s -> %s:%d (%s)' % (cmd, host, port, pkt.hex()))
    end = time.time() + 2.0
    while time.time() < end:
        try: d, src = rx.recvfrom(2048)
        except socket.timeout: continue
        if len(d) < 32: continue
        op = d[24:32]; c = d[32:]
        if op[0] != 0x07 or (op[3] != kind - 1 and op[3] != kind): continue
        print('reply from %s: opcode %s, %d bytes: %s' % (src[0], op.hex(), len(c), c.hex()))
        if kind in (0x81, 0x83) and len(c) >= 16:
            n, cur, new = struct.unpack('>xxH', c[0:4])[0], struct.unpack('>I', c[4:8])[0], struct.unpack('>I', c[8:12])[0]
            rates = struct.unpack('>%dI' % n, c[16:16 + 4 * n]) if len(c) >= 16 + 4 * n else ()
            print('  current %d, pending %d, supported %s' % (cur, new, list(rates)))
        break
    else: print('no reply within 2 s')

if __name__ == '__main__': main()
