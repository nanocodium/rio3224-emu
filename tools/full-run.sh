#!/bin/bash
# full-run.sh [seconds] : run the host emulator and the Brooklyn III module together, UARTs bridged over TCP.
#   host console -> /tmp/full-host.txt, host stderr -> /tmp/full-host.err, module boot log -> /tmp/full-bk3.log
#   CLOCK_MS: when the module's word clock appears (default 60000 ms, roughly when dac is up); HOST_ARGS: extra rio-emu args
SECS=${1:-170}
Q=/mnt/d/rio3224-emu/fw/d2/bk3/qemu
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
( sleep $((SECS+15)); printf '\035' ) | ~/rio3224/build/rio-emu --no-raw --flash /tmp/scratch-flash.bin --dnt-uart 5550,5551 --log DNTUART \
    --dante-clock-ms ${CLOCK_MS:-60000} --oled /tmp/full-oled.pgm --panel ${HOST_ARGS} > /tmp/full-host.raw 2>/tmp/full-host.err &
HOSTPID=$!
sleep 3
BK3_UART=tcp INITRD=$Q/initramfs-qemu.cpio.gz bash /mnt/d/rio3224-emu/tools/brooklyn-run.sh $SECS > /tmp/full-bk3.log 2>&1 < /dev/null &
QPID=$!
wait $QPID; wait $HOSTPID
tr -d '\r' < /tmp/full-host.raw | iconv -f SHIFT_JIS -t UTF-8 2>/dev/null | grep -vE '^Assert|PNLSCAN' > /tmp/full-host.txt
echo "=== host console (tail) ==="; tail -${TAIL:-40} /tmp/full-host.txt
echo "=== host stderr (DNT UART / CS5 lines) ==="; grep -E "^\[CS5\]|client" /tmp/full-host.err | head -10
bash /mnt/d/rio3224-emu/tools/dnt-bytes.sh /tmp/full-host.err
echo "=== module log tail ==="; tail -5 /tmp/full-bk3.log
