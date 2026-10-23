#!/bin/bash
# show the FPGA ID bus exchange following the first "cmd 0" (board id) in /tmp/plds.err
L=$(grep -n 'fpga-id bus cmd 0' /tmp/plds.err | head -1 | cut -d: -f1)
echo "first cmd 0 at line $L"
awk -v s=$L 'NR>=s-16 && NR<=s+60 && /fpga/' /tmp/plds.err | grep -vE 'miso 0 \(bits 0\)' | head -70
