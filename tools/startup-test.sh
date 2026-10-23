#!/bin/bash
# startup-test.sh MODE [MAXINSNS] : boot from a flash copy whose saved SETUP Dev/UI/StartupMode is MODE (0 REFRESH w/o
# MUTE, 1 REFRESH = factory default, 2 RESUME; names as the SETUP page shows them), with 32 tones at the sockets, and report what the firmware does with the DIO mute,
# the meter port and the parameter readback.  Results: /tmp/su$MODE.{txt,err}, OLED out/su$MODE.png
MODE=$1; MAX=${2:-9000000000}; N=su$MODE
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/$N-flash.bin startup=$MODE
ARGS=""
for n in $(seq 1 32); do ARGS="$ARGS --tone $n:sine:$((101*n)):-20"; done
printf '27000 mms\n27500 getprm SETUP Dev/UI/StartupMode 0 0\n28500 /rep current\n30000 /dio state\n31500 /dsp meter\n' > /tmp/$N-console.txt
timeout 1500 ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/$N-flash.bin --log DANTE --dante-boot-ms 15000 --oled /tmp/$N.pgm --panel \
   --meter-print 5000 --console-script /tmp/$N-console.txt $ARGS "${@:3}" < /dev/null > /tmp/$N.raw 2>/tmp/$N.err
tr -d '\r' < /tmp/$N.raw | iconv -f SHIFT_JIS -t UTF-8 -c | grep -vE '^Assert|PNLSCAN' > /tmp/$N.txt
python3 /mnt/d/rio3224-emu/tools/pgm2png.py /tmp/$N.pgm /mnt/d/rio3224-emu/out/$N.png > /dev/null 2>&1
echo "=== $N: $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/$N.err | head -1)"
echo "--- DIO / mute lines:"; grep -nE 'DIO:|Mute|mute|sound on|StartupMode|E0[0-9]-' /tmp/$N.txt | head -40
echo "--- console answers:"; grep -nE -A3 '^> ' /tmp/$N.txt | head -80
echo "--- panel:"; grep -A7 "^\[panel\]" /tmp/$N.err | tail -8
echo "--- meter reads by firmware:"; grep -c 'meterRead' /tmp/$N.err; grep "^\[audio\]" /tmp/$N.err | tail -6 | cut -c1-200
