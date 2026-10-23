#!/bin/bash
# unmapped-summary.sh ERRFILE : group "[bus] unmapped" lines by kind/address/PC
grep unmapped "$1" | awk '{print $3, $4, $8}' | sort | uniq -c | sort -rn | head -${2:-10}
