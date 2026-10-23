#!/bin/bash
# full-debug-run.sh [seconds] : host emulator + Brooklyn module with UART A bridged (dac link, TCP 5550); the
# module's UART B carries periodic diagnostic dumps (initramfs-diagB) captured to /tmp/fd-s1.txt.
# Logs: /tmp/fd-host.{txt,err}, /tmp/fd-bk3.log (qemu stderr), /tmp/fd-s1.txt (module diagnostics)
SECS=${1:-200}
Q=/mnt/d/rio3224-emu/fw/d2/bk3/qemu
D=/mnt/d/rio3224-emu/fw/d2/bk3/_FWUpdate_Rio3224-D2_v4020503_sw501.dnt.extracted
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
rm -f /tmp/fd-s1.txt
( sleep $((SECS+15)); printf '\035' ) | ~/rio3224/build/rio-emu --no-raw --flash /tmp/scratch-flash.bin --dnt-uart 5550,0 --log DNTUART \
    --dante-clock-ms ${CLOCK_MS:-60000} --panel ${HOST_ARGS} > /tmp/fd-host.raw 2>/tmp/fd-host.err &
HOSTPID=$!
sleep 3
timeout $SECS qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -monitor none \
  -kernel $D/zImage -dtb $Q/kernel-qemu.dtb -initrd $Q/initramfs-diagB.cpio.gz -drive if=mtd,unit=8,format=raw,file=$Q/bk3flash.img \
  -append "console=ttynull quiet root=/dev/ram0 rdinit=/init" \
  -serial tcp:127.0.0.1:5550,nodelay -serial file:/tmp/fd-s1.txt > /tmp/fd-bk3.log 2>&1 < /dev/null
wait $HOSTPID
tr -d '\r' < /tmp/fd-host.raw | iconv -f SHIFT_JIS -t UTF-8 2>/dev/null | grep -vE '^Assert|PNLSCAN' > /tmp/fd-host.txt
echo "=== module diagnostics (last dump) ==="; tr -d '\r' < /tmp/fd-s1.txt | awk '/=== DIAG/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' | head -${TAIL:-90}
echo "=== host console (tail) ==="; tail -${HTAIL:-12} /tmp/fd-host.txt
bash /mnt/d/rio3224-emu/tools/dnt-bytes.sh /tmp/fd-host.err | head -8
echo "=== qemu stderr ==="; tail -3 /tmp/fd-bk3.log
