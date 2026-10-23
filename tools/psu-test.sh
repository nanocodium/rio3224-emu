#!/bin/bash
# psu-test.sh : dual-PSU / +48V MASTER model test.  Boot (RESUME flash with +48V saved ON for inputs 1-2), +48V MASTER
# switch ON at 27 s, PSU A fails at 30 s and recovers at 36 s; the firmware's status is read at 29/33/38 s.
cd /mnt/d/rio3224-emu/emu
python3 ../tools/flash-set.py ../fw/d2/flash.bin /tmp/psu-flash.bin startup=2 48v=1:1 48v=2:1 > /dev/null
{ echo '28500 rep'; echo '29000 current'; echo '33000 current'; echo '38000 current'; echo '39000 /err info'; } > /tmp/psu-console.txt
timeout 1500 ~/rio3224/build/rio-emu --max ${MAX:-9000000000} --no-raw --flash /tmp/psu-flash.bin --dante-boot-ms 15000 --oled /tmp/psu.pgm --panel \
   --48v-master on@27000 --psu-fail A@30000-36000 --oled-at 31500=/tmp/psu-1.pgm --oled-at 38500=/tmp/psu-2.pgm --console-script /tmp/psu-console.txt --tone 1:sine:1000:-20 < /dev/null > /tmp/psu.raw 2>/tmp/psu.err
tr -d '\r' < /tmp/psu.raw | iconv -f SHIFT_JIS -t UTF-8 -c | grep -vE '^Assert|PNLSCAN' > /tmp/psu.txt
for f in /tmp/psu-1.pgm /tmp/psu-2.pgm /tmp/psu.pgm; do b=$(basename $f .pgm); python3 /mnt/d/rio3224-emu/tools/pgm2png.py $f /mnt/d/rio3224-emu/out/$b.png > /dev/null 2>&1; done
echo "=== $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/psu.err | head -1 | cut -c1-80)"
grep -E '^\[pld\]|^\[panel\]' -A3 /tmp/psu.err | grep -E 'pld|\+48V in' | head
grep -nE 'System Status|48V Master|OCCURRED|RECOVER|POWER|E00' /tmp/psu.txt | head -20 | cut -c1-110
grep -A40 -m1 'err info' /tmp/psu.txt | grep -vE ': 0$' | head -8
grep -E '^\[audio\] in01' /tmp/psu.err | tail -1 | cut -c1-170
