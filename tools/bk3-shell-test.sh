#!/bin/bash
# Boot the Brooklyn with the debug initramfs and run shell commands after boot; output in /tmp/bk3s.log
#   CMDFILE=file (one command per line) or commands as arguments; WAIT = seconds before typing
Q=/mnt/d/rio3224-emu/fw/d2/bk3/qemu
D=/mnt/d/rio3224-emu/fw/d2/bk3/_FWUpdate_Rio3224-D2_v4020503_sw501.dnt.extracted
WAIT=${WAIT:-60}
feed() { sleep $WAIT; if [ -n "$CMDFILE" ]; then while IFS= read -r c; do printf '%s\n' "$c"; sleep ${STEP:-3}; done < "$CMDFILE"; else for c in "$@"; do printf '%s\n' "$c"; sleep ${STEP:-3}; done; fi; sleep 6; }
feed "$@" | timeout $((WAIT+${EXTRA:-60})) qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -monitor none \
  -kernel $D/zImage -dtb $Q/kernel-qemu.dtb -initrd $Q/initramfs-debug.cpio.gz -drive if=mtd,unit=8,format=raw,file=$Q/bk3flash.img \
  -append "console=ttyPS0,115200 root=/dev/ram0 rdinit=/init" -serial stdio -serial null > /tmp/bk3s.log 2>&1
sed -n "/^echo START/,\$p" /tmp/bk3s.log | tr -d '\r' | grep -v "^# $" | head -${TAIL:-120}
