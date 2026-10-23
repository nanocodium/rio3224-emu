#!/bin/bash
# Summarize external-bus (CS*) accesses recorded in /tmp/boot.err: per address+direction counts and sample PCs
F=${1:-/tmp/boot.err}
echo "=== per address/direction counts ==="
grep -E '^\[CS' "$F" | awk '{print $1, $2, $3}' | sort | uniq -c | sort -k4
echo "=== sample PCs per address (first 3 distinct) ==="
grep -E '^\[CS' "$F" | grep -oE '^\[CS[0-9]\] (rd|wr) [0-9A-F]+|PC=[0-9A-F]+' | paste - - | sort -u | awk '{k=$1" "$2" "$3; c[k]++; if (c[k]<=3) print}'
