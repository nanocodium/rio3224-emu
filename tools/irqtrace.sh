#!/bin/bash
# Run the emulator tracing N instructions after each interrupt/exception; show the context before the first illegal-instruction exception.
# usage: irqtrace.sh [max_insns] [trace_len] [context_lines]
MAX=${1:-400000}; TL=${2:-6000}; CTX=${3:-130}
cd /mnt/d/rio3224-emu/emu
timeout 300 ~/rio3224/build/rio-emu --fw ../fw/d2/RIO2_PROG.BIN --max $MAX --no-raw --debug-exc --trace-irq $TL < /dev/null > /tmp/irqtrace.txt 2>&1
L=$(grep -n 'exception vec 4' /tmp/irqtrace.txt | head -1 | cut -d: -f1)
echo "first illegal-instruction exception at line $L of $(wc -l < /tmp/irqtrace.txt)"
[ -n "$L" ] && sed -n "$((L-CTX)),$((L+1))p" /tmp/irqtrace.txt
