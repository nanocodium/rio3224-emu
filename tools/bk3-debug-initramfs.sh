#!/bin/bash
# Build Brooklyn initramfs variants for QEMU:
#   initramfs-qemu.cpio.gz   : user squashfs embedded and loop-mounted (QEMU's QSPI reads are unusable for the
#                              kernel driver), synthesized capability file, no console getty on the dac UART
#   initramfs-debug.cpio.gz  : same plus a root shell on ttyPS0 (UART A) and rcS in the background
#   initramfs-debugB.cpio.gz : same plus a root shell on ttyPS1 (UART B) so UART A can carry the dac link
#   initramfs-diagB.cpio.gz  : same as qemu plus a background job dumping the Yamaha logs to ttyPS1 every 30 s
S=/mnt/d/rio3224-emu/fw/d2/bk3/_FWUpdate_Rio3224-D2_v4020503_sw501.dnt.extracted
O=/mnt/d/rio3224-emu/fw/d2/bk3/qemu
build() {   # $1 = output name, $2 = shell tty ("" = none), $3 = diag dump tty ("" = none)
  rm -rf /tmp/bk3-ir && cp -a $S/initramfs /tmp/bk3-ir
  cp $S/rootfs.squashfs /tmp/bk3-ir/usr/local.sqfs
  # synthesized module capability (tools/bk3-mkcap.py) replaces the unreadable flash partition
  if [ -f $O/capability.bin ]; then
    cp $O/capability.bin /tmp/bk3-ir/etc/capability.bin
    sed -i 's#^\(\s*\)printf "Mounting MTD partitions: "#\1rm -f /dev/flash/capability; ln -sf /etc/capability.bin /dev/flash/capability\n\1printf "Mounting MTD partitions: "#' /tmp/bk3-ir/etc/init.d/S10mtd
  fi
  # mount the embedded squashfs instead of the flash partition; do not block forever on the Dante apps marker
  sed -i -e 's#^USER_PARTITION="/dev/flash/userblock"#USER_PARTITION="/usr/local.sqfs"#' \
         -e 's#mount -t $USER_PARTITION_TYPE $USER_PARTITION $USER_PARTITION_DIR#mount -t $USER_PARTITION_TYPE -o loop $USER_PARTITION $USER_PARTITION_DIR; (sleep 20; touch /tmp/apps_done) \&#' \
         /tmp/bk3-ir/etc/init.d/S90userapps
  if [ -n "$2" ]; then
    sed -i -e "s#^ttyConsole::respawn:.*#$2::respawn:-/bin/sh#" \
           -e 's#^::sysinit:/etc/init.d/rcS$#::sysinit:/bin/sh -c "/etc/init.d/rcS \&"#' /tmp/bk3-ir/etc/inittab
  else
    sed -i -e 's#^ttyConsole::respawn:.*##' /tmp/bk3-ir/etc/inittab
  fi
  if [ -n "$3" ]; then
    cat > /tmp/bk3-ir/etc/init.d/S95diag <<DIAG
#!/bin/sh
# emulator diagnostics: dump the Yamaha daemon logs to $3 every 30 s
( while true; do sleep 30
    { echo "=== DIAG uptime \$(cat /proc/uptime) ==="; echo "--- ymhinit.log"; grep -vE "^\\[.*\\] *: 0x" /tmp/ymhinit.log 2>/dev/null
      echo "--- dac.log"; sed -n '9,60p' /tmp/dac.log 2>/dev/null; echo "--- haremote_client.log"; sed -n '1,20p' /tmp/haremote_client.log 2>/dev/null
      echo "--- vsock.log"; sed -n '1,6p' /tmp/vsock.log 2>/dev/null; echo "--- ps"; ps | grep -E "dac|conmon|vsock|ymhinit" | grep -v grep
      echo "--- dac_info"; sed -n '1,60p' /tmp/dac_info 2>/dev/null
      echo "--- dante_apec.log"; sed -n '1,40p' /tmp/dante_apec.log 2>/dev/null; echo "--- dante_manager.log (tail)"; sed -n '30,80p' /tmp/dante_manager.log 2>/dev/null
      echo "--- dante_conmon_server.log"; sed -n '1,30p' /tmp/dante_conmon_server.log 2>/dev/null; echo "--- dante_ptp.log"; sed -n '1,12p' /tmp/dante_ptp.log 2>/dev/null
      echo "=== END ==="; } > /dev/$3 2>&1
  done ) &
DIAG
    chmod +x /tmp/bk3-ir/etc/init.d/S95diag
  fi
  ( cd /tmp/bk3-ir && find . | cpio -o -H newc 2>/dev/null | gzip -9 > $O/$1 )
  ls -la $O/$1
}
build initramfs-qemu.cpio.gz "" ""
build initramfs-debug.cpio.gz ttyPS0 ""
build initramfs-debugB.cpio.gz ttyPS1 ""
build initramfs-diagB.cpio.gz "" ttyPS1
