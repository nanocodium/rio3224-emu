#!/usr/bin/env python3
"""Print the decompiled function containing each given address (from RIO2_PROG.decomp.c)."""
import sys, re, bisect, os
here=os.path.dirname(os.path.abspath(__file__))
lines=open(os.path.join(here,'RIO2_PROG.decomp.c'),encoding='utf-8',errors='replace').read().split('\n')
starts=[]; 
for i,l in enumerate(lines):
    m=re.match(r'// ==== ([0-9a-f]{8}) (\S+)',l)
    if m: starts.append((int(m.group(1),16),i,m.group(2)))
addrs=[a for a,_,_ in starts]
for arg in sys.argv[1:]:
    a=int(arg,16)&0x1FFFFFFF if int(arg,16)>=0x20000000 and int(arg,16)<0x40000000 else int(arg,16)
    k=bisect.bisect_right(addrs,a)-1
    if k<0: print("no function for",arg); continue
    st=starts[k][1]; en=starts[k+1][1] if k+1<len(starts) else len(lines)
    print(f"##### {arg} is in {starts[k][2]} @ {starts[k][0]:08x} (lines {st}-{en})")
    print('\n'.join(lines[st:min(en,st+int(os.environ.get('MAXL','120')))]))
