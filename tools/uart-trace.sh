#!/bin/bash
# uart-trace.sh MAXINSNS [args]: run with DANTE + DNTUART logs, keep stderr in /tmp/ut.err
cd /mnt/d/rio3224-emu/emu
MAX=$1; shift
cp ../fw/d2/flash.bin /tmp/scratch-flash.bin
timeout 900 ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/scratch-flash.bin --log DANTE --log DNTUART --dante-boot-ms 15000 "$@" < /dev/null > /tmp/ut.raw 2>/tmp/ut.err
echo "stopped: $(grep -E '^\[emu\] stopped' /tmp/ut.err)"
echo "module->host bytes queued: $(grep -c '^\[DNTA\] <- ' /tmp/ut.err)  host RX reads: $(grep -c '^\[DNTA\] <= [0-9A-F]' /tmp/ut.err)  empty reads: $(grep -c '^\[DNTA\] <= (empty)' /tmp/ut.err)  host TX bytes: $(grep -c '^\[DNTA\] -> ' /tmp/ut.err)"
