#!/usr/bin/env python3
"""patch_rotate.py : add --rotate STEPS@T[,gray|bin][,MSPERSTEP] : step the PLD +0xA 4-bit field (front-panel rotary) from time T."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

def rw(path, pairs):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)

rw('emu/src/board/machine.h', [(
"""    struct Press { int sw; uint64_t fromMs, toMs; };""",
"""    struct Rotate { int steps; uint64_t fromMs; int msPerStep; bool gray; };
    std::vector<Rotate> rotations;           // scheduled rotary-encoder movements (--rotate): the 4-bit code on PLD +0xA bits 4:1
    int rotaryCode = 0;                      // current position code (binary count; gray-encoded on the pins when asked)
    struct Press { int sw; uint64_t fromMs, toMs; };""")])

rw('emu/src/board/machine.cpp', [(
"""        for (auto& pr : presses) if (ms >= pr.fromMs && ms < pr.toMs && pr.sw >= 0 && pr.sw < 8) b |= 1 << pr.sw;
        pld->buttons = b;""",
"""        for (auto& pr : presses) if (ms >= pr.fromMs && ms < pr.toMs && pr.sw >= 0 && pr.sw < 8) b |= 1 << pr.sw;
        pld->buttons = b;
        if (!rotations.empty()) {                                                                // rotary encoder position = base + steps done so far
            int pos = 0; bool gray = false;
            for (auto& r : rotations) {
                if (ms < r.fromMs) continue;
                int done = (int)((ms - r.fromMs) / (r.msPerStep > 0 ? r.msPerStep : 60)) + 1;
                int n = r.steps < 0 ? -r.steps : r.steps; if (done > n) done = n;
                pos += r.steps < 0 ? -done : done; gray = r.gray;
            }
            rotaryCode = pos & 0xF;
            pld->dipSw = gray ? (uint8_t)(rotaryCode ^ (rotaryCode >> 1)) : (uint8_t)rotaryCode;
        }""")])

rw('emu/src/main.cpp', [(
"""        else if (a == "--trace-ms" && i + 1 < argc) traceMs = atoi(argv[++i]);""",
"""        else if (a == "--trace-ms" && i + 1 < argc) traceMs = atoi(argv[++i]);
        else if (a == "--rotate" && i + 1 < argc) {                     // STEPS@T[,gray|bin][,MSPERSTEP]: turn the front-panel encoder
            int steps = 0; unsigned t = 0; char mode[8] = "bin"; int per = 60;
            if (sscanf(argv[++i], "%d@%u,%7[a-z],%d", &steps, &t, mode, &per) >= 2 || sscanf(argv[i], "%d@%u", &steps, &t) == 2)
                rotations.push_back({ steps, t, per, std::string(mode) == "gray" });
        }"""),
("""int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath, ramDumpPath;""",
 """int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath, ramDumpPath; std::vector<Machine::Rotate> rotations;"""),
("""    mach.pld->inputMaskHigh = pldIn; mach.pld->reg10 = pldIn10;""",
 """    mach.pld->inputMaskHigh = pldIn; mach.pld->reg10 = pldIn10; mach.rotations = rotations;"""),
])
print('rotate option added')
