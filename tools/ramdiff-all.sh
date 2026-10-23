#!/bin/bash
# ramdiff-all.sh : diff every /tmp/pd-<variant>.bin against /tmp/pd-base.bin (see param-hunt.sh)
cd /mnt/d/rio3224-emu
for v in gcon0 gcon1 gcg0 gain0 hpf0 p48v0 hpfon0; do echo "==== $v"; python3 tools/ramdiff.py /tmp/pd-base.bin /tmp/pd-$v.bin 0C000000 ${N:-24}; done
