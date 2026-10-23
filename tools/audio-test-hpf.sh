#!/bin/bash
# audio-test-hpf.sh : spec item 10 - stationary tones on inputs 1-6, HPF switched on at 125 Hz through the firmware's
# parameter tree at 30 s and off again at 38 s; the Dante TX capture is compared with the biquad the firmware programmed.
cd /mnt/d/rio3224-emu/emu
MAX=${1:-7700000000}
ARGS="--tone 1:sine:31.5:-20 --tone 2:sine:63:-20 --tone 3:sine:125:-20 --tone 4:sine:250:-20 --tone 5:sine:500:-20 --tone 6:sine:1000:-20"
printf '24000 mms\n30000 set hpffreq all 1250\n30500 set hpfon all 1\n38000 set hpfon all 0\n' > /tmp/hpf-console.txt
cp ../fw/d2/flash.bin /tmp/hpf-flash.bin
~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/hpf-flash.bin --dante-boot-ms 15000 --trace-input 3 --trace-ms 2000 \
   --tx-out /tmp/hpf-tx.wav --console-script /tmp/hpf-console.txt --log DSPIF $ARGS < /dev/null > /tmp/hpf.raw 2> /tmp/hpf.err
tail -2 /tmp/hpf.err | cut -c1-160
grep -E '^\[console\]|reg 0063 x5|reg 007A x2' /tmp/hpf.err | grep -A3 'console. 30000' | head -8
grep '^\[audio\].*in03' /tmp/hpf.err | sed -n '12,22p' | cut -c1-200
cd /mnt/d/rio3224-emu && python3 tools/hpf-check.py /tmp/hpf-tx.wav 26 29.5 33 37.5 1:31.5,2:63,3:125,4:250,5:500,6:1000
