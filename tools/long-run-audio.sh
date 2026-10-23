#!/bin/bash
# long-run-audio.sh [RUN_MS] : long-run stability test (spec item 79): the unit runs for RUN_MS of emulated time
# (default 10 minutes) with 32 test tones, its own TX looped onto RX 1-24, meters printed every minute and the Dante TX
# recorded for the last seconds; checks that the audio engine, the DIO lock and the firmware stay healthy (no CPU fatal,
# no DIO unlock, constant levels, no block loss).  Uses a copy of the binary so rebuilding during the run is harmless.
RUN=${1:-600000}
cp ~/rio3224/build/rio-emu /tmp/rio-emu-longrun
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/long-flash.bin startup=2 > /dev/null
ARGS=""; for n in $(seq 1 32); do ARGS="$ARGS --tone $n:sine:$((101*n)):-20"; done
PATCH=""; for n in $(seq 1 24); do PATCH="$PATCH$n@$n,"; done
START=$(date +%s)
/tmp/rio-emu-longrun --max 900000000000 --run-ms $RUN --no-raw --flash /tmp/long-flash.bin --dante-boot-ms 15000 --panel \
   --dante-patch "${PATCH%,}" --meter-print 60000 --tx-out /tmp/long-tx.wav $ARGS < /dev/null > /tmp/long.raw 2> /tmp/long.err
END=$(date +%s)
tr -d '\r' < /tmp/long.raw | iconv -f SHIFT_JIS -t UTF-8 -c > /tmp/long.txt
echo "=== $(grep -E '^\[emu\] (stopped|--run)|CPU FATAL' /tmp/long.err | head -1 | cut -c1-90)  wall $((END-START)) s"
echo "DIO state changes after lock: $(grep -c 'DIO_STATE' /tmp/long.txt)  errors occurred: $(grep -c 'OCCURRED' /tmp/long.txt)  asserts: $(grep -ci 'assert' /tmp/long.txt)"
grep -E 'OCCURRED|CLEARED|CPU|FATAL|Fatal' /tmp/long.txt | head -5
grep -E '^\[audio\] .*meters in' /tmp/long.err | awk '{print $2, $6, $7, $8, "...", $37}' | head -12
grep -E '^\[audio\] processed|^\[dante\]' /tmp/long.err | cut -c1-160
cd /mnt/d/rio3224-emu && python3 tools/wav-analyse.py /tmp/long-tx.wav $((RUN/1000-8)) $((RUN/1000-2)) | head -5
