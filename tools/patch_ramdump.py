#!/usr/bin/env python3
"""patch_ramdump.py : add --ramdump FILE (write the SDRAM contents at exit) to main.cpp."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
nl = chr(92) + 'n'
p = os.path.join(root, 'emu/src/main.cpp'); s = open(p, encoding='utf-8', newline='').read()
old = """        else if (a == "--dante-fresh") danteFresh = true;"""
new = """        else if (a == "--dante-fresh") danteFresh = true;
        else if (a == "--ramdump" && i + 1 < argc) ramDumpPath = argv[++i];   // SDRAM image at exit (parameter-location hunting)"""
assert old in s; s = s.replace(old, new, 1)
old = """int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath;"""
new = """int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath, ramDumpPath;"""
assert old in s; s = s.replace(old, new, 1)
# at shutdown: before mach.shutdown() is called the first time
idx = s.index('mach.shutdown(')
line_start = s.rfind('\n', 0, idx) + 1
indent = s[line_start:idx]
ins = (indent + 'if (!ramDumpPath.empty()) { FILE* rf = fopen(ramDumpPath.c_str(), "wb"); if (rf) { fwrite(mach.bus.ramPtr(Machine::kSdramBase), 1, Machine::kSdramSize, rf); fclose(rf); fprintf(stderr, "[emu] SDRAM dumped to %s' + nl + '", ramDumpPath.c_str()); } }\n')
s = s[:line_start] + ins + s[line_start:]
open(p, 'w', encoding='utf-8', newline='').write(s)
print('ramdump option added')
