#!/usr/bin/env python3
"""uart-decode.py ERRFILE : decode the BCA frames from a --log DNTUART trace, separately for the bytes the host
wrote ([DNTA] -> XX) and the bytes the host actually read from the RX FIFO ([DNTA] <= XX)."""
import sys, re
def frames(bytes_):
    out = []; buf = None; esc = False
    for b in bytes_:
        if esc and buf is not None: buf.append(b); esc = False; continue
        if b == 0xFF: buf = []; esc = False; continue
        if buf is None: continue
        if b == 0xFE:
            if len(buf) >= 9:
                h = buf[:8]; pl = buf[8:-1]; ck = buf[-1]
                out.append((h, pl, ck))
            else: out.append((None, buf, None))
            buf = None; continue
        if b == 0xFD and not esc: esc = True; continue
        buf.append(b); esc = False
    return out
def show(tag, fr):
    for h, pl, ck in fr:
        if h is None: print(f"{tag} short frame {bytes(pl).hex(' ')}"); continue
        length = (h[6] << 8) | h[7]
        print(f"{tag} cat {h[0]:02X} cmd {h[1]:3d} seq {h[3]:3d} res {h[4]} org {h[5]} len {length:3d} pl {len(pl):3d}: {bytes(pl[:20]).hex(' ')}{' ...' if len(pl) > 20 else ''}")
txt = open(sys.argv[1], errors='replace').read()
tx = [int(m.group(1), 16) for m in re.finditer(r'^\[DNTA\] -> ([0-9A-F]{2})', txt, re.M)]
rx = [int(m.group(1), 16) for m in re.finditer(r'^\[DNTA\] <= ([0-9A-F]{2})', txt, re.M)]
print(f"host wrote {len(tx)} bytes, read {len(rx)} bytes")
sel = sys.argv[2] if len(sys.argv) > 2 else 'both'
if sel in ('both', 'rx'): show('host-read ', frames(rx))
if sel in ('both', 'tx'): show('host-wrote', frames(tx))
