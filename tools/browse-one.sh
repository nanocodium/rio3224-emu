#!/bin/bash
# browse-one.sh MAXINSNS BROWSE(c,f,g) [extra args] : one run, print DANTE frames + dante console lines
cd /mnt/d/rio3224-emu/emu
MAX=$1; shift; B=$1; shift
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 600 ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/scratch-flash.bin --log DANTE --dante-boot-ms 15000 --dante-browse $B "$@" < /dev/null > /tmp/b1.raw 2>/tmp/b1.err
tr -d '\r' < /tmp/b1.raw | iconv -f SHIFT_JIS -t UTF-8 2>/dev/null | grep -vE '^Assert|PNLSCAN' > /tmp/b1.txt
echo "=== browse $B: $(grep -E '^\[emu\] stopped' /tmp/b1.err)"
grep -E '^\[DANTE\]' /tmp/b1.err | head -${DMAX:-40}
echo "=== console (dante/dnt/bca/dsp lines):"
grep -n -i 'dante\|dnt\|bca\|brook\|DSP' /tmp/b1.txt | head -40
