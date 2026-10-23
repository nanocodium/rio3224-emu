#!/bin/bash
# factory-init.sh [NAME] : exercise SETUP -> INITIALIZE -> HA PARAMETERS from the front panel on a RESUME flash whose
# input 1 has +20 dB saved: five DEC+INC presses reach SETUP, 21 INC presses walk the cursor to INITIALIZE (the first
# press only shows the cursor), the encoder selects HA PARAMETERS, the switch opens the confirmation, the switch again
# confirms.  OLED captures out/NAME-{a..f}.png; the GAIN page afterwards must show -6 dB everywhere.
NAME=${1:-finit}
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/$NAME-flash.bin startup=2 gain=1:20 gain=2:30 > /dev/null
ARGS="--oled-at 24500=/tmp/$NAME-a.pgm"; T=25000
for k in 1 2 3 4 5; do ARGS="$ARGS --press 3@$T-$((T+150)) --press 4@$T-$((T+150))"; T=$((T+1200)); done
ARGS="$ARGS --oled-at $((T+300))=/tmp/$NAME-b.pgm"; T=$((T+800))
for k in $(seq 1 21); do ARGS="$ARGS --press 4@$T-$((T+150))"; T=$((T+450)); done   # 21 fields: UNIT ID .. INITIALIZE (IP, MASK, GW have 4 octets each)
ARGS="$ARGS --oled-at $((T+300))=/tmp/$NAME-c.pgm --rotate 1@$((T+600)) --oled-at $((T+1500))=/tmp/$NAME-d.pgm --press 5@$((T+1800))-$((T+2100)) --oled-at $((T+3000))=/tmp/$NAME-e.pgm --press 3@$((T+3500))-$((T+3650)) --oled-at $((T+4200))=/tmp/$NAME-f.pgm --press 5@$((T+4500))-$((T+4800)) --oled-at $((T+7000))=/tmp/$NAME-g.pgm"
T=$((T+8000))
# back to GAIN: hold both 2.2 s twice (SETUP -> ... the higher menus), then capture
ARGS="$ARGS --press 3@$T-$((T+2300)) --press 4@$T-$((T+2300))"; T=$((T+3000))
printf '%d rep\n%d hgain\n' $T $((T+500)) > /tmp/$NAME-console.txt
END=$((T+3000))
timeout 1500 ~/rio3224/build/rio-emu --max 90000000000 --run-ms $END --no-raw --flash /tmp/$NAME-flash.bin --dante-boot-ms 15000 --oled /tmp/$NAME.pgm --panel $ARGS --console-script /tmp/$NAME-console.txt < /dev/null > /tmp/$NAME.raw 2>/tmp/$NAME.err
tr -d '\r' < /tmp/$NAME.raw | iconv -f SHIFT_JIS -t UTF-8 -c | grep -vE '^Assert|PNLSCAN' > /tmp/$NAME.txt
for f in /tmp/$NAME-*.pgm /tmp/$NAME.pgm; do b=$(basename $f .pgm); python3 /mnt/d/rio3224-emu/tools/pgm2png.py $f /mnt/d/rio3224-emu/out/$b.png > /dev/null 2>&1; done
echo "=== $NAME: $(grep -E '^\[emu\] (stopped|--run)|CPU FATAL' /tmp/$NAME.err | head -1 | cut -c1-80)"
grep -nE -i 'initiali|INIT' /tmp/$NAME.txt | tail -8 | cut -c1-120
grep -A1 'HA Gain' /tmp/$NAME.txt | tail -1
