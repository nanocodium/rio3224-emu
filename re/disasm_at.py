#!/usr/bin/env python3
"""Tiny SH-2A word dumper: prints raw opcodes at an address range of RIO2_PROG.BIN (use the emulator's --disasm for mnemonics)."""
import sys,struct
d=open(__file__.rsplit('/',1)[0]+'/../fw/d2/RIO2_PROG.BIN','rb').read()
a=int(sys.argv[1],16)&0x1FFFFFFF; n=int(sys.argv[2],0) if len(sys.argv)>2 else 32
off=a-0x0C0F0FF0
for i in range(0,n,2):
    print(f"{a+i:08X}: {struct.unpack('>H',d[off+i:off+i+2])[0]:04X}")
