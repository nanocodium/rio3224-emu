#!/bin/bash
# count-uart.sh [ERRFILE] [TXTFILE]: UART A/B byte counts from a --log DNTUART run and the EVT lines of the console
E=${1:-/tmp/pk.err}; T=${2:-/tmp/pk.txt}
echo "UART A host->module bytes: $(grep -c '^\[DNTA\] -> ' $E)   UART A module->host reads: $(grep -c '^\[DNTA\] <= [0-9A-F]' $E)"
echo "UART B host->module bytes: $(grep -c '^\[DNTB\] -> ' $E)   UART B module->host reads: $(grep -c '^\[DNTB\] <= [0-9A-F]' $E)"
echo "--- first UART B bytes:"; grep '^\[DNTB\] -> ' $E | head -40 | cut -c11- | tr '\n' ' '; echo
echo "--- EVT lines:"; grep -n 'EVT' $T
