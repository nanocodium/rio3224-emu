#!/bin/bash
# dac-ctx.sh SYMBOL PATTERN [BEFORE] [AFTER] : context around PATTERN inside dac function SYMBOL (from /tmp/dac_text.txt)
L=$(grep -n -m1 "<$1+0>:" /tmp/dac_text.txt | cut -d: -f1)
[ -z "$L" ] && { echo "no $1"; exit 1; }
E=$(awk -v s="$L" 'NR>s && /\+0>:/ {print NR; exit}' /tmp/dac_text.txt)
sed -n "${L},${E}p" /tmp/dac_text.txt | sed -E 's/^ +0x0*([0-9a-f]+) <[^>]*>:\t/\1: /' | grep -n -B${3:-20} -A${4:-3} "$2"
