#!/bin/bash
# Boot the firmware and feed debug-console commands after a delay; stdout (console) -> /tmp/console.txt, stderr -> /tmp/emu.err
# usage: console-run.sh MAX_INSNS DELAY_S "cmd1" "cmd2" ...
MAX=$1; DELAY=$2; shift 2
cd /mnt/d/rio3224-emu/emu
( sleep $DELAY; printf '\r'; sleep 1; for c in "$@"; do printf '%s\r' "$c"; sleep 3; done; sleep 600 ) | \
  timeout 900 ~/rio3224/build/rio-emu --max $MAX --no-raw > /tmp/console.txt 2>/tmp/emu.err
echo "=== console ($(wc -l < /tmp/console.txt) lines) ==="; tr -d '\r' < /tmp/console.txt | head -${LINES_MAX:-220}
echo "=== stderr tail ==="; grep -vE "^\[MTU2\]" /tmp/emu.err | tail -6
