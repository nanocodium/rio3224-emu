#!/bin/bash
# show N lines of /tmp/irqtrace.txt before the first trace line whose PC matches a regex
RE=${1:-'^0000[0-9A-F]{4}  '}; N=${2:-90}
L=$(grep -nE "$RE" /tmp/irqtrace.txt | head -1 | cut -d: -f1)
echo "first match at line $L"
[ -n "$L" ] && sed -n "$((L-N)),$((L+3))p" /tmp/irqtrace.txt
