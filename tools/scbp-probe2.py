#!/usr/bin/env python3
# scbp-probe2.py NAME [SPACING_MS] : HA-remote (MCBP) probe - console messages injected on DNT UART B with the
# emulator's injection markers and the firmware's console prints merged into one stream, so each reaction can be
# attributed to a frame; afterwards the firmware's HA parameter reports are read back.
#
# Frame (decoded from the firmware, see docs/HARDWARE.md part 7):
#   LEN | C0 'M' 'B' 'C' 01 | u16 region length | src MAC[6] | dst MAC[6] | 00 | messages... | CS   (CS = ~sum(C0..last) & 0xFF)
#   message = u16 length (incl. itself) | hdr | 07 | u16 command id | u16 count | u16 index | value...
# Environment:
#   FRAMES="hi:lo[:hexvalue[:index[:hdr]]],..."   messages to send (command id hi/lo bytes in hex), e.g. 22:10:1a:0
#   EXTRA="--probe ... --probe-max N"                extra emulator options (register probes, --dante-haremote MS, ...)
# Reads back: rep current / hgain / h48 / hpfo / hpff / gcom.  Outputs in out/NAME.raw (WSL /tmp is volatile).
import os, re, subprocess, sys

OUT = '/mnt/d/rio3224-emu/out'
name = sys.argv[1] if len(sys.argv) > 1 else 'probe'
spacing = int(sys.argv[2]) if len(sys.argv) > 2 else 1500
UNIT_MAC = bytes.fromhex('1dc112345600')          # the 6 bytes the unit itself puts in the source field of its frames
CONSOLE_MAC = bytes.fromhex('001dc1aabbcc')
NL = chr(10)

def wrap(messages, dst=UNIT_MAC, src=CONSOLE_MAC, lenprefix=True):
    region = b''.join(messages)
    body = src + dst + bytes([0]) + region
    f = bytes([0xC0, 0x4D, 0x42, 0x43, 0x01]) + len(region).to_bytes(2, 'big') + body
    f += bytes([(~sum(f)) & 0xFF])
    return (bytes([len(f)]) + f) if lenprefix else f

def message(hi, lo, value=b'', index=0, count=1, hdr=0x10):
    payload = bytes([hdr, 0x07, hi, lo]) + count.to_bytes(2, 'big') + index.to_bytes(2, 'big') + value
    return (len(payload) + 2).to_bytes(2, 'big') + payload

frames = []
for spec in (os.environ.get('FRAMES') or '22:10:1a:0').split(','):
    hi, lo, val, idx, hdr = (spec.split(':') + ['', '', '0', '10'])[:5]
    idx = int(idx or 0); hdr = int(hdr or '10', 16)
    frames.append(('hdr %02x cmd %s%s idx %d v=%s' % (hdr, hi, lo, idx, val or '-'), wrap([message(int(hi, 16), int(lo, 16), bytes.fromhex(val), idx, 1, hdr)])))

args = []; t = 30000
for label, f in frames:
    args += ['--dntb-send', '%d:%s' % (t, f.hex())]; t += spacing
end = t + 7000
os.chdir('/mnt/d/rio3224-emu/emu')
subprocess.run(['python3', '../tools/flash-set.py', '../fw/d2/flash.bin', '%s/%s-flash.bin' % (OUT, name), 'startup=2'], stdout=subprocess.DEVNULL)
console = ['%d /rep current' % (end - 5000), '%d /rep hgain' % (end - 4000), '%d /rep h48' % (end - 3500), '%d /rep hpfo' % (end - 3000), '%d /rep hpff' % (end - 2500), '%d /rep gcom' % (end - 2000)]
open('%s/%s-console.txt' % (OUT, name), 'w').write(NL.join(console) + NL)
cmd = [os.path.expanduser('~/rio3224/build/rio-emu'), '--max', '90000000000', '--run-ms', str(end), '--no-raw', '--flash', '%s/%s-flash.bin' % (OUT, name),
       '--dante-boot-ms', '15000', '--log', 'DNTUART', '--console-script', '%s/%s-console.txt' % (OUT, name)] + args + os.environ.get('EXTRA', '').split()
with open('%s/%s.raw' % (OUT, name), 'wb') as out:
    subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT)      # merged so the firmware's prints interleave with the injection markers
txt = open('%s/%s.raw' % (OUT, name), 'rb').read().decode('shift_jis', 'replace').replace(chr(13), '')
print('=== %s' % name)
for i, (label, f) in enumerate(frames): print('  %2d @%d ms: %-34s %d bytes: %s' % (i, 30000 + i * spacing, label, len(f), f.hex()))
print('--- injection markers and firmware reactions, in order:')
for l in txt.split(NL):
    if 'injected' in l or 'HAP' in l or 'MCBP' in l or 'HA_REMOTE' in l or l.startswith('[probe]') or l.startswith('  R') or l.startswith('  mem') or l.startswith('  stack'): print('  ' + l[:170])
lines = txt.split(NL); start = next((i for i, l in enumerate(lines) if 'injected' in l), len(lines))
tx = bytes(int(m.group(1), 16) for l in lines[start:] for m in [re.match(r'\[DNTB\] -> ([0-9A-F]{2})', l)] if m)
print('--- unit transmitted %d bytes on UART B after the first probe' % len(tx))
i = 0
while i < len(tx):
    L = tx[i]; f = tx[i + 1:i + 1 + L]; i += 1 + L
    if len(f) < 24: print('  trailing', f.hex(' ')); break
    msg = f[20:-1]
    if len(msg) >= 10:
        mlen = int.from_bytes(msg[0:2], 'big'); val = msg[10:mlen]
        print('  <- hdr %02x cmd %s count %d index %d value %s%s' % (msg[2], msg[4:6].hex(), int.from_bytes(msg[6:8], 'big'), int.from_bytes(msg[8:10], 'big'), val.hex(' ') or '-', '' if len(f) < 40 else '  (padded frame)'))
    else: print('  <- raw', f.hex(' '))
for key in ('Panel Lock:', 'Read Only:', 'Mute(DIO):', '48V Master:', '- HA Gain', '- HA 48V', '- HPF [', '- HPF Freq', '- Gain Comp'):
    m = re.search(re.escape(key) + r'[^' + NL + r']*(' + NL + r'[^' + NL + r']*){0,2}', txt)
    print('---', key, ':', (' | '.join(m.group(0).split(NL)[:3]) if m else '(none)'))
