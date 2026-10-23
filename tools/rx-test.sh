#!/bin/bash
cd /mnt/d/rio3224-emu/emu
( sleep 3; printf 'x'; sleep 2; printf 'y'; sleep 2; printf 'z\r'; sleep 30 ) | timeout 120 ~/rio3224/build/rio-emu --max 900000000 --no-raw --log SCIF0 --debug-exc > /tmp/c4.txt 2>/tmp/e4.err
echo "console: $(tr -d '\r' < /tmp/c4.txt | wc -l) lines"; tr -d '\r' < /tmp/c4.txt | tail -5
echo "=== interrupt vectors seen ==="; grep -oE 'interrupt vec [0-9]+' /tmp/e4.err | sort | uniq -c
echo "=== rxByte events and FIFO reads ==="; grep -nE 'rxByte|rd FFFE8014|rd FFFE801C -> [1-9]' /tmp/e4.err | head -20
echo "=== SCSCR/SCFCR writes ==="; grep -nE 'wr FFFE8008|wr FFFE8018' /tmp/e4.err | tail -8
echo "=== SCFTDR writes: count and last lines ==="; grep -c 'wr FFFE800C' /tmp/e4.err; grep -nE 'wr FFFE800C|interrupt vec 242' /tmp/e4.err | tail -6
