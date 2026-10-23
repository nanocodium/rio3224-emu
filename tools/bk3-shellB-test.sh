#!/bin/bash
# Module alone: UART A -> file, UART B -> TCP server 5560 with a root shell (debugB initramfs, kernel console off).
D=/mnt/d/rio3224-emu/fw/d2/bk3/_FWUpdate_Rio3224-D2_v4020503_sw501.dnt.extracted
Q=/mnt/d/rio3224-emu/fw/d2/bk3/qemu
WAIT=${WAIT:-70}; SECS=${SECS:-110}
rm -f /tmp/s0.txt
timeout $SECS qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -monitor none -kernel $D/zImage -dtb $Q/kernel-qemu.dtb \
  -initrd $Q/initramfs-debugB.cpio.gz -drive if=mtd,unit=8,format=raw,file=$Q/bk3flash.img \
  -append "console=ttynull quiet root=/dev/ram0 rdinit=/init" \
  -serial file:/tmp/s0.txt -chardev socket,id=s1,host=127.0.0.1,port=5560,server=on,wait=off -serial chardev:s1 < /dev/null > /tmp/q.out 2>&1 &
QPID=$!
python3 /mnt/d/rio3224-emu/tools/tcp-shell.py 127.0.0.1 5560 $WAIT ${CMDFILE:-/mnt/d/rio3224-emu/tools/bk3-cmds15.txt} ${STEP:-3} > /tmp/shellB.txt 2>&1
wait $QPID
echo "serial0 (UART A) bytes: $(wc -c < /tmp/s0.txt)"; echo "--- shell session ---"; cat /tmp/shellB.txt | head -${TAIL:-100}; echo "--- qemu ---"; tail -2 /tmp/q.out
