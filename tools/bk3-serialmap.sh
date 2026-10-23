#!/bin/bash
# Which QEMU -serial index is the Zynq UART the kernel calls ttyPS0 / ttyPS1?  (60 s so the shell prompt appears)
D=/mnt/d/rio3224-emu/fw/d2/bk3/_FWUpdate_Rio3224-D2_v4020503_sw501.dnt.extracted
Q=/mnt/d/rio3224-emu/fw/d2/bk3/qemu
rm -f /tmp/s0.txt /tmp/s1.txt
timeout 60 qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -monitor none -kernel $D/zImage -dtb $Q/kernel-qemu.dtb \
  -initrd $Q/initramfs-debugB.cpio.gz -append "console=ttyPS1,115200 root=/dev/ram0 rdinit=/init" \
  -serial file:/tmp/s0.txt -serial file:/tmp/s1.txt < /dev/null > /tmp/q.out 2>&1
echo "console=ttyPS1: serial0 $(wc -c < /tmp/s0.txt) bytes, serial1 $(wc -c < /tmp/s1.txt) bytes"
echo "--- serial0 tail ---"; tail -4 /tmp/s0.txt | cut -c1-100
echo "--- serial1 content ---"; head -c 400 /tmp/s1.txt | tr -d '\r' | tail -8
