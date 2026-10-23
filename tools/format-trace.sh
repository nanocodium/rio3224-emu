#!/bin/bash
cd /mnt/d/rio3224-emu/emu
( sleep 5; printf '\r'; sleep 1; printf 'mms\r'; sleep 2; printf 'init\r'; sleep 6 ) | timeout 120 ~/rio3224/build/rio-emu --no-raw --hook 0C171A2E@0C1408F0:r4=1 --trace-pc 0C100470,700 --log flash > /tmp/fmt.txt 2>/tmp/fmt.err
echo "trace lines: $(grep -cE '^[0-9A-F]{8}  ' /tmp/fmt.err)"
grep -E '^[0-9A-F]{8}  |^\[flash\]' /tmp/fmt.err | head -${1:-260}
