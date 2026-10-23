#!/bin/bash
cd /mnt/d/rio3224-emu/emu
( sleep 4; printf '?\r'; sleep 3; printf '?\r'; sleep 20 ) | timeout 120 ~/rio3224/build/rio-emu --max 500000000 --no-raw --log SCIF0 --debug-exc > /tmp/c3.txt 2>/tmp/e3.err
echo "console lines: $(tr -d '\r' < /tmp/c3.txt | wc -l)"
echo "RXI (vec 241) interrupts: $(grep -c 'interrupt vec 241' /tmp/e3.err)   TXI (242): $(grep -c 'interrupt vec 242' /tmp/e3.err)   ERI (240): $(grep -c 'interrupt vec 240' /tmp/e3.err)"
echo "SCFRDR reads: $(grep -c 'rd FFFE8014' /tmp/e3.err)   SCFSR reads: $(grep -c 'rd FFFE8010' /tmp/e3.err)   SCFSR writes: $(grep -c 'wr FFFE8010' /tmp/e3.err)"
echo "=== first SCFSR/SCFRDR events after RX ==="; grep -nE 'rd FFFE8014|wr FFFE8010|rd FFFE8010 -> [1-9]|interrupt vec 24[01]' /tmp/e3.err | head -20
echo "=== all distinct interrupt vectors seen ==="; grep -oE 'interrupt vec [0-9]+' /tmp/e3.err | sort | uniq -c
