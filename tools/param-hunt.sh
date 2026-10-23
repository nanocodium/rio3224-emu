#!/bin/bash
# param-hunt.sh : boot several times with one extra "mms set ..." console command each and dump SDRAM at exit,
# so tools/ramdiff.py can locate the firmware's parameter storage. Results: /tmp/pd-<name>.bin
cd /mnt/d/rio3224-emu/emu
MAX=${MAX:-5000000000}
run() {   # run NAME "console line"
  local name=$1; shift
  printf '24000 mms\n' > /tmp/pd-$name.txt
  for l in "$@"; do printf '%s\n' "$l" >> /tmp/pd-$name.txt; done
  cp ../fw/d2/flash.bin /tmp/pd-$name-flash.bin
  ~/rio3224/build/rio-emu --max $MAX --no-raw --flash /tmp/pd-$name-flash.bin --dante-boot-ms 15000 \
      --console-script /tmp/pd-$name.txt --ramdump /tmp/pd-$name.bin < /dev/null > /tmp/pd-$name.out 2> /tmp/pd-$name.err
  echo "$name: $(tail -1 /tmp/pd-$name.err | cut -c1-80)"
}
run base
run gcon0  "25000 set gcon 0 1"
run gcon1  "25000 set gcon 1 1"
run gcg0   "25000 set gcgain 0 2000"
run gain0  "25000 set gain 0 2000"
run hpf0   "25000 set hpffreq 0 1200"
run p48v0  "25000 set 48v 0 1"
run hpfon0 "25000 set hpfon 0 1"
