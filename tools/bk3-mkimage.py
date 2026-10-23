"""bk3-mkimage.py — build a QEMU-sized (16 MB) QSPI flash image and a matching device tree for the
Brooklyn III Dante module.  The real module has a 64 MB flash; QEMU's xilinx-zynq-a9 QSPI model is 16 MB,
so the partition table is rewritten (kernel/initramfs are loaded by QEMU directly, fpga images are unused).

usage: python bk3-mkimage.py <extracted .dnt dir> <out dir>
writes <out>/kernel-qemu.dts (compile with dtc) and <out>/bk3flash.img
"""
import re, sys, os, struct

# name: (offset, size)   -- all inside 16 MB
LAYOUT = {
    'skd':         (0x000000, 0x040000),
    'fsbl':        (0x040000, 0x040000),
    'safefpga':    (0x080000, 0x010000),
    'safeboot':    (0x090000, 0x010000),
    'safebootenv': (0x0A0000, 0x010000),
    'fpga':        (0x0B0000, 0x010000),
    'boot':        (0x0C0000, 0x010000),
    'bootenv':     (0x0D0000, 0x010000),
    'kernel':      (0x100000, 0x100000),
    'rootfs':      (0x200000, 0x400000),
    'userarea':    (0x600000, 0x400000),
    'brdinfo':     (0xA00000, 0x010000),
    'license':     (0xA10000, 0x010000),
    'safelicense': (0xA20000, 0x010000),
    'capability':  (0xA30000, 0x010000),
    'config':      (0xA40000, 0x020000),
    'flashall':    (0x000000, 0x1000000),
    'image':       (0x100000, 0x500000),
}
FLASH_SIZE = 16 << 20

def patch_dts(src, dst):
    text = open(src).read()
    out = []
    cur = None
    for line in text.splitlines():
        m = re.match(r'\s*partition@(\w+)\s*{', line)
        if m: cur = m.group(1)
        if cur and re.match(r'\s*reg = <', line) and cur in LAYOUT:
            off, size = LAYOUT[cur]
            line = re.sub(r'reg = <[^>]*>', 'reg = <0x%x 0x%x>' % (off, size), line)
        if re.match(r'\s*};', line) and cur: cur = None
        if 'stdout-path' in line:            # no kernel console on the dac UART
            continue
        out.append(line)
    open(dst, 'w').write('\n'.join(out) + '\n')

def build_flash(srcdir, dst, squashfs):
    img = bytearray(b'\xff' * FLASH_SIZE)
    sq = open(squashfs, 'rb').read()
    for part in ('rootfs', 'userarea'):
        off, size = LAYOUT[part]
        assert len(sq) <= size, part
        img[off:off + len(sq)] = sq
    open(dst, 'wb').write(img)
    return len(sq)

if __name__ == '__main__':
    srcdir, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    patch_dts(os.path.join(srcdir, 'kernel.dts'), os.path.join(outdir, 'kernel-qemu.dts'))
    n = build_flash(srcdir, os.path.join(outdir, 'bk3flash.img'), os.path.join(srcdir, 'rootfs.squashfs'))
    print('wrote %s/kernel-qemu.dts and %s/bk3flash.img (squashfs %d bytes at rootfs/userarea)' % (outdir, outdir, n))
