#!/bin/bash
# led-writes.sh [ERRFILE] : LED state array writes (watch on 0c764c81,78) as id:value@PC
F=${1:-/tmp/led.err}
grep '^\[watch\] wr' "$F" | sed -E 's/\[watch\] wr 0C764C([0-9A-F]+) size 1 val ([0-9A-F]+) at PC=([0-9A-F]+).*/\1 \2 \3/' | sort -u | python3 -c '
import sys
out=[]
for line in sys.stdin:
    a,v,pc=line.split(); out.append(f"{int(a,16)-0x81}:{v}@{pc}")
print(" ".join(out))' | fold -w 170
