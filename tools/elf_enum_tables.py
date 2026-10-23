"""elf_enum_tables.py <arm32 ELF> : find {uint32 value, char* name} enum tables (module_config *_STRINGS) in an ELF."""
import struct, sys, re
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'\x7fELF' and d[4] == 1 and d[5] == 1   # ELF32 LE
e_phoff, e_phentsize, e_phnum = struct.unpack_from('<I', d, 28)[0], struct.unpack_from('<H', d, 42)[0], struct.unpack_from('<H', d, 44)[0]
segs = []
for i in range(e_phnum):
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from('<IIIIII', d, e_phoff + i * e_phentsize)[:6]
    if p_type == 1: segs.append((p_vaddr, p_offset, p_filesz))
def v2o(v):
    for va, off, sz in segs:
        if va <= v < va + sz: return off + (v - va)
    return None
def cstr(v):
    o = v2o(v)
    if o is None: return None
    e = d.find(b'\0', o)
    s = d[o:e]
    return s.decode() if 0 < len(s) < 64 and all(32 <= c < 127 for c in s) else None
# scan all loadable data for 8-byte pairs (value, name ptr) where name matches *_[A-Z]+ enum style, grouped by prefix
tables = {}
for va, off, sz in segs:
    for o in range(off, off + sz - 8, 4):
        val, ptr = struct.unpack_from('<II', d, o)
        if val > 0x10000: continue
        s = cstr(ptr)
        if s and re.match(r'^[A-Z][A-Z0-9]+(_[A-Z0-9_]+)+$', s) and not s.endswith('_STRINGS'):
            prefix = s.rsplit('_', 1)[0]
            tables.setdefault((va + (o - off)) & ~0xFFF, []).append((va + (o - off), val, s))
for page in sorted(tables):
    for addr, val, s in tables[page]:
        print(f"{addr:08x}: {val:3d} = {s}")
