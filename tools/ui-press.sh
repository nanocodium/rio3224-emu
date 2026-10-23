#!/bin/bash
# ui-press.sh "SPEC[;SPEC...]" ... : one boot per argument; each argument is a ';'-separated list of --press specs
# (e.g. "4@25000-25200;1@25500-25700"), OLED dumped to out/ui-<n>.png
cd /mnt/d/rio3224-emu
n=0
for arg in "$@"; do
  n=$((n+1))
  PRESS=""
  IFS='+' read -ra parts <<< "$arg"
  for p in "${parts[@]}"; do PRESS="$PRESS --press $p"; done
  bash tools/press-run.sh ui$n ${MAX:-5000000000} $PRESS ${EXTRA:-} > /dev/null 2>&1
  echo "ui$n ($arg): $(stat -c %s out/ui$n.png) bytes, console tail: $(tail -1 /tmp/ui$n.txt | cut -c1-60)"
done
