#!/bin/bash
# cs6-seq.sh [ERRFILE] : DM9000 (CS6) index/data access sequence in order, compacted
F=${1:-/tmp/aes.err}
grep 'CS6' "$F" | sed -E 's/ at PC=.*//' | awk '{printf "%s %s %s; ", $2, $3, $5} END {print ""}' | fold -w 150
