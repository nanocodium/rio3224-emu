#!/usr/bin/env python3
"""patch_console_marker.py : print a '[console] <ms> ms > <text>' marker on stderr when a scripted console line is sent."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
nl = chr(92) + 'n'
cr = "'" + chr(92) + "r'"
p = os.path.join(root, 'emu/src/board/machine.cpp'); s = open(p, encoding='utf-8', newline='').read()
old = "{ consolePending += consoleScript[consolePos].text; consolePending += " + cr + "; consolePos++; }"
assert old in s, 'console feed line not found'
new = ("{ consolePending += consoleScript[consolePos].text; consolePending += " + cr +
       '; fprintf(stderr, "[console] %llu ms > %s' + nl + '", (unsigned long long)ms, consoleScript[consolePos].text.c_str()); consolePos++; }')
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print('marker added')
