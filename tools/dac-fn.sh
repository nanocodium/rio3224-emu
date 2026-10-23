#!/bin/bash
# dac-fn.sh SYMBOL [LINES] : print the disassembly of a dac function from /tmp/dac_text.txt
L=$(grep -n -m1 "<$1+0>:" /tmp/dac_text.txt | cut -d: -f1)
[ -z "$L" ] && { echo "no $1"; exit 1; }
sed -n "${L},$((L+${2:-120}))p" /tmp/dac_text.txt | sed -E 's/^ +0x0*([0-9a-f]+) <[^>]*>:\t/\1: /'
