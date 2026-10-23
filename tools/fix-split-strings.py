#!/usr/bin/env python3
"""fix-split-strings.py FILE... : repair C++ lines where a string literal was broken by a raw newline (an editing
accident): a line with an odd number of unescaped double quotes is joined with the following line using a \\n escape."""
import sys, re
BS = chr(92)
for path in sys.argv[1:]:
    lines = open(path, encoding='utf-8').read().split('\n'); out = []; i = 0; fixed = 0
    while i < len(lines):
        l = lines[i]
        body = re.sub(r"'(?:\\.|[^'\\])'", "''", l)          # drop char literals such as '"'
        if len(re.findall(r'(?<!' + BS + BS + ')"', body)) % 2 == 1 and i + 1 < len(lines) and not l.lstrip().startswith('//'):
            l = l + BS + 'n' + lines[i + 1]; i += 1; fixed += 1
        out.append(l); i += 1
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('%s: joined %d lines' % (path, fixed))
