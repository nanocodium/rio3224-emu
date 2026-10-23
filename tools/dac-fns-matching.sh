#!/bin/bash
# dac-fns-matching.sh PATTERN [LINES] : disassemble every dac function whose name matches PATTERN (from /tmp/dac_text.txt)
grep -o "<[A-Za-z0-9_]*${1}[A-Za-z0-9_]*+0>:" /tmp/dac_text.txt | sort -u | tr -d '<>:' | sed 's/+0$//' | while read -r f; do echo "### $f"; bash "$(dirname "$0")/dac-fn.sh" "$f" "${2:-60}"; done
