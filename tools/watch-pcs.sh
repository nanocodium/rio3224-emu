#!/bin/bash
# watch-pcs.sh [ERRFILE] : aggregate [watch] lines by direction and PC
F=${1:-/tmp/watch.err}
grep -E '^\[watch\]' "$F" | sed -E 's/\[watch\] (rd|wr) ([0-9A-F]+) size ([0-9]) val ([0-9A-F]+) at PC=([0-9A-F]+) insn ([0-9]+)/\1 \5/' | sort | uniq -c | sort -rn | head -${LINES_MAX:-40}
