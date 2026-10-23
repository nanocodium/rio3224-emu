#!/bin/bash
# dump-run.sh MAXINSNS ADDR,LEN [args] : boot and hex-dump emulated memory at exit
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 600 ~/rio3224/build/rio-emu --max $1 --no-raw --flash /tmp/scratch-flash.bin --dump $2 "${@:3}" < /dev/null 2>&1 >/dev/null | grep -E '^[0-9A-F]{8}:' 
