#!/usr/bin/env python3
"""patch_dntmute.py : model the Dante module's DNT_MUTE line (main FPGA +4 bit 15): asserted while the module is down or
has just come up; released ~1.5 s after the module runs. The firmware's DIO reports Mute(DIO) from it and only reads the
DSP meter port while unmuted."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

def rw(path, pairs):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)

rw('emu/src/board/mainfpga.h', [
("""        case 0x0004: v = reg04; break;""",
 """        case 0x0004: v = (reg04 & 0x7FFF) | (dntMute ? 0 : 0x8000); break;   // bit 15 = DNT_MUTE from the module (active low: 0 = mute)"""),
("""    bool moduleRunning() const { return src[8].present; }""",
 """    bool moduleRunning() const { return src[8].present; }
    bool dntMute = true;                   // Dante module mute request line (released once the module runs with a stable clock)"""),
])
rw('emu/src/board/machine.h', [
("""    std::unique_ptr<AudioGlue> audio;        // the PCM engine (head amps, DSP, Dante TX/RX, outputs) fed by the firmware's writes""",
 """    std::unique_ptr<AudioGlue> audio;        // the PCM engine (head amps, DSP, Dante TX/RX, outputs) fed by the firmware's writes
    uint64_t danteRunningSinceMs = 0;        // when the module model last came up (DNT_MUTE release timing)
    int dntMuteReleaseMs = 1500;             // module releases its mute line this long after it is running"""),
])
rw('emu/src/board/machine.cpp', [
("""    if (dante) { dante->tick((uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs)); mainFpga->src[8].present = dante->running; }   // module up -> word clock + heartbeats""",
 """    if (dante) {
        uint64_t ms = (uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs);
        dante->tick(ms); mainFpga->src[8].present = dante->running;                         // module up -> word clock + heartbeats
        if (!dante->running) danteRunningSinceMs = 0; else if (!danteRunningSinceMs) danteRunningSinceMs = ms;
        mainFpga->dntMute = !(dante->running && danteRunningSinceMs && ms >= danteRunningSinceMs + (uint64_t)dntMuteReleaseMs);
    }"""),
])
print('DNT_MUTE modelled')
