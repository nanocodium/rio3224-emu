#!/bin/bash
# Unpack the Audinate .dnt images (run as root inside WSL: wsl -u root bash tools/extract-dante.sh)
set -u
cd "$(dirname "$0")/../fw/d2" || exit 1
F3=FWUpdate_Rio3224-D2_v4020503_sw501.dnt
F2=FWUpdate_Rio3224-D2_v4020401_sw310.dnt
X3=bk3/_${F3}.extracted
X2=bk2/_${F2}.extracted
[ -d "$X3/squashfs-root" ] || { rm -rf bk3; mkdir -p bk3; binwalk -e -q --run-as=root -C bk3 "$F3" >/dev/null 2>&1; }
# Brooklyn III: carve DTBs, kernel, initramfs, bitstream
dd if="$F3" of="$X3/uboot.dtb"  bs=1M iflag=skip_bytes,count_bytes skip=$((0x56FDC)) count=9972  status=none
dd if="$F3" of="$X3/kernel.dtb" bs=1M iflag=skip_bytes,count_bytes skip=$((0x3B3670)) count=12325 status=none
dtc -I dtb -O dts -o "$X3/kernel.dts" "$X3/kernel.dtb" 2>/dev/null
dtc -I dtb -O dts -o "$X3/uboot.dts"  "$X3/uboot.dtb"  2>/dev/null
# uImage headers: 64 bytes then payload
dd if="$F3" of="$X3/u-boot.bin"      bs=1M iflag=skip_bytes,count_bytes skip=$((0xB0+64)) count=366048  status=none
dd if="$F3" of="$X3/zImage"          bs=1M iflag=skip_bytes,count_bytes skip=$((0x3C3670+64)) count=3002880 status=none
dd if="$F3" of="$X3/initramfs.cpio.gz" bs=1M iflag=skip_bytes,count_bytes skip=$((0x9C3670+64)) count=6954774 status=none
dd if="$F3" of="$X3/rootfs.squashfs" bs=1M iflag=skip_bytes,count_bytes skip=$((0x106ED6C)) status=none
# bitstream: find the Xilinx header 'a' field just before sync word
python3 - "$F3" "$X3" <<'PY'
import sys,struct
d=open(sys.argv[1],'rb').read(); out=sys.argv[2]
sync=d.find(bytes.fromhex('FFFFFFFF000000BB11220044FFFFFFFFFFFFFFFFAA995566'))
print("bitstream sync @", hex(sync))
# search backwards for the .bit header magic 00 09 0f f0 0f f0 0f f0 0f f0 00 00 01 'a'
hdr=d.rfind(bytes.fromhex('00090ff00ff00ff00ff000000001'), max(0,sync-0x1000), sync)
if hdr>=0:
    p=hdr+13
    fields={}
    while True:
        k=d[p:p+1].decode(); p+=1
        if k=='e':
            ln=struct.unpack('>I',d[p:p+4])[0]; p+=4; fields['e_len']=ln; break
        ln=struct.unpack('>H',d[p:p+2])[0]; p+=2
        fields[k]=d[p:p+ln].rstrip(b'\0').decode('latin1'); p+=ln
    print("bit header:", fields)
    open(out+'/pl.bit','wb').write(d[hdr:p+fields['e_len']])
else:
    print("no .bit header found; raw bitstream from sync-? not carved")
PY
mkdir -p "$X3/initramfs" && (cd "$X3/initramfs" && gunzip -c ../initramfs.cpio.gz | cpio -idm --no-absolute-filenames 2>/dev/null)
echo "=== kernel.dts ==="; cat "$X3/kernel.dts"
echo; echo "=== uboot.dts (summary) ==="; grep -E "model|compatible|bootargs|reg = " "$X3/uboot.dts" | head -40
echo; echo "=== initramfs top ==="; (cd "$X3/initramfs" && find . -maxdepth 2 | sort | head -100)
echo; echo "=== squashfs-root ==="; find "$X3/squashfs-root" | sort | head -200
# Brooklyn II cramfs
echo; echo "=== BK2 cramfs ==="
for c in "$X2"/*.cramfs; do
  out="${c%.cramfs}.root"; rm -rf "$out"; mkdir -p "$out"
  if command -v fsck.cramfs >/dev/null; then fsck.cramfs -x "$out" "$c" >/dev/null 2>&1 && echo "extracted $c with fsck.cramfs"; fi
  [ -n "$(ls -A "$out")" ] || { command -v 7z >/dev/null && 7z x -y -o"$out" "$c" >/dev/null 2>&1 && echo "extracted $c with 7z"; }
  find "$out" | sort | head -120
done
