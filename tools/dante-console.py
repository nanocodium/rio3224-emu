#!/usr/bin/env python3
"""dante-console.py UNIT [OPTIONS] COMMAND... : a console / R Remote stand-in on the emulator's UDP virtual Dante
network.  It sends head-amp remote frames to the named unit (packet type 6; the emulator hands them to the firmware on
DNT UART B exactly like the Dante module would) and prints the frames the unit sends back on that UART (packet type 7:
acknowledgements and the parameter notifies the real console would display).

Commands (channels are 1-based; several commands may be given in one call, separated by ';'):
  gain CH dB            head-amp gain -6..66 (0x2216, int16 in 0.01 dB)      gcgain CH dB   GC start gain (0x221A)
  48v CH on|off         +48V (0x2217)                                       gc CH on|off    gain compensation (0x221B)
  hpf CH on|off         HPF on (0x2218)                                      hpffreq CH Hz   HPF frequency 20-600 (0x2219, 0.1 Hz)
  lock on|off           panel lock (0x2213)         mute on|off  audio mute (0x2214)      readonly on|off  HA read only (0x2215)
  brightness N          display brightness (0x2212) label                  request the device label (0x1105)
  listen [SECONDS]      only print the unit's notifies for a while
  raw HI LO [HEXVALUE [INDEX [HDR]]]   send one message `HDR 07 HI LO 0001 INDEX VALUE` (HDR default 00 = request)
Options: --group G --port P --iface IP (as dante-ctl.py), --wait S (listen S seconds after the last command, default 2),
         --src MAC (console MAC, default 00:1d:c1:aa:bb:cc), --dst MAC (unit MAC field, default 1d:c1:12:34:56:00).

Frame: LEN | C0 'M' 'B' 'C' 01 | u16 region length | src MAC | dst MAC | 00 | messages | ~sum(C0..last)
Message: u16 length (incl. itself) | hdr | 07 | u16 command id | u16 count | u16 index | values   (docs/HARDWARE.md part 7)
This is the emulator's own transport (not Audinate's Dante, not Yamaha's licensed console software); the message layer is
what the Rio firmware itself speaks on its HA-remote port, recovered by probing the emulated unit."""
import socket, struct, sys, time

IDS = {0x1103: ('UnitId', 'u8'), 0x1105: ('DeviceLabel', 'str'), 0x1107: ('SystemMode', 'u8'), 0x1108: ('SystemStatus', 'u8'), 0x1109: ('SyncStatus', 'u8'),
       0x2210: ('P48vMasterSw', 'u8'), 0x2211: ('OutputLoadImpedance?', 'u8'), 0x2212: ('Brightness', 'u8'), 0x2213: ('PanelLock', 'u8'), 0x2214: ('AudioMute', 'u8'),
       0x2215: ('ReadOnlyOption', 'u8'), 0x2216: ('HaGain', 'db'), 0x2217: ('P48vOn', 'u8'), 0x2218: ('HpfOn', 'u8'), 0x2219: ('HpfFreq', 'hz'),
       0x221A: ('GcGain', 'db'), 0x221B: ('GcOn', 'u8'), 0x221C: ('GainTrim', 'db'), 0x221D: ('PortToPort', 'u8')}
HDRS = {0x00: 'request', 0x10: 'notify', 0x20: 'ack', 0x30: 'ack', 0x40: 'reply'}

args = sys.argv[1:]
opts = {'--group': '239.69.83.1', '--port': '14336', '--iface': '0.0.0.0', '--src': '00:1d:c1:aa:bb:cc', '--dst': '1d:c1:12:34:56:00', '--wait': '2'}
for k in list(opts):
    if k in args:
        i = args.index(k); opts[k] = args[i + 1]; del args[i:i + 2]
if len(args) < 2: print(__doc__); sys.exit(1)
unit = args[0]; cmdline = ' '.join(args[1:])
group, port, iface = opts['--group'], int(opts['--port']), opts['--iface']
mac = lambda s: bytes.fromhex(s.replace(':', ''))

def name32(s): return s.encode()[:31].ljust(32, b'\0')
def message(cmd, value=b'', index=0, count=1, hdr=0x00):
    payload = bytes([hdr, 0x07]) + cmd.to_bytes(2, 'big') + count.to_bytes(2, 'big') + index.to_bytes(2, 'big') + value
    return (len(payload) + 2).to_bytes(2, 'big') + payload
def frame(messages):
    region = b''.join(messages); body = mac(opts['--src']) + mac(opts['--dst']) + bytes([0]) + region
    f = bytes([0xC0, 0x4D, 0x42, 0x43, 0x01]) + len(region).to_bytes(2, 'big') + body
    f += bytes([(~sum(f)) & 0xFF]); return bytes([len(f)]) + f

def fmt_values(cmd, count, val):
    name, kind = IDS.get(cmd, ('cmd %04x' % cmd, 'raw'))
    if kind == 'str': return repr(val.rstrip(b'\0').decode(errors='replace'))
    if kind == 'db' and len(val) >= 2 * count: return ' '.join('%+.2f' % (struct.unpack('>h', val[2 * i:2 * i + 2])[0] / 100.0) for i in range(count))
    if kind == 'hz' and len(val) >= 2 * count: return ' '.join('%.1f' % (struct.unpack('>H', val[2 * i:2 * i + 2])[0] / 10.0) for i in range(count))
    if kind == 'u8' and len(val) >= count: return ' '.join(str(b) for b in val[:count])
    return val.hex(' ') or '-'
def decode(f):
    if len(f) < 25: return 'raw ' + f.hex(' ')
    body = f[1:1 + f[0]]
    if body[0] != 0xC0 or body[1:4] != b'MBC': return 'raw ' + f.hex(' ')
    out = []; pos = 20
    while pos + 10 <= len(body) - 1:
        mlen = int.from_bytes(body[pos:pos + 2], 'big')
        if mlen < 10 or pos + mlen > len(body): break
        m = body[pos:pos + mlen]; hdr = m[2]; cmd = int.from_bytes(m[4:6], 'big'); count = int.from_bytes(m[6:8], 'big'); idx = int.from_bytes(m[8:10], 'big')
        name = IDS.get(cmd, ('cmd %04x' % cmd, ''))[0]
        out.append('%s %s%s = %s' % (HDRS.get(hdr, 'hdr %02x' % hdr), name, ('[%d..%d]' % (idx + 1, idx + count)) if count > 1 or cmd >= 0x2216 else '', fmt_values(cmd, count, m[10:])))
        pos += mlen
    return ' ; '.join(out) if out else 'raw ' + f.hex(' ')

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); rx.bind(('', port))
rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group) + socket.inet_aton(iface)); rx.settimeout(0.2)
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP); tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
if iface != '0.0.0.0': tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))

def send(msgs, what):
    f = frame(msgs); tx.sendto(b'RIOV' + bytes([6, 1, 0, 0]) + name32(unit) + name32('console') + f, (group, port)); print('-> %s: %s' % (unit, what))
def listen(seconds):
    t0 = time.time()
    while time.time() - t0 < seconds:
        try: d, addr = rx.recvfrom(65535)
        except socket.timeout: continue
        if len(d) > 72 and d[:4] == b'RIOV' and d[4] == 7:
            nm = d[8:40].split(b'\0')[0].decode(errors='replace')
            if nm == unit or unit == '*': print('<- %s: %s' % (nm, decode(d[72:])))
onoff = lambda s: b'\x01' if s.lower() in ('on', '1', 'true') else b'\x00'
ch = lambda s: int(s) - 1

for part in cmdline.split(';'):
    a = part.split()
    if not a: continue
    c = a[0].lower()
    try:
        if c == 'listen': listen(float(a[1]) if len(a) > 1 else 10); continue
        elif c == 'gain': send([message(0x2216, struct.pack('>h', int(round(float(a[2]) * 100))), ch(a[1]))], 'HaGain ch%s = %s dB' % (a[1], a[2]))
        elif c == 'gcgain': send([message(0x221A, struct.pack('>h', int(round(float(a[2]) * 100))), ch(a[1]))], 'GcGain ch%s = %s dB' % (a[1], a[2]))
        elif c == '48v': send([message(0x2217, onoff(a[2]), ch(a[1]))], 'P48vOn ch%s = %s' % (a[1], a[2]))
        elif c == 'hpf': send([message(0x2218, onoff(a[2]), ch(a[1]))], 'HpfOn ch%s = %s' % (a[1], a[2]))
        elif c == 'hpffreq': send([message(0x2219, struct.pack('>H', int(round(float(a[2]) * 10))), ch(a[1]))], 'HpfFreq ch%s = %s Hz' % (a[1], a[2]))
        elif c == 'gc': send([message(0x221B, onoff(a[2]), ch(a[1]))], 'GcOn ch%s = %s' % (a[1], a[2]))
        elif c == 'lock': send([message(0x2213, onoff(a[1]))], 'PanelLock = %s' % a[1])
        elif c == 'mute': send([message(0x2214, onoff(a[1]))], 'AudioMute = %s' % a[1])
        elif c == 'readonly': send([message(0x2215, onoff(a[1]))], 'ReadOnlyOption = %s' % a[1])
        elif c == 'brightness': send([message(0x2212, bytes([int(a[1])]))], 'Brightness = %s' % a[1])
        elif c == 'label': send([message(0x1105)], 'DeviceLabel ?')
        elif c == 'raw':
            cmd = int(a[1], 16) << 8 | int(a[2], 16); val = bytes.fromhex(a[3]) if len(a) > 3 else b''; idx = int(a[4]) if len(a) > 4 else 0; hdr = int(a[5], 16) if len(a) > 5 else 0
            send([message(cmd, val, idx, 1, hdr)], 'raw %04x value %s index %d hdr %02x' % (cmd, val.hex() or '-', idx, hdr))
        else: print('unknown command:', part); continue
    except (IndexError, ValueError) as e: print('bad command %r (%s)' % (part, e)); continue
    listen(0.4)
listen(float(opts['--wait']))
