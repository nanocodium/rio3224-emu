#!/bin/bash
# param-hunt2.sh : pairs of boots whose console scripts differ only in one parameter VALUE (same text length),
# so the SDRAM diff isolates the parameter storage. Dumps: /tmp/pd2-<name>.bin ; then prints the diffs.
cd /mnt/d/rio3224-emu/emu
MAX=${MAX:-5000000000}
run() {   # run NAME "console line"
  local name=$1; shift
  printf '24000 mms\n' > /tmp/pd2-$name.txt
  for l in "$@"; do printf '%s\n' "$l" >> /tmp/pd2-$name.txt; done
  cp ../fw/d2/flash.bin /tmp/pd2-$name-flash.bin
  ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/pd2-$name-flash.bin --dante-boot-ms 15000 \
      --console-script /tmp/pd2-$name.txt --ramdump /tmp/pd2-$name.bin < /dev/null > /tmp/pd2-$name.out 2> /tmp/pd2-$name.err
}
run gcon_a "25000 set gcon 0 1";      run gcon_b "25000 set gcon 0 0"
run gcg_a  "25000 set gcgain 0 2000"; run gcg_b  "25000 set gcgain 0 2500"
run gain_a "25000 set gain 0 2000";   run gain_b "25000 set gain 0 2600"
run p48_a  "25000 set 48v 0 1";       run p48_b  "25000 set 48v 0 0"
run gcch_a "25000 set gcon 1 1";      run gcch_b "25000 set gcon 2 1"
cd /mnt/d/rio3224-emu
for p in gcon gcg gain p48 gcch; do echo "==== $p (a vs b)"; python3 tools/ramdiff.py /tmp/pd2-${p}_a.bin /tmp/pd2-${p}_b.bin 0C000000 ${N:-30}; done
