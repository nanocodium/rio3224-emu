#!/bin/bash
# Print host->module and module->host UART bytes recorded in a log (default /tmp/boot.err) as hex streams
F=${1:-/tmp/boot.err}
for ch in A B; do
  echo "=== host -> module UART $ch: $(grep -cE "^\[DNT$ch\] ->" "$F") bytes ==="
  grep -E "^\[DNT$ch\] ->" "$F" | head -${MAXB:-200} | awk '{printf "%s ", $3} END {print ""}'
  echo "=== module -> host UART $ch: $(grep -cE "^\[DNT$ch\] <-" "$F") bytes ==="
  grep -E "^\[DNT$ch\] <-" "$F" | head -${MAXB:-200} | awk '{printf "%s ", $3} END {print ""}'
done
