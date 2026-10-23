#!/bin/bash
# setup-edit.sh NAME [ROTATE_STEPS] : change START UP MODE from the front panel on the factory flash: five DEC+INC
# presses reach SETUP, INC twice moves the cursor to START UP MODE, the encoder turns ROTATE_STEPS (default 1, +1 =
# REFRESH -> RESUME) and the encoder switch confirms.  The firmware saves only in its power-off sequence:
#   PWRSEQ=1   run `misc pwrseq` from the debug monitor at 40 s
#   POWEROFF=1 drop /UVLO at 40 s (PSU model); HOLD=ms sets the supply hold-up time (default 3000)
# The flash copy is written back at shutdown and listed with flash-records.py.  out/NAME-{0..9}.png show each step.
NAME=$1; STEPS=${2:-1}
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/$NAME-flash.bin
ARGS=""; T=25000
for k in 1 2 3 4 5; do ARGS="$ARGS --press 3@$T-$((T+150)) --press 4@$T-$((T+150)) --oled-at $((T+900))=/tmp/$NAME-$k.pgm"; T=$((T+1200)); done
ARGS="$ARGS --press 4@32000-32300 --press 4@32700-33000 --oled-at 33200=/tmp/$NAME-6.pgm --rotate $STEPS@33500 --oled-at 34400=/tmp/$NAME-7.pgm --press 5@35000-35300 --oled-at 36000=/tmp/$NAME-8.pgm --oled-at 42000=/tmp/$NAME-9.pgm"
if [ -n "$PWRSEQ" ]; then { echo "39000 misc"; echo "40000 pwrseq"; } > /tmp/$NAME-console.txt; ARGS="$ARGS --console-script /tmp/$NAME-console.txt"; fi
if [ -n "$POWEROFF" ]; then ARGS="$ARGS --power-off 40000,${HOLD:-3000}"; fi
timeout 1500 ~/rio3224/build/rio-emu --max ${MAX:-10000000000} --no-raw --flash /tmp/$NAME-flash.bin --dante-boot-ms 15000 --oled /tmp/$NAME.pgm --panel $ARGS < /dev/null > /tmp/$NAME.raw 2>/tmp/$NAME.err
tr -d '\r' < /tmp/$NAME.raw | iconv -f SHIFT_JIS -t UTF-8 -c | grep -vE '^Assert|PNLSCAN' > /tmp/$NAME.txt
for f in /tmp/$NAME-*.pgm /tmp/$NAME.pgm; do b=$(basename $f .pgm); python3 /mnt/d/rio3224-emu/tools/pgm2png.py $f /mnt/d/rio3224-emu/out/$b.png > /dev/null 2>&1; done
echo "=== $NAME: $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/$NAME.err | head -1 | cut -c1-80)"
grep -E '^\[flash\]|^\[pld\]' /tmp/$NAME.err | head -12
echo "power-off sequences run: $(grep -c 'DIO: Power off return' /tmp/$NAME.txt)"
cd /mnt/d/rio3224-emu && python3 tools/flash-records.py /tmp/$NAME-flash.bin
