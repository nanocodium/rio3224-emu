#!/bin/bash
# perf-run.sh [MAXINSNS] : profile a plain boot with perf and print the top symbols
cd /mnt/d/rio3224-emu/emu
cp ../fw/d2/flash.bin /tmp/pf-flash.bin
perf record -q -o /tmp/perf.data -- ~/rio3224/build/rio-emu --max ${1:-3000000000} --no-raw --flash /tmp/pf-flash.bin --dante-boot-ms 15000 < /dev/null > /dev/null 2>/tmp/pf.err
tail -1 /tmp/pf.err
perf report -i /tmp/perf.data --stdio --no-children 2>/dev/null | grep -v '^#' | grep -v '^$' | head -${2:-28}
