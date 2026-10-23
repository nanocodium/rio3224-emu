#!/bin/bash
# long-run.sh MAXINSNS [args]: full run with the Dante model, DANTE log, OLED + panel dump; outputs in /tmp/lr.*
cd /mnt/d/rio3224-emu/emu
MAX=$1; shift
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 1200 ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/scratch-flash.bin --log DANTE --dante-boot-ms 15000 --oled-ascii --panel --oled /tmp/lr-oled.pgm "$@" < /dev/null > /tmp/lr.raw 2>/tmp/lr.err
tr -d '\r' < /tmp/lr.raw | iconv -f SHIFT_JIS -t UTF-8 2>/dev/null | grep -vE '^Assert|PNLSCAN' > /tmp/lr.txt
grep -E '^\[emu\] stopped|CPU FATAL' /tmp/lr.err
echo "=== console lines: $(wc -l < /tmp/lr.txt); last ${TAILN:-40}:"; tail -${TAILN:-40} /tmp/lr.txt
echo "=== frames:"; N=${N:-20} bash /mnt/d/rio3224-emu/tools/dante-frames.sh /tmp/lr.err
echo "=== OLED/panel:"; grep -A 60 '^\[oled\] panel' /tmp/lr.err | head -75
