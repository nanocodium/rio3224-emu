#!/bin/bash
# Summarize [watch] lines from /tmp/boot.err: address, direction, PC, sample value (first occurrence per addr/dir/PC)
F=${1:-/tmp/boot.err}
grep -E '^\[watch\]' "$F" | sed -E 's/\[watch\] (rd|wr) ([0-9A-F]+) size ([0-9]) val ([0-9A-F]+) at PC=([0-9A-F]+) insn ([0-9]+)/\2 \1 \5 \4 sz\3/' | sort -u -k1,3 | sort
