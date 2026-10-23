#!/bin/bash
# dump the HI7000/4 interrupt dispatch table (pointer at 0x0C5053BC) after MAX instructions
MAX=${1:-400000}
cd /mnt/d/rio3224-emu/emu
P=$(timeout 120 ~/rio3224/build/rio-emu --max $MAX --no-raw --no-unmapped --dump 0C5053BC,10 < /dev/null 2>&1 | grep '^0C5053BC:' | awk '{print $2}')
echo "dispatch table at $P"
timeout 120 ~/rio3224/build/rio-emu --max $MAX --no-raw --no-unmapped --dump $P,800 < /dev/null 2>&1 | grep -E '^[0-9A-F]{8}:' | python3 -c '
import sys
ents=[]
for line in sys.stdin:
    p=line.split(); base=int(p[0].rstrip(":"),16)
    for i,w in enumerate(p[1:]): ents.append((base+i*4,int(w,16)))
from collections import Counter
c=Counter(v for a,v in ents); default=c.most_common(1)[0][0]
print(f"default handler {default:08x} ({c[default]} entries); non-default vectors:")
t0=ents[0][0]
for a,v in ents:
    if v!=default: print(f"  vec {(a-t0)//4:3d}  -> {v:08x}")
'
