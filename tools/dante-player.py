#!/usr/bin/env python3
"""dante-player.py NAME SOURCE [SECONDS] : a transmitting device on the emulator's UDP virtual Dante network - stands in
for a console output, another I/O rack or a playback system feeding the Rio's Dante receive channels.

  SOURCE = FILE.wav                       stream the WAV file's channels as TX 1..n (16/24/32-bit PCM or float, any rate = the network rate)
         | tone:FREQ[,FREQ2,...][:dBFS]   sine tones on TX 1..n (default -20 dBFS)
Options: --group G --port P --iface IP (as dante-ctl.py), --group2 G2 --port2 P2 to also send on a secondary network.
Announces itself every second and sends one 1 ms block (48 frames at 48 kHz) per packet, paced to real time.
Subscribe the Rio with: dante-ctl.py subscribe <RioName> <rx> NAME <tx>  (or the emulator's --dante-patch "rx@NAME:tx").
This is the emulator's own protocol (emu/src/audio/dante_virtual.h), not Audinate's Dante."""
import math, socket, struct, sys, time

args = sys.argv[1:]
opts = {'--group': '239.69.83.1', '--port': '14336', '--iface': '0.0.0.0', '--group2': '', '--port2': '14337'}
for k in list(opts):
    if k in args:
        i = args.index(k); opts[k] = args[i + 1]; del args[i:i + 2]
if len(args) < 2: print(__doc__); sys.exit(1)
name, source = args[0], args[1]; seconds = float(args[2]) if len(args) > 2 else 1e9

def read_wav(path):
    d = open(path, 'rb').read(); assert d[:4] == b'RIFF' and d[8:12] == b'WAVE'
    pos = 12; fmt = None; data = None
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]; sz = struct.unpack_from('<I', d, pos + 4)[0]; body = d[pos + 8:pos + 8 + sz]
        if cid == b'fmt ': fmt = struct.unpack_from('<HHIIHH', body, 0)
        elif cid == b'data': data = body
        pos += 8 + sz + (sz & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 0xFFFE: tag = struct.unpack_from('<H', body, 24)[0] if False else (3 if bits == 32 and b'\x03\x00\x00\x00' in fmt_body(d) else 1)
    n = len(data) // (ch * bits // 8); frames = []
    for i in range(n):
        f = []
        for c in range(ch):
            o = (i * ch + c) * (bits // 8)
            if bits == 16: v = struct.unpack_from('<h', data, o)[0] / 32768.0
            elif bits == 24: v = int.from_bytes(data[o:o + 3], 'little', signed=True) / 8388608.0
            elif bits == 32 and tag == 3: v = struct.unpack_from('<f', data, o)[0]
            else: v = struct.unpack_from('<i', data, o)[0] / 2147483648.0
            f.append(v)
        frames.append(f)
    return rate, ch, frames

def fmt_body(d):
    pos = 12
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]; sz = struct.unpack_from('<I', d, pos + 4)[0]
        if cid == b'fmt ': return d[pos + 8:pos + 8 + sz]
        pos += 8 + sz + (sz & 1)
    return b''

rate = 48000
if source.startswith('tone:'):
    parts = source.split(':'); freqs = [float(x) for x in parts[1].split(',')]; level = float(parts[2]) if len(parts) > 2 else -20.0
    amp = 10 ** (level / 20.0); ch = len(freqs); wav = None
    print('%s: %d tone channel(s) %s Hz at %.1f dBFS' % (name, ch, freqs, level))
else:
    rate, ch, wav = read_wav(source); print('%s: %s, %d ch, %d Hz, %.1f s' % (name, source, ch, rate, len(wav) / rate))

def sock(group, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    if opts['--iface'] != '0.0.0.0': s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(opts['--iface']))
    return s, (group, port)
nets = [sock(opts['--group'], int(opts['--port']))]
if opts['--group2']: nets.append(sock(opts['--group2'], int(opts['--port2'])))

def name32(s): return s.encode()[:31].ljust(32, b'\0')
block = rate // 1000; t = 0; seq = 0; t0 = time.perf_counter(); last_ann = 0.0; pos = 0
hdr_audio = b'RIOV' + bytes([2, 1, 0, 0]) + name32(name)
while t / rate < seconds:
    now = time.perf_counter()
    if now - last_ann >= 1.0:
        last_ann = now
        ann = b'RIOV' + bytes([1, 1, 0, 0]) + name32(name) + struct.pack('<IHHQB', rate, ch, 0, t, 0)
        for s, to in nets: s.sendto(ann, to)
    # one block per millisecond of real time
    due = t0 + (t / rate)
    if now < due: time.sleep(min(0.001, due - now)); continue
    pcm = bytearray()
    for i in range(block):
        if wav is None:
            for c in range(ch): pcm += struct.pack('<i', int(round(amp * math.sin(2 * math.pi * freqs[c] * (t + i) / rate) * 8388607)) << 8)
        else:
            f = wav[pos % len(wav)]; pos += 1
            for c in range(ch): v = max(-1.0, min(1.0, f[c])); pcm += struct.pack('<i', int(round(v * 8388607)) << 8)
    pkt = hdr_audio + struct.pack('<IHHQI', rate, ch, block, t, seq) + bytes(pcm)
    for s, to in nets: s.sendto(pkt, to)
    t += block; seq += 1
print('%s: sent %d blocks (%.1f s)' % (name, seq, t / rate))
