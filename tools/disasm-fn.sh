#!/bin/bash
# disasm-fn.sh ADDR COUNT [GREP] : disassemble COUNT instructions from ADDR into /tmp/dis.txt; if GREP given, show 140 lines after the first match
cd /mnt/d/rio3224-emu/emu
~/rio3224/build/rio-emu --disasm "$1,$2" 2>/dev/null > /tmp/dis.txt
echo "lines: $(wc -l < /tmp/dis.txt)"
if [ -n "$3" ]; then
  L=$(grep -n -m1 "$3" /tmp/dis.txt | cut -d: -f1)
  echo "first match of $3 at line ${L:-none}"
  [ -n "$L" ] && sed -n "$((L>${BEFORE:-2}?L-${BEFORE:-2}:1)),$((L+${LINES:-140}))p" /tmp/dis.txt
fi
