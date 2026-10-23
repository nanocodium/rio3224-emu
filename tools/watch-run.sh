#!/bin/bash
# watch-run.sh MAXINSNS ADDR,LEN : boot and list distinct accesses (addr dir PC value) to the watched range
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 600 ~/rio3224/build/rio-emu --max $1 --no-raw --flash /tmp/scratch-flash.bin --watch $2 "${@:3}" < /dev/null > /dev/null 2>/tmp/watch.err
grep -E '^\[watch\] wr' /tmp/watch.err | sed -E 's/\[watch\] (rd|wr) ([0-9A-F]+) size ([0-9]) val ([0-9A-F]+) at PC=([0-9A-F]+) insn ([0-9]+)/\2 \1 PC=\5 val=\4 sz\3 insn=\6/' | head -${LINES_MAX:-80}
