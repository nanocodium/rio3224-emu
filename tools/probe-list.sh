#!/bin/bash
# probe-list.sh ERRFILE [HEX8] : one line per probe hit (insn count, PR, first dumped memory line); with HEX8, show context for the first hit whose first memory line contains HEX8
F=$1
awk '/^\[probe\]/ {split($0,a,"insn "); ins=a[2]; n=0; next} n>=0 && /PR=/ {split($0,b,"PR="); pr=substr(b[2],1,8)} /^  [0-9A-F]+: / && ins!="" {if (!shown) print NR": insn", ins, "PR", pr, $0; shown=1; next} /^\[probe\]/ {shown=0} {if ($0 ~ /^\[probe\]/) shown=0}' "$F" | awk '!seen[$3]++' | head -${N:-60}
if [ -n "$2" ]; then
  L=$(grep -n -E "^  [0-9A-F]{8}: .*$2" "$F" | head -1 | cut -d: -f1)
  if [ -n "$L" ]; then echo "--- context before line $L"; head -$L "$F" | grep -E '^\[DANTE\]|^\[probe\]' | tail -14; sed -n "$((L-5)),$((L+3))p" "$F"; fi
fi
