#!/bin/bash
# logctx.sh FILE REGEX BEFORE AFTER  — print context around the first match, collapsing consecutive duplicate lines
F=$1; RE=$2; B=${3:-10}; A=${4:-40}
L=$(grep -nE "$RE" "$F" | head -1 | cut -d: -f1)
[ -z "$L" ] && { echo "no match for $RE"; exit 0; }
S=$((L-B)); [ $S -lt 1 ] && S=1
sed -n "${S},$((L+A))p" "$F" | uniq -c | sed 's/^ *1 //'
