#!/bin/bash
# show-fn.sh FILE FUNC... : print the compact disassembly of functions from a gdb dump
F=$1; shift
for fn in "$@"; do
  echo "=== $fn ==="
  sed -n "/function $fn:/,/End of assembler/p" "$F" | sed -E 's/^ +0x[0-9a-f]+ <\+([0-9]+)>:\t/\1: /' | grep -vE '^Dump|^End|push|pop' | head -${LINES_MAX:-80}
done
