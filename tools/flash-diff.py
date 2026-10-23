#!/usr/bin/env python3
"""flash-diff.py A B : list differing byte ranges between two flash images"""
import sys
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
n=min(len(a),len(b)); diffs=[i for i in range(n) if a[i]!=b[i]]
print('sizes',len(a),len(b),'diff bytes',len(diffs))
rs=[]
for i in diffs:
    if rs and i==rs[-1][1]+1: rs[-1][1]=i
    else: rs.append([i,i])
for s,e in rs[:40]: print('%06x-%06x (%d bytes): %s -> %s' % (s,e,e-s+1,a[s:min(e+1,s+16)].hex(),b[s:min(e+1,s+16)].hex()))
