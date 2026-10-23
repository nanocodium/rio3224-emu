#!/bin/bash
# menu-walk.sh NAME N [extra rio-emu args] : from the GAIN home page press DEC+INC together N times (each press
# selects the next-lower menu: +48V ON, HPF ON, HPF F, GC ON, SETUP, INFORMATION per the owner's manual), capturing
# the OLED after each step into out/NAME-k.png; the final state also goes to out/NAME.png.
NAME=$1; N=$2; shift 2
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/$NAME-flash.bin startup=${STARTUP:-0} $FLASHOPS > /dev/null   # STARTUP=N FLASHOPS="gain=1:20 ..." env overrides
ARGS="--oled-at 24500=/tmp/$NAME-0.pgm"; T=25000
for k in $(seq 1 $N); do ARGS="$ARGS --press 3@$T-$((T+150)) --press 4@$T-$((T+150)) --oled-at $((T+900))=/tmp/$NAME-$k.pgm"; T=$((T+1200)); done
timeout 1500 ~/rio3224/build/rio-emu --max ${MAX:-$((5000000000 + N * 400000000))} --no-raw --flash /tmp/$NAME-flash.bin --dante-boot-ms 15000 --oled /tmp/$NAME.pgm --panel $ARGS "$@" < /dev/null > /tmp/$NAME.raw 2>/tmp/$NAME.err
tr -d '\r' < /tmp/$NAME.raw | iconv -f SHIFT_JIS -t UTF-8 -c | grep -vE '^Assert|PNLSCAN' > /tmp/$NAME.txt
for f in /tmp/$NAME-*.pgm /tmp/$NAME.pgm; do b=$(basename $f .pgm); python3 /mnt/d/rio3224-emu/tools/pgm2png.py $f /mnt/d/rio3224-emu/out/$b.png > /dev/null 2>&1; done
echo "=== $NAME: $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/$NAME.err | head -1 | cut -c1-80)"
grep -E 'oled-at|\[oled\]' /tmp/$NAME.err | head
