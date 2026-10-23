"""pgm2png.py IN.pgm OUT.png [scale] - convert a binary 8-bit PGM to PNG (no PIL needed), optional integer upscale."""
import sys, zlib, struct
def read_pgm(p):
    d = open(p, 'rb').read()
    parts = d.split(maxsplit=4)
    assert parts[0] == b'P5'
    w, h, mx = int(parts[1]), int(parts[2]), int(parts[3])
    px = d[len(d) - w * h:]
    return w, h, px
def write_png(p, w, h, px, scale):
    W, H = w * scale, h * scale
    raw = bytearray()
    for y in range(H):
        row = px[(y // scale) * w:(y // scale + 1) * w]
        line = bytearray()
        for v in row: line += bytes([v]) * scale
        raw += b'\x00' + line
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    open(p, 'wb').write(png)
w, h, px = read_pgm(sys.argv[1]); write_png(sys.argv[2], w, h, px, int(sys.argv[3]) if len(sys.argv) > 3 else 1)
print(f"{sys.argv[2]}: {w}x{h}")
