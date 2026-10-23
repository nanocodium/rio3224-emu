#!/bin/bash
# Run boot with CS5 logging and print the ordered write sequence on the OLED port (0x14001000/2) and display control (0x14001802)
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 300 ~/rio3224/build/rio-emu --max ${MAX:-400000000} --no-raw --log CS5 --flash /tmp/scratch-flash.bin < /dev/null > /dev/null 2>/tmp/oled.err
grep -E '^\[CS5\] wr 1400(100[0-6]|180[0-6])' /tmp/oled.err | awk '{a=$3; v=$5; if (a=="14001002") printf "%s ", substr(v,3,2); else printf "\n[%s=%s] ", substr(a,5,4), v}' | head -c ${CHARS:-6000}
echo; echo "=== total writes: $(grep -cE '^\[CS5\] wr 14001002' /tmp/oled.err) data bytes ==="
