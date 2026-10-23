#!/bin/bash
# dump-after-boot.sh ADDR,LEN [ADDR,LEN ...]  — boot 300M insns then dump memory regions
cd /mnt/d/rio3224-emu/emu
for r in "$@"; do timeout 120 ~/rio3224/build/rio-emu --max 300000000 --no-raw --no-unmapped --dump $r < /dev/null 2>&1 | grep -E '^[0-9A-F]{8}:'; done
