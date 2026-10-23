#!/usr/bin/env python3
"""patch_realtime.py : add --realtime (pace the emulation to wall-clock time so a live audio backend / interactive use runs at 1x)."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
nl = chr(92) + 'n'

def rw(path, pairs):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)

rw('emu/src/main.cpp', [
("""        else if (a == "--audio") audioOn = true;""",
 """        else if (a == "--audio") audioOn = true;
        else if (a == "--realtime") realtime = true;                 // pace emulated time to wall-clock time (1x)"""),
("""    bool audioOn = false, audioLoop = false;""",
 """    bool realtime = false; bool audioOn = false, audioLoop = false;"""),
("""        uint64_t dc = mach.cpu.cycles - lastCycles; lastCycles = mach.cpu.cycles;
        mach.tick(dc);""",
 """        uint64_t dc = mach.cpu.cycles - lastCycles; lastCycles = mach.cpu.cycles;
        mach.tick(dc);
        if (realtime) {                                                                  // sleep whenever emulated time runs ahead of the wall clock
            double emuMs = mach.mainFpga->elapsedCycles / mach.mainFpga->cyclesPerMs;
            double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (emuMs > wallMs + 1.0) std::this_thread::sleep_for(std::chrono::microseconds((long long)((emuMs - wallMs - 0.5) * 1000.0)));
        }"""),
])
print('realtime option added')
