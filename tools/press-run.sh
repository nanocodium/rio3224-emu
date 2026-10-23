#!/bin/bash
# press-run.sh NAME MAXINSNS [rio-emu args...] : full-model run; results in /tmp/NAME.{err,txt,pgm}, OLED PNG in out/NAME.png
NAME=$1; MAX=$2; shift 2
cd /mnt/d/rio3224-emu/emu
cp "${FLASH:-../fw/d2/flash.bin}" /tmp/$NAME-flash.bin   # FLASH=/path reuses an image from an earlier boot (configured unit)
timeout 1500 ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/$NAME-flash.bin --log DANTE --dante-boot-ms 15000 --oled /tmp/$NAME.pgm --panel "$@" < /dev/null > /tmp/$NAME.raw 2>/tmp/$NAME.err
tr -d '\r' < /tmp/$NAME.raw | iconv -f SHIFT_JIS -t UTF-8 2>/dev/null | grep -vE '^Assert|PNLSCAN' > /tmp/$NAME.txt
python3 /mnt/d/rio3224-emu/tools/pgm2png.py /tmp/$NAME.pgm /mnt/d/rio3224-emu/out/$NAME.png > /dev/null 2>&1
echo "=== $NAME: $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/$NAME.err | head -1)"
echo "    EVT: $(grep 'EVT' /tmp/$NAME.txt | tr '\n' '|')"
echo "    console lines $(wc -l < /tmp/$NAME.txt), last: $(tail -1 /tmp/$NAME.txt)"
