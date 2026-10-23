#!/bin/bash
# Boot the Brooklyn III (Zynq-7000) Dante module image in QEMU.
#   brooklyn-run.sh [seconds] [extra qemu args]
# UART0 (ttyPS0 = /dev/ttyS0, Yamaha "dac" link = DNT_UART_A) and UART1 (ttyPS1 = /dev/ttyS1, conmon HA remote =
# DNT_UART_B) are exposed as TCP clients to the host emulator's listeners (rio-emu --dnt-uart 5550,5551) when
# BK3_UART=tcp; otherwise UART0 goes to stdio for inspection.
#   INITRD=...  initramfs to use (default: original; tools/bk3-debug-initramfs.sh builds QEMU variants)
D=/mnt/d/rio3224-emu/fw/d2/bk3/_FWUpdate_Rio3224-D2_v4020503_sw501.dnt.extracted
Q=/mnt/d/rio3224-emu/fw/d2/bk3/qemu            # tools/bk3-mkimage.py output: 16 MB flash + patched DTB
SECS=${1:-60}; shift
if [ "${BK3_UART:-stdio}" = tcp ]; then
  SER=(-serial tcp:127.0.0.1:${PORT_A:-5550},nodelay -serial tcp:127.0.0.1:${PORT_B:-5551},nodelay)
  CONSOLE="console=ttynull quiet"                     # keep the dac UART clean
else
  SER=(-serial stdio -serial null)
  CONSOLE="console=ttyPS0,115200"
fi
DTB=$D/kernel.dtb; FLASH=()
if [ -f $Q/kernel-qemu.dtb ] && [ -f $Q/bk3flash.img ]; then DTB=$Q/kernel-qemu.dtb; FLASH=(-drive if=mtd,unit=8,format=raw,file=$Q/bk3flash.img); fi
INITRD=${INITRD:-$D/initramfs.cpio.gz}
exec timeout $SECS qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -monitor none \
  -kernel $D/zImage -dtb $DTB -initrd $INITRD "${FLASH[@]}" \
  -append "$CONSOLE root=/dev/ram0 rdinit=/init ${BK3_APPEND}" \
  "${SER[@]}" "$@"
