#!/bin/bash
# audio-test-tones.sh [MAXINSNS] : spec item 69 - 32 unique tones (101*n Hz, -20 dBu) into the 32 sockets, capture the 32
# Dante TX channels, print the firmware's meter page and analyse which tone sits on which TX channel.
cd /mnt/d/rio3224-emu/emu
MAX=${1:-6400000000}
ARGS=""
for n in $(seq 1 32); do ARGS="$ARGS --tone $n:sine:$((101*n)):-20"; done
printf '29000 dsp\n29500 meter\n' > /tmp/tones-console.txt
cp ../fw/d2/flash.bin /tmp/tones-flash.bin
~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/tones-flash.bin --dante-boot-ms 15000 --panel --meter-print 5000 --trace-input 1 \
   --tx-out /tmp/tones-tx.wav --console-script /tmp/tones-console.txt $ARGS < /dev/null > /tmp/tones.raw 2> /tmp/tones.err
tr -d '\r' < /tmp/tones.raw | iconv -f SHIFT_JIS -t UTF-8 -c > /tmp/tones.txt
tail -3 /tmp/tones.err | cut -c1-160
grep -A26 -m1 "port met sig peak" /tmp/tones.txt
grep -A7 "^\[panel\]" /tmp/tones.err
grep "^\[audio\]" /tmp/tones.err | head -14 | cut -c1-220
cd /mnt/d/rio3224-emu && python3 tools/wav-analyse.py /tmp/tones-tx.wav 26 34
