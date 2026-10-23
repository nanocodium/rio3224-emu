#!/bin/bash
# console-test.sh : virtual console end-to-end test (spec: console / R Remote HA control, one authoritative state).
# A unit runs on the virtual Dante network in real time with a -20 dBu tone on input 1; at 33 s the console stand-in sets
# HA gain 1 = +20 dB, +48V 2 on, HPF 3 on at 120 Hz, GC on 4 with start gain +10 dB and panel lock on.  The unit's own
# `rep` reports, its UART notifies (printed by the console) and the engine's head-amp trace must all show the new values.
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /mnt/d/rio3224-emu/out/ct-flash.bin startup=2 > /dev/null
{ echo '44000 /rep current'; echo '45000 /rep hgain'; echo '45500 /rep h48'; echo '46000 /rep hpfo'; echo '46500 /rep hpff'; echo '47000 /rep gcom'; echo '47500 /rep gcoms'; } > /mnt/d/rio3224-emu/out/ct-console.txt
~/rio3224/build/rio-emu --max 90000000000 --run-ms 50000 --realtime --no-raw --flash /mnt/d/rio3224-emu/out/ct-flash.bin --dante-boot-ms 15000 \
   --dante-name RioA --dante-net --tone 1:sine:1000:-20 --trace-input 1 --trace-ms 5000 --console-script /mnt/d/rio3224-emu/out/ct-console.txt \
   < /dev/null > /mnt/d/rio3224-emu/out/ct.raw 2> /mnt/d/rio3224-emu/out/ct.err &
PID=$!
sleep 33
cd /mnt/d/rio3224-emu
python3 tools/dante-console.py RioA --wait 3 'gain 1 20; 48v 2 on; hpf 3 on; hpffreq 3 120; gc 4 on; gcgain 4 10; lock on' 2>&1 | cut -c1-170 | tee out/ct-console.log
wait $PID
tr -d '\r' < out/ct.raw | iconv -f SHIFT_JIS -t UTF-8 -c > out/ct.txt
echo "=== unit: $(grep -E '^\[emu\] (--run|stopped)' out/ct.err | head -1 | cut -c1-60)"
grep -E 'HA remote frame' out/ct.err | head -3
grep -E 'Panel Lock|Read Only' out/ct.txt | head -2
for k in 'HA Gain' 'HA 48V' 'HPF \[' 'HPF Freq' 'Gain Compensation \[' 'Gain Compensation Start'; do grep -A1 -E "^- $k" out/ct.txt | tail -1 | cut -c1-80; done
grep -E '^\[audio\] +[0-9]+ ms in01' out/ct.err | tail -2 | cut -c1-170
