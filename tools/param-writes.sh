#!/bin/bash
# param-writes.sh ERRFILE [FROM_MS] : the head-amp link and DSP register writes that follow each scripted console line
# (run the emulator with --console-script ... --log DSPIF --log HA)
F=$1; FROM=${2:-0}
grep -A1000000 -m1 "\[console\] $FROM" "$F" | grep -E '^\[console\]|^\[CS4\] (write|burst)|^\[HA\]' | head -${N:-200}
