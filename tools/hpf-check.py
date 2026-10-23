#!/usr/bin/env python3
"""hpf-check.py TX.wav OFF_T0 OFF_T1 ON_T0 ON_T1 CH:FREQ[,CH:FREQ...] [coef words]
Compares the Dante TX level of stationary tones with the HPF off and on, and against the response of the biquad
whose five Q30 words the firmware wrote (default: the 125 Hz set observed from RIO2_PROG V1.91)."""
import sys, math
sys.path.insert(0, __import__('os').path.dirname(__file__))
from importlib import import_module
wa = import_module('wav-analyse')

def q30(x): return (x - 2**32 if x & 0x80000000 else x) / 2**30
def biquad_db(words, f, fs):
    v = [q30(w) for w in words]; na2, na1, b0, b1, b2 = v
    w = 2 * math.pi * f / fs
    z = complex(math.cos(w), -math.sin(w)); z2 = z * z
    num = b0 + b1 * z + b2 * z2; den = 1 - na1 * z - na2 * z2
    return 20 * math.log10(abs(num / den) + 1e-30)

path = sys.argv[1]; off0, off1, on0, on1 = (float(x) for x in sys.argv[2:6])
chans = [(int(p.split(':')[0]), float(p.split(':')[1])) for p in sys.argv[6].split(',')]
words = [int(x, 16) for x in sys.argv[7:12]] if len(sys.argv) >= 12 else [0xC176C5F1, 0x7E84E3FC, 0x3F438783, 0x8178F0FA, 0x3F438783]
tag, nch, fs, bits, frames, data = wa.read_wav(path)
print('%s: %d ch %d Hz; HPF off window %.1f-%.1f s, on window %.1f-%.1f s' % (path, nch, fs, off0, off1, on0, on1))
print('  ch   tone(Hz)   off(dBFS)    on(dBFS)   measured(dB)  biquad(dB)   error')
worst = 0.0
for ch, f in chans:
    xo = wa.samples(data, tag, nch, bits, frames, ch - 1, int(off0 * fs), int(off1 * fs))
    xn = wa.samples(data, tag, nch, bits, frames, ch - 1, int(on0 * fs), int(on1 * fs))
    po, ro, fo = wa.analyse(xo, fs); pn, rn, fn = wa.analyse(xn, fs)
    meas = rn - ro; theo = biquad_db(words, f, fs); err = meas - theo; worst = max(worst, abs(err))
    print('  %2d  %8.1f   %9.2f   %9.2f     %8.2f     %8.2f   %+6.2f' % (ch, f, ro, rn, meas, theo, err))
print('worst error %.2f dB -> %s' % (worst, 'PASS' if worst < 0.3 else 'CHECK'))
