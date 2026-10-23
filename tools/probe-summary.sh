#!/bin/bash
# probe-summary.sh [file] : list probe hits as "PC insn"
grep '^\[probe\]' "${1:-/tmp/boot.err}" | awk '{print $2, $4}' | head -${MAXP:-60}
