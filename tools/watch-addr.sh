#!/bin/bash
# watch-addr.sh ADDRFILE [LEN] [MAX] : boot once with --watch on the hex address stored in ADDRFILE and print the writes
cd /mnt/d/rio3224-emu/emu
A=$(cat "$1"); LEN=${2:-4}
cp ../fw/d2/flash.bin /tmp/wa-flash.bin
~/rio3224/build/rio-emu --max ${MAX:-5000000000} --no-raw --flash /tmp/wa-flash.bin --dante-boot-ms 15000 --watch "$A,$LEN" --watch-max ${3:-30} < /dev/null > /dev/null 2> /tmp/wa.err
grep '^\[watch\] wr' /tmp/wa.err | head -40
