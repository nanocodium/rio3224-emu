"""Collect real literal-pool values (mov.l @(disp,pc)) and movi20 immediates from an SH-2A image."""
import struct, sys, collections
path=sys.argv[1]; d=open(path,'rb').read()
HDR=0x10; BASE=0x0C0F1000
def u16(o): return struct.unpack('>H',d[o:o+2])[0]
lits=collections.Counter(); movi=collections.Counter()
fpu=0; trapa=collections.Counter(); sh2a_only=0; ldbank=0
for o in range(HDR, len(d)-1, 2):
    op=u16(o)
    pc=BASE+(o-HDR)
    if (op & 0xF000)==0xD000:          # mov.l @(disp8,pc),Rn
        disp=op&0xFF
        tgt=((pc+4)&~3)+disp*4
        fo=tgt-BASE+HDR
        if 0<=fo<len(d)-3:
            lits[struct.unpack('>I',d[fo:fo+4])[0]]+=1
    elif (op & 0xF00E)==0x0000 and (op & 0xF0FF)!=0x0000 and o+2<len(d):   # movi20 (low nibble 0) / movi20s (low nibble 1)
        imm=(((op>>4)&0xF)<<16) | u16(o+2)
        if imm & 0x80000: imm-=0x100000
        if (op & 1): imm<<=8
        movi[imm&0xffffffff]+=1
    if (op & 0xF000)==0xF000: fpu+=1
    if (op & 0xFF00)==0xC300: trapa[op&0xFF]+=1
    if (op & 0xF0FF) in (0x404B,0x406B): sh2a_only+=1   # jsr/n, rts/n? (rts/n = 0x006B)
    if op==0x006B: sh2a_only+=1
    if (op & 0xF0FF)==0x00E3: ldbank+=1
print(f"literal refs: {sum(lits.values())}, distinct {len(lits)}; movi20 refs {sum(movi.values())}; fpu-opcode words {fpu}; sh2a-only ops {sh2a_only}; ldbank {ldbank}")
print("TRAPA vectors:", dict(sorted(trapa.items())))
allv=lits+movi
regs={v:c for v,c in allv.items() if v>=0xFFF80000}
print(f"\n--- on-chip registers referenced ({len(regs)}) ---")
for v,c in sorted(regs.items()): print(f"  {v:#010x} x{c}")
ext={v:c for v,c in allv.items() if v<0x0C000000 and v>=0x1000}
print(f"\n--- below SDRAM (CS0..CS2 / internal) ({len(ext)}) grouped by 64K ---")
g=collections.Counter()
for v,c in ext.items(): g[v>>16]+=c
for k,c in sorted(g.items()): print(f"  {k<<16:#010x} refs={c}")
ext2={v:c for v,c in allv.items() if 0x10000000<=v<0xFFF80000}
g=collections.Counter()
for v,c in ext2.items(): g[v>>16]+=c
print(f"\n--- above SDRAM (CS4..CS7 / RAM / cache areas) grouped by 64K ---")
for k,c in sorted(g.items()): print(f"  {k<<16:#010x} refs={c}")
ram={v:c for v,c in allv.items() if 0x0C000000<=v<0x10000000}
g=collections.Counter()
for v,c in ram.items(): g[v>>20]+=c
print(f"\n--- SDRAM area by 1MB ---")
for k,c in sorted(g.items()): print(f"  {k<<20:#010x} refs={c}")
mx=max(ram); print("highest SDRAM literal:", hex(mx))
