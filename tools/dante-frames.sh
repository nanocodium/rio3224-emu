#!/bin/bash
# dante-frames.sh [ERRFILE] : summarise BCA frames logged with --log DANTE (default /tmp/b1.err)
# log line format: [DANTE] <ms> ms -> cat CC cmd N seq S res R len L: payload
F=${1:-/tmp/b1.err}
echo "=== host -> module requests (cat/cmd count):"
grep -E '^\[DANTE\] +[0-9]+ ms <- cat 0[123] ' "$F" | awk '{printf "%s/%s\n", $6, $8}' | sort | uniq -c | sort -rn | awk '{printf "  %s x%s\n", $2, $1}' | tr '\n' ' '; echo
echo "=== module -> host notifies:"
grep -E '^\[DANTE\] +[0-9]+ ms -> cat 03 ' "$F" | awk '{print $8}' | sort | uniq -c | tr '\n' ' '; echo
echo "=== unhandled:"
grep -E 'unhandled' "$F" | sort | uniq -c
echo "=== last ${N:-30} frames:"
grep -E '^\[DANTE\]' "$F" | tail -${N:-30}
