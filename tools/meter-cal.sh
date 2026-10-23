#!/bin/bash
# meter-cal.sh NAME dBu1 dBu2 ... : boot (StartupMode 0 = REFRESH w/o MUTE flash copy) with sine tones at the given socket levels on
# inputs 1..n (others silent), hold LEFT+RIGHT at 25 s for the OLED meter page, read `dsp meter` at 30 s and the error
# list at 31.5 s; print the firmware's met/sig/peak bytes, the panel LEDs and the engine's own levels.
# Results /tmp/NAME.{txt,err}, OLED capture out/NAME.png.  MAX=insns overrides the run length.
NAME=$1; shift
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/$NAME-flash.bin startup=0 > /dev/null
ARGS=""; n=1
for lv in "$@"; do ARGS="$ARGS --tone $n:sine:$((100*n+1)):$lv"; n=$((n+1)); done
{ echo '29000 dsp'; echo '30000 meter'; echo '31500 /err info'; } > /tmp/$NAME-console.txt
timeout 1500 ~/rio3224/build/rio-emu --max ${MAX:-8200000000} --no-raw --flash /tmp/$NAME-flash.bin --dante-boot-ms 15000 --oled /tmp/$NAME.pgm --panel \
   --press 3@25000-27200 --press 4@25000-27200 --console-script /tmp/$NAME-console.txt $ARGS < /dev/null > /tmp/$NAME.raw 2>/tmp/$NAME.err
tr -d '\r' < /tmp/$NAME.raw | iconv -f SHIFT_JIS -t UTF-8 -c | grep -vE '^Assert|PNLSCAN' > /tmp/$NAME.txt
python3 /mnt/d/rio3224-emu/tools/pgm2png.py /tmp/$NAME.pgm /mnt/d/rio3224-emu/out/$NAME.png > /dev/null 2>&1
echo "=== $NAME: $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/$NAME.err | head -1 | cut -c1-80)"
grep -A26 -m1 "port met sig peak" /tmp/$NAME.txt
grep -A12 -m1 "err info" /tmp/$NAME.txt
grep -A6 "^\[panel\]" /tmp/$NAME.err | tail -6
grep "^\[audio\] in" /tmp/$NAME.err | head -$((n-1)) | cut -c1-150
