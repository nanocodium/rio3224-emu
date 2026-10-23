#!/bin/bash
# 1) show DSP-interface read transactions from the current /tmp/boot.err, 2) rerun with --log CS4 and summarize the 16-bit writes to +0x8E
echo "=== read transactions (current log) ==="
grep -E '^\[CS4\] read dev' /tmp/boot.err | sort | uniq -c | sort -rn | head -30
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 600 ~/rio3224/build/rio-emu --max ${MAX:-6500000000} --no-raw --flash /tmp/scratch-flash.bin --log CS4 --dante-clock-ms 3000,10000,10400 < /dev/null > /dev/null 2>/tmp/cs4.err
echo "=== +8E value histogram ==="
grep -E '^\[CS4\] wr 1000008E' /tmp/cs4.err | awk '{print $5}' | sort | uniq -c | sort -rn | head -12
echo "=== first 64 +8E writes ==="
grep -E '^\[CS4\] wr 1000008E' /tmp/cs4.err | head -64 | awk '{printf "%s ", $5} END {print ""}'
echo "=== context before first +8E write ==="
L=$(grep -n -m1 'wr 1000008E' /tmp/cs4.err | cut -d: -f1)
sed -n "$((L-14)),$((L+4))p" /tmp/cs4.err
echo "=== other CS4 offsets ==="
grep -E '^\[CS4\]' /tmp/cs4.err | awk '{print $2, $3}' | sort | uniq -c | sort -k3
