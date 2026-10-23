#!/bin/bash
# slot-ctx.sh START COUNT SLOTHEX [LINES] : disassemble and show context around the instruction loading literal SLOTHEX
cd /mnt/d/rio3224-emu/emu
~/rio3224/build/rio-emu --disasm "$1,$2" 2>/dev/null > /tmp/dis-$3.txt
echo "lines: $(wc -l < /tmp/dis-$3.txt)"
grep -n -i "$3" /tmp/dis-$3.txt | head -5
L=$(grep -n -i -m1 "$3" /tmp/dis-$3.txt | cut -d: -f1)
[ -n "$L" ] && sed -n "$((L-${4:-40})),$((L+8))p" /tmp/dis-$3.txt
