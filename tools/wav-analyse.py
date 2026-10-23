#!/usr/bin/env python3
"""wav-analyse.py FILE.wav [FROM_SEC [TO_SEC]] : per channel peak/RMS level (dBFS) and dominant frequency of the window.
Pure python (no numpy): the dominant frequency is estimated from zero crossings of the band-limited signal, the
level from the samples.  Good enough to prove which tone sits on which channel."""
import sys, struct, math

def read_wav(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'WAVE'
    pos = 12; fmt = None; data = None
    while pos + 8 <= len(d):
        cid = d[pos:pos+4]; sz = struct.unpack_from('<I', d, pos + 4)[0]; body = d[pos+8:pos+8+sz]
        if cid == b'fmt ': fmt = body
        elif cid == b'data': data = body
        pos += 8 + sz + (sz & 1)
    tag, nch, fs = struct.unpack_from('<HHI', fmt, 0); bits = struct.unpack_from('<H', fmt, 14)[0]
    if tag == 0xFFFE: tag = struct.unpack_from('<H', fmt, 24)[0]
    bps = bits // 8; frames = len(data) // (nch * bps)
    return tag, nch, fs, bits, frames, data

def samples(data, tag, nch, bits, frames, ch, start, end):
    bps = bits // 8; out = []
    for i in range(start, end):
        o = (i * nch + ch) * bps
        if tag == 3: out.append(struct.unpack_from('<f', data, o)[0])
        elif bits == 16: out.append(struct.unpack_from('<h', data, o)[0] / 32768.0)
        elif bits == 24:
            v = data[o] | (data[o+1] << 8) | (data[o+2] << 16)
            if v & 0x800000: v -= 1 << 24
            out.append(v / 8388608.0)
        else: out.append(struct.unpack_from('<i', data, o)[0] / 2147483648.0)
    return out

def analyse(x, fs):
    if not x: return (-200.0, -200.0, 0.0)
    pk = max(abs(v) for v in x); rms = math.sqrt(sum(v * v for v in x) / len(x))
    # zero crossings (positive-going) -> frequency
    zc = 0; prev = x[0]
    for v in x[1:]:
        if prev < 0 <= v: zc += 1
        prev = v
    f = zc * fs / len(x)
    db = lambda a: 20 * math.log10(a) if a > 1e-12 else -200.0
    return (db(pk), db(rms), f)

if __name__ == '__main__':
    path = sys.argv[1]
    tag, nch, fs, bits, frames, data = read_wav(path)
    t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    t1 = float(sys.argv[3]) if len(sys.argv) > 3 else frames / fs
    s, e = int(t0 * fs), min(int(t1 * fs), frames)
    print('%s: %d ch, %d Hz, %d bit (fmt %d), %.2f s; window %.2f-%.2f s' % (path, nch, fs, bits, tag, frames / fs, s / fs, e / fs))
    for c in range(nch):
        pk, rms, f = analyse(samples(data, tag, nch, bits, frames, c, s, e), fs)
        print('  ch %2d: peak %7.2f dBFS  rms %7.2f dBFS  f %8.1f Hz' % (c + 1, pk, rms, f))
