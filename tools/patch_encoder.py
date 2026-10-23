#!/usr/bin/env python3
"""patch_encoder.py : front-panel rotary encoder = PLD +0 bits 13 (A) / 14 (B), sampled by the firmware's timer interrupt
(FUN_0c165598 -> FUN_0c1c8220 reads (PLD[0] >> 13) & 3). --rotate now steps the quadrature phase instead of the DIP field."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

def rw(path, pairs):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)

rw('emu/src/board/pld.h', [
("""    uint8_t buttons = 0;                     // bit n set = switch n pressed""",
 """    uint8_t buttons = 0;                     // bit n set = switch n pressed
    uint8_t encPhase = 0;                    // rotary encoder quadrature phase 0..3 -> +0 bit13 (A) / bit14 (B); FUN_0c1c8220 reads (PLD[0] >> 13) & 3
    uint16_t encBits() const { static const uint8_t gray[4] = { 0, 1, 3, 2 }; return (uint16_t)(gray[encPhase & 3] << 13); }"""),
])
# the +0 read: replace the ctrl mask so bits 13/14 come from the encoder
p = os.path.join(root, 'emu/src/board/pld.h'); s = open(p, encoding='utf-8', newline='').read()
i = s.index('(ctrl & 0x77FF)')
s = s[:i] + '(ctrl & 0x17FF) | encBits()' + s[i + len('(ctrl & 0x77FF)'):]
open(p, 'w', encoding='utf-8', newline='').write(s)

rw('emu/src/board/machine.cpp', [
("""            rotaryCode = pos & 0xF;
            pld->dipSw = gray ? (uint8_t)(rotaryCode ^ (rotaryCode >> 1)) : (uint8_t)rotaryCode;""",
 """            rotaryCode = pos;
            pld->encPhase = (uint8_t)(pos & 3);                                              // quadrature phase on PLD +0 bits 13/14"""),
])
rw('emu/src/board/machine.h', [
("""    struct Rotate { int steps; uint64_t fromMs; int msPerStep; bool gray; };
    std::vector<Rotate> rotations;           // scheduled rotary-encoder movements (--rotate): the 4-bit code on PLD +0xA bits 4:1
    int rotaryCode = 0;                      // current position code (binary count; gray-encoded on the pins when asked)""",
 """    struct Rotate { int steps; uint64_t fromMs; int msPerStep; bool gray; };
    std::vector<Rotate> rotations;           // scheduled rotary-encoder movements (--rotate STEPS@T[,x,MSPERSTEP]): quadrature transitions
    int rotaryCode = 0;                      // current encoder position (transitions)"""),
])
print('encoder on PLD +0 bits 13/14')
