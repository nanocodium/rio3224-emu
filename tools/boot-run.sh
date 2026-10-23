#!/bin/bash
# boot-run.sh MAXINSNS [extra rio-emu args...]  -> console in /tmp/boot.txt, stderr in /tmp/boot.err
cd /mnt/d/rio3224-emu/emu
MAX=$1; shift
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 600 ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/scratch-flash.bin "$@" < /dev/null > /tmp/boot.raw 2>/tmp/boot.err
tr -d '\r' < /tmp/boot.raw | iconv -f SHIFT_JIS -t UTF-8 2>/dev/null | grep -vE '^Assert|PNLSCAN' > /tmp/boot.txt
echo "=== console (${SKIP:-1}..): $(wc -l < /tmp/boot.txt) lines ==="
tail -n +${SKIP:-1} /tmp/boot.txt | head -${LINES_MAX:-120}
echo "=== stderr summary ==="
grep -E '^\[emu\] stopped' /tmp/boot.err
grep -E '^\[(FPGA|bus|PLD|watch)' /tmp/boot.err | sed -E 's/ at PC=.*//; s/insn [0-9]+//' | sort | uniq -c | sort -rn | head -${ERR_MAX:-30}
