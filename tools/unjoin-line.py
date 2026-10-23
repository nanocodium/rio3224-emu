#!/usr/bin/env python3
"""unjoin-line.py FILE MARKER : split a source line at a literal backslash-n that precedes MARKER (repair helper)."""
import sys
path, marker = sys.argv[1], sys.argv[2]
s = open(path, encoding='utf-8').read()
bad = chr(92) + 'n' + marker
n = s.count(bad)
s = s.replace(bad, '\n' + marker)
open(path, 'w', encoding='utf-8', newline='\n').write(s)
print('%s: split %d occurrence(s)' % (path, n))
