#!/usr/bin/env python3
"""patch_console_script.py : add --console-script FILE (timed lines typed into the SCIF0 debug console)."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
nl = chr(92) + 'n'

def rw(path, pairs):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)

rw('emu/src/board/machine.h', [(
"""    struct Press { int sw; uint64_t fromMs, toMs; };""",
"""    struct ConsoleLine { uint64_t ms; std::string text; };
    std::vector<ConsoleLine> consoleScript;   // --console-script: lines typed into the SCIF0 debug monitor at the given times
    size_t consolePos = 0; std::string consolePending; uint64_t consoleNextByteMs = 0;
    struct Press { int sw; uint64_t fromMs, toMs; };""")])

rw('emu/src/board/machine.cpp', [(
"""    {   // board inputs that depend on time: fan tacho square wave, scheduled switch presses""",
"""    if (consolePos < consoleScript.size() || !consolePending.empty()) {   // scripted debug-console input, one byte per 2 ms
        uint64_t ms = mainFpga->elapsedCycles / mainFpga->cyclesPerMs;
        if (consolePos < consoleScript.size() && ms >= consoleScript[consolePos].ms) { consolePending += consoleScript[consolePos].text; consolePending += '\\r'; consolePos++; }
        if (!consolePending.empty() && ms >= consoleNextByteMs) { scif[0]->rxByte((uint8_t)consolePending[0]); consolePending.erase(0, 1); consoleNextByteMs = ms + 2; }
    }
    {   // board inputs that depend on time: fan tacho square wave, scheduled switch presses""")])

rw('emu/src/main.cpp', [(
"""        else if (a == "--dante-fresh") danteFresh = true;""",
"""        else if (a == "--dante-fresh") danteFresh = true;
        else if (a == "--console-script" && i + 1 < argc) consoleScriptPath = argv[++i];   // file of "<ms> <text>" lines"""),
("""int danteSwMode = -1; bool danteFresh = false;""",
 """int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath;"""),
("""    mach.cpu.ringEnable = ringDump > 0;""",
 """    mach.cpu.ringEnable = ringDump > 0;
    if (!consoleScriptPath.empty()) {
        FILE* cf = fopen(consoleScriptPath.c_str(), "r");
        if (!cf) { fprintf(stderr, "cannot open console script %s""" + nl + """", consoleScriptPath.c_str()); return 1; }
        char line[512];
        while (fgets(line, sizeof line, cf)) {
            char* end = nullptr; unsigned long long t = strtoull(line, &end, 10);
            if (end == line) continue;
            while (*end == ' ' || *end == '\\t') end++;
            std::string text(end); while (!text.empty() && (text.back() == '\\n' || text.back() == '\\r')) text.pop_back();
            mach.consoleScript.push_back({ t, text });
        }
        fclose(cf);
    }""")])
print('console script option added')
