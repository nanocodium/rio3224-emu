#!/bin/bash
# Headless Ghidra analysis + decompile export of RIO2_PROG.BIN (run inside WSL: bash tools/ghidra-rio.sh)
set -e
G=~/tools/ghidra_12.1.4_PUBLIC
W=~/rio3224
REPO=/mnt/d/rio3224-emu
mkdir -p $W/ghidra-proj
cp -f $REPO/fw/d2/RIO2_PROG.BIN $W/
cd $W
rm -rf $W/ghidra-proj/rio.rep $W/ghidra-proj/rio.gpr
setsid nohup $G/support/analyzeHeadless $W/ghidra-proj rio \
  -import $W/RIO2_PROG.BIN -processor "SuperH:BE:32:SH-2A" \
  -loader BinaryLoader -loader-baseAddr 0x0C0F0FF0 \
  -scriptPath $REPO/re/ghidra_scripts \
  -preScript PrepRio.java \
  -postScript ListSymbols.java $REPO/re/RIO2_PROG.symbols.txt \
  -postScript ExportDecomp.java $REPO/re/RIO2_PROG.decomp.c \
  > $W/ghidra-rio.log 2>&1 < /dev/null &
echo "started pid $!"
