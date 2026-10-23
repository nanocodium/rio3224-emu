#!/bin/bash
# run-summary.sh NAME... : summarise press-run.sh results in /tmp/NAME.*
for n in "$@"; do
  python3 /mnt/d/rio3224-emu/tools/pgm2png.py /tmp/$n.pgm /mnt/d/rio3224-emu/out/$n.png > /dev/null 2>&1
  echo "== $n: $(grep -E '^\[emu\] stopped|CPU FATAL' /tmp/$n.err | head -1)"
  echo "   EVT: $(grep EVT /tmp/$n.txt | tr '\n' '|')   png bytes: $(stat -c %s /mnt/d/rio3224-emu/out/$n.png)"
  echo "   last console: $(tail -2 /tmp/$n.txt | tr '\n' '|')"
done
