#!/usr/bin/env python3
"""oled-bars.py FILE.pgm : measure the 32 level bars of the firmware's METER page in an OLED capture (left panel):
prints, per input, the bar's column range and its lit height in pixels."""
import sys
f = open(sys.argv[1], 'rb'); assert f.readline().strip() == b'P5'
line = f.readline()
while line.startswith(b'#'): line = f.readline()
w, h = map(int, line.split()); mx = int(f.readline()); px = f.read()
def p(x, y): return px[y * w + x]
# bar region: rows 14..h-8 roughly; find columns with lit pixels in the lower band of the left panel
left = w // 2
cols = [x for x in range(left) if any(p(x, y) > 96 for y in range(12, h - 8))]
# group consecutive columns into bars
bars = []
for x in cols:
    if bars and x == bars[-1][1] + 1: bars[-1][1] = x
    else: bars.append([x, x])
print('panel %dx%d, %d bar groups' % (w, h, len(bars)))
for i, (a, b) in enumerate(bars):
    ys = [y for y in range(12, h - 8) if any(p(x, y) > 96 for x in range(a, b + 1))]
    print('bar %2d: cols %3d-%3d  rows %2d-%2d  height %2d' % (i + 1, a, b, min(ys), max(ys), max(ys) - min(ys) + 1))
