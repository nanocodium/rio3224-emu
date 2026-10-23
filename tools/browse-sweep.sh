#!/bin/bash
# try browse-complete payload variants; print host commands received by the module for each
cd /mnt/d/rio3224-emu/emu
for v in 1,7,0 2,7,0 3,0,0 3,7,1 80,0,0; do
  cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
  timeout 300 ~/rio3224/build/rio-emu --max 3200000000 --no-raw --flash /tmp/scratch-flash.bin --log DANTE --dante-boot-ms 15000 --dante-browse $v < /dev/null > /dev/null 2>/tmp/sweep.err
  echo "=== browse $v: host cmds: $(grep -E '^\[DANTE\] <- cat 0[12]' /tmp/sweep.err | awk '{print $4 $5 "-" $7}' | sort | uniq -c | tr '\n' ' ')"
done
