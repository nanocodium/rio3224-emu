#!/bin/bash
# Summarize CS4 (DSP interface) traffic from a log file (default /tmp/boot.err)
F=${1:-/tmp/boot.err}
echo "=== per address/direction ==="
grep -E '^\[CS4\]' "$F" | awk '{print $2, $3}' | sort | uniq -c | sort -k3
echo "=== serial words (+A8) in first-seen order with counts ==="
grep -E '^\[CS4\] wr 100000A8' "$F" | awk '{c[$5]++; if (!($5 in o)) {o[$5]=NR; order[++n]=$5}} END {for (i=1;i<=n;i++) printf "%s x%d\n", order[i], c[order[i]]}' | head -${WORDS_MAX:-120}
