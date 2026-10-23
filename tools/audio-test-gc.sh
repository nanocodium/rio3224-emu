#!/bin/bash
# audio-test-gc.sh [MAXINSNS] : gain-compensation acceptance test (spec items 12/74).  Boot from a flash copy saved
# with StartupMode RESUME, input 1 HA +20 dB with GC on (start gain +20), input 2 the same but GC off, input 3 GC on
# with a hot signal that clips the converter once the gain is raised.  At 30 s the firmware's debug monitor raises the
# HA gain of inputs 1-3 to +30 dB (the same path the panel encoder and a console use).  The Dante TX recording must
# show input 1 unchanged (GC = start - current = -10 dB applied digitally), input 2 up by 10 dB, and input 3 clipped
# (GC does not undo analog/converter clipping).  Results: /tmp/gc.{txt,err}, /tmp/gc-tx.wav
cd /mnt/d/rio3224-emu/emu
MAX=${1:-9400000000}
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/gc-flash.bin startup=0 gain=1:20 gcon=1:1 gcstart=1:20 gain=2:20 gain=3:20 gcon=3:1 gcstart=3:20
{ echo '29000 mms'; echo '30000 set gain 0 3000'; echo '30300 set gain 1 3000'; echo '30600 set gain 2 3000'; echo '32000 get gcgain 0'; echo '33000 /rep gcom'; echo '34000 /dsp meter'; } > /tmp/gc-console.txt
~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/gc-flash.bin --dante-boot-ms 15000 --panel --trace-input 1 --trace-ms 2000 \
   --tx-out /tmp/gc-tx.wav --console-script /tmp/gc-console.txt \
   --tone 1:sine:1000:-30 --tone 2:sine:2000:-30 --tone 3:sine:3000:-2 < /dev/null > /tmp/gc.raw 2> /tmp/gc.err
tr -d '\r' < /tmp/gc.raw | iconv -f SHIFT_JIS -t UTF-8 -c > /tmp/gc.txt
echo "=== $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/gc.err | head -1 | cut -c1-80)"
grep -nE 'gcgain|GC|Gain Comp|gcom' /tmp/gc.txt | head -20
grep -A5 -m1 "port met sig peak" /tmp/gc.txt
grep "^\[audio\]  *[0-9]* ms" /tmp/gc.err | cut -c1-200 | awk 'NR%2==1' | head -12
echo "--- TX levels (dBFS) in 1 s windows, channels 1-3, before (26-29 s) and after (32-36 s) the gain step:"
cd /mnt/d/rio3224-emu && python3 tools/wav-analyse.py /tmp/gc-tx.wav 26 29 && python3 tools/wav-analyse.py /tmp/gc-tx.wav 32 36
