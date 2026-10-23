#!/bin/bash
# dante-net-test.sh [RUN_MS] : two emulator instances on the UDP virtual Dante network (spec items: Dante transport,
# subscriptions, latency, redundancy).  RioA plays 1 kHz / 2.5 kHz test tones into its inputs 1-2; RioB subscribes its
# RX 1-2 to RioA's TX 1-2 and records its analog outputs.  Both run in real time from RESUME-mode flash copies so the
# firmware unmutes without a console.  With REDUNDANT=1 both instances use a secondary network too and RioA's primary
# "cable" is pulled from 30 s to 34 s (the audio must continue via the secondary network with no loss).
RUN=${1:-40000}
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/vdA-flash.bin startup=2 > /dev/null
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/vdB-flash.bin startup=2 unitid=2 > /dev/null
{ echo "35000 rep"; echo "36000 current"; } > /tmp/vdB-console.txt
NET2=""; FAIL=""
if [ -n "$REDUNDANT" ]; then NET2="--dante-net2"; FAIL="--dante-fail primary@30000-34000"; fi
~/rio3224/build/rio-emu --max 30000000000 --run-ms $RUN --realtime --no-raw --flash /tmp/vdA-flash.bin --dante-boot-ms 15000 \
   --dante-name RioA --dante-net $NET2 $FAIL --tone 1:sine:1000:-20 --tone 2:sine:2500:-20 --tx-out /tmp/vdA-tx.wav < /dev/null > /tmp/vdA.raw 2> /tmp/vdA.err &
PA=$!
~/rio3224/build/rio-emu --max 30000000000 --run-ms $RUN --realtime --no-raw --flash /tmp/vdB-flash.bin --dante-boot-ms 15000 --mac 00:a0:de:00:00:02 \
   --dante-name RioB --dante-net $NET2 --log DANTE --dante-clock follower --dante-patch "1@RioA:1,2@RioA:2" --audio-out /tmp/vdB-out.wav --meter-print 10000 --console-script /tmp/vdB-console.txt < /dev/null > /tmp/vdB.raw 2> /tmp/vdB.err &
PB=$!
wait $PA $PB
for u in A B; do tr -d '\r' < /tmp/vd$u.raw | iconv -f SHIFT_JIS -t UTF-8 -c > /tmp/vd$u.txt; done
echo "=== RioA: $(grep -E '^\[emu\] (stopped|--run)' /tmp/vdA.err | head -1 | cut -c1-70)"; grep -E '^\[dante\]|^\[audio\] processed' /tmp/vdA.err | cut -c1-200
echo "=== RioB: $(grep -E '^\[emu\] (stopped|--run)' /tmp/vdB.err | head -1 | cut -c1-70)"; grep -E '^\[dante\]|^\[audio\] processed|sound on' /tmp/vdB.err /tmp/vdB.txt | cut -c1-200
grep -E "Sync Status|System Status" /tmp/vdB.txt | head -2
grep -E "meters in" /tmp/vdB.err | tail -1 | sed 's/.*| out://' | cut -c1-80
echo "--- RioB analog outputs 1-2 (window 30-38 s): expect 1000 / 2500 Hz at about -50 dBFS"
cd /mnt/d/rio3224-emu && python3 tools/wav-analyse.py /tmp/vdB-out.wav 30 38 | head -4
