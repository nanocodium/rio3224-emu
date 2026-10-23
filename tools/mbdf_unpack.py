import sys, struct, zlib, os
def parse(path, outdir):
    d = open(path,'rb').read()
    assert d[:19] == b'#YAMAHA MBDFArchive'
    hlen = struct.unpack('>I', d[0x18:0x1c])[0]
    hdr = d[0x24:0x24+hlen]
    print("archive hdr len", hlen, "type", hdr[:4], "model", hdr[4:0x11].rstrip(b'\0'), "ver bytes", hdr[0x11:0x13].hex(), "rest", hdr[0x13:].hex())
    off = 0x24 + hlen
    i = 0
    while off < len(d):
        assert d[off:off+5] == b'#FILE', (hex(off), d[off:off+16])
        A, size, B = struct.unpack('>III', d[off+0x18:off+0x24])
        nlen, C = struct.unpack('>HH', d[off+0x24:off+0x28])
        D = struct.unpack('>I', d[off+0x28:off+0x2c])[0]
        name = d[off+0x2c:off+0x2c+nlen].decode('latin1')
        npad = (nlen + 3) & ~3
        data_off = off + 0x2c + npad
        data = d[data_off:data_off+size]
        print(f"[{i}] off={off:#x} A={A:#x} size={size} B={B:#x} nlen={nlen} C={C:#x} D={D:#x} name={name!r} data@{data_off:#x} first={data[:4].hex()}")
        if size:
            out = os.path.join(outdir, name.replace('/', os.sep))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            try:
                raw = zlib.decompress(data)
                print(f"      zlib -> {len(raw)} bytes")
            except Exception as e:
                print(f"      not zlib ({e}); raw")
                raw = data
            open(out,'wb').write(raw)
        off = (data_off + size + 3) & ~3
        if d[off:off+4] == b"#END":
            print("END at", hex(off), "trailer:", d[off:off+64].hex()); break
        i += 1
parse(sys.argv[1], sys.argv[2])
