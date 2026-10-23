#!/usr/bin/env python3
"""flow.py DUMPFILE FUNC [FUNC...] : print the branch/call/compare/immediate-move lines of a function from a gdb
'disassemble /s' dump (control-flow skeleton)."""
import sys, re
txt = open(sys.argv[1], errors='replace').read()
for fn in sys.argv[2:]:
    m = re.search(r'Dump of assembler code for function %s:\n(.*?)End of assembler dump' % re.escape(fn), txt, re.S)
    if not m: print(f"## {fn}: not found"); continue
    print(f"## {fn}")
    for line in m.group(1).split('\n'):
        if re.search(r'\t(bl|b|beq|bne|blt|bge|bgt|ble|bls|bhi|bcc|bcs|bmi|bpl|cmp|cmpne|mov[a-z]*\tr[0-3], #|movw\tr[0-3])\b', line) or 'ldrb\tr3, [sp' in line:
            print(line.rstrip())
