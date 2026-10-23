#!/bin/bash
# Interactive debug-monitor session: monitor-session.sh TOTAL_SECONDS "cmd" ...  (EXTRA env = extra emulator args)
# Ends with Ctrl-] so the emulator exits cleanly (and saves the flash image).
cd /mnt/d/rio3224-emu/emu
TOTAL=$1; shift
( sleep 5; printf '\r'; sleep 1; for c in "$@"; do printf '%s\r' "$c"; sleep 2; done; sleep $TOTAL; printf '\035' ) | timeout $((TOTAL+120)) ~/rio3224/build/rio-emu --no-raw --debug-exc $EXTRA > /tmp/mon.txt 2>/tmp/mon.err
echo "=== console ($(tr -d '\r' < /tmp/mon.txt | wc -l) lines) ==="; tr -d '\r' < /tmp/mon.txt | iconv -f SHIFT_JIS -t UTF-8 2>/dev/null | grep -vE "PNLSCAN" | head -${LINES_MAX:-400}
echo "=== emulator ==="; grep -E '^\[flash\] (saved|placed|loaded)|^\[cpu\] hook|^\[emu\] stopped' /tmp/mon.err | head -6
