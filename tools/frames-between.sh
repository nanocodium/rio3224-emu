#!/bin/bash
# frames-between.sh T0_MS T1_MS [ERRFILE] : Dante frames logged between two emulated times
T0=$1; T1=$2; F=${3:-/tmp/sp.err}
grep '^\[DANTE\]' "$F" | awk -v a="$T0" -v b="$T1" '$2+0 >= a+0 && $2+0 <= b+0' | head -${N:-60}
