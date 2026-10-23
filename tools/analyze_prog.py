"""Static analysis helpers for RIO2_PROG.BIN (SH-2A big-endian image)."""
import struct, sys, re, collections
path = sys.argv[1]
d = open(path,'rb').read()
HDR = 0x10  # "V191 \0" + 0xff pad

# 1. all aligned 32-bit literal candidates
lits = collections.Counter()
for i in range(HDR, len(d)-3, 2):   # SH literal pools are 4-aligned relative to load addr; try both
    w = struct.unpack('>I', d[i:i+4])[0]
    lits[w] += 1

# 2. find load base by correlating string offsets with literal values
strs = []
for m in re.finditer(rb'[\x20-\x7e]{8,}\x00', d):
    strs.append(m.start())
cand = collections.Counter()
litset = set(lits)
for s in strs[:4000]:
    for base_hi in (0x0C000000, 0x0C100000, 0x0C200000, 0x0C300000, 0x0C400000, 0x0C800000, 0x0D000000, 0x08000000, 0x00000000, 0x1C000000):
        pass
# generic: for each string offset s, for each literal v with v>>24 == 0x0c, base = v - s; count
hits = collections.Counter()
lits_0c = [v for v in litset if (v >> 24) in (0x0c, 0x0d, 0x08, 0x1c, 0x00) and v != 0]
lits_0c_set = set(lits_0c)
for s in strs:
    for off in (0, -HDR):
        for v in lits_0c_set:
            pass
    break
# faster: build dict of literal values -> count; for each s test base candidates from a few literals
bases = collections.Counter()
sample_strs = strs[::max(1,len(strs)//600)]
for s in sample_strs:
    for v in lits_0c:
        b = v - s
        if b & 0xF == 0 and b >= 0:
            bases[b] += 1
print("load-base candidates (base = literal - string_file_offset):")
for b,c in bases.most_common(8):
    print(f"  {b:#010x}  score {c}")
BASE = bases.most_common(1)[0][0]

# 3. peripheral register usage: literals in 0xFFF80000..0xFFFFFFFF
regs = collections.Counter({v:c for v,c in lits.items() if v >= 0xFFF80000})
print(f"\n{len(regs)} distinct on-chip register addresses referenced (top 120):")
for v,c in sorted(regs.items())[:400]:
    print(f"  {v:#010x} x{c}")

# 4. other interesting ranges: CS areas 0x00000000-0x1FFFFFFF excl. SDRAM
ext = collections.Counter({v:c for v,c in lits.items() if (v >> 24) in (0x00,0x04,0x08,0x10,0x14,0x18,0x1c, 0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c) and c>=3 and v>0x1000})
print("\nexternal bus (CS0..CS7 window) literals with >=3 refs, grouped by 1MB:")
grp = collections.Counter()
for v,c in ext.items(): grp[v>>20] += c
for k,c in sorted(grp.items()):
    print(f"  {k<<20:#010x}..  refs={c}")
