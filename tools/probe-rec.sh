#!/bin/bash
# probe-rec.sh ERRFILE HEXPREFIX : show probe hits whose first dumped memory line starts with HEXPREFIX (e.g. "0000 0026"), with the preceding DANTE lines
F=$1; P=$2
grep -n -A24 '^\[probe\]' "$F" | grep -E "^[0-9]+-  [0-9A-F]{8}: $P" | head -5 | while IFS=- read -r L rest; do
  echo "--- probe block ending near line $L"
  S=$((L-24)); [ $S -lt 1 ] && S=1
  sed -n "${S},$((L+3))p" "$F"
  grep -E '^\[DANTE\]' <(head -$L "$F") | tail -4
done
