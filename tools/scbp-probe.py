#!/usr/bin/env python3
"""scbp-probe.py [NAME] [OP_LIST] : send candidate console -> unit frames into the firmware's HA remote port (UART B)
and report how it reacts (frames it answers with, [HAP] errors, head-amp changes).  Frames mirror the unit's own
notify format on that port:
   C0 'M' 'B' 'C' 01 00 LL | src MAC[6] | dst MAC[6] | 00 00 LL | 10 07 op class u16 count u16 index value... | ~sum
One frame per second from 30 s; the run ends at 30 s + N + 4 s.  Default probe: ops for class 5 (DeviceLabel)
with no value (a "get"), then ops with a 1-byte value for class 0x12/0x13 (candidate HaGain ids).
Usage from WSL: python3 tools/scbp-probe.py probe1 "11,22,01,02,03,04,05,10,12,13,20,21,23,31,33"
"""
import os, re, subprocess, sys
LEN_PREFIX = os.environ.get('NOLEN') is None   # the host prefixes every frame on this UART with its length byte; mirror that unless NOLEN=1

name = sys.argv[1] if len(sys.argv) > 1 else 'probe1'
ops = [int(x, 16) for x in (sys.argv[2] if len(sys.argv) > 2 else '11,22,01,02,03,04,05,10,12,13,20,21,23,31,33').split(',')]
cls = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x05
value = bytes.fromhex(sys.argv[4]) if len(sys.argv) > 4 else b''

def frame(op, cls, count, index, value):
    scbp = bytes([0x10, 0x07, op, cls]) + count.to_bytes(2, 'big') + index.to_bytes(2, 'big') + value
    src = bytes.fromhex('001dc1aabbcc'); dst = bytes.fromhex('ffffffffffff')
    body = src + dst + bytes([0, 0, len(scbp)]) + scbp
    f = bytes([0xC0, 0x4D, 0x42, 0x43, 0x01, 0x00, len(body) - 15 + 0]) + body   # LL as the unit uses it: SCBP length? see below
    # the unit's frames carry LL = len(scbp) in both places; mirror that
    f = bytes([0xC0, 0x4D, 0x42, 0x43, 0x01, 0x00, len(scbp)]) + body
    cs = (~sum(f)) & 0xFF
    f = f + bytes([cs])
    return bytes([len(f)]) + f if LEN_PREFIX else f

args = []
t = 30000
for op in ops:
    f = frame(op, cls, 1, 0, value)
    args += ['--dntb-send', '%d:%s' % (t, f.hex())]
    t += 1000
end = t + 4000
os.chdir('/mnt/d/rio3224-emu/emu')
subprocess.run(['python3', '../tools/flash-set.py', '../fw/d2/flash.bin', '/tmp/%s-flash.bin' % name, 'startup=2'], stdout=subprocess.DEVNULL)
with open('/tmp/%s-console.txt' % name, 'w') as c:
    c.write('%d rep\n%d hgain\n' % (end - 2500, end - 2000))
cmd = [os.path.expanduser('~/rio3224/build/rio-emu'), '--max', '90000000000', '--run-ms', str(end), '--no-raw', '--flash', '/tmp/%s-flash.bin' % name,
       '--dante-boot-ms', '15000', '--log', 'DNTUART', '--console-script', '/tmp/%s-console.txt' % name] + args
with open('/tmp/%s.raw' % name, 'wb') as out, open('/tmp/%s.err' % name, 'wb') as err:
    subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
txt = open('/tmp/%s.raw' % name, 'rb').read().decode('shift_jis', 'replace').replace('\r', '')
errt = open('/tmp/%s.err' % name, encoding='utf-8', errors='replace').read()
print('=== %s: %s' % (name, re.search(r'\[emu\] (--run-ms.*|stopped.*)', errt).group(0)[:80] if re.search(r'\[emu\] (--run-ms.*|stopped.*)', errt) else '?'))
for m in re.finditer(r'^\[DNT[AB]\] \d+ ms: injected.*$', errt, re.M): print(m.group(0))
hap = [l for l in txt.split('\n') if 'HAP' in l or 'HA_REMOTE' in l or 'haremote' in l.lower() or 'SCBP' in l]
print('--- firmware lines mentioning HAP/HA_REMOTE (%d):' % len(hap)); print('\n'.join(hap[:30]))
# frames the unit transmitted on UART B after the probes started (bytes logged as [DNTB] -> XX)
lines = errt.split('\n'); start = next((i for i, l in enumerate(lines) if 'injected' in l), len(lines))
tx = bytes(int(m.group(1), 16) for l in lines[start:] for m in [re.match(r'\[DNTB\] -> ([0-9A-F]{2})', l)] if m)
print('--- unit transmitted %d bytes on UART B after the first probe' % len(tx))
i = 0
while i < len(tx):
    L = tx[i]; f = tx[i + 1:i + 1 + L]; i += 1 + L
    if len(f) < 8: print('  trailing', f.hex(' ')); break
    print('  len %3d hdr %s scbp %s' % (L, f[:7].hex(' '), f[22:-1].hex(' ')[:120]))
m = re.search(r'hgain[\s\S]{0,600}', txt)
print('--- rep hgain:'); print(m.group(0)[:600] if m else '(none)')
