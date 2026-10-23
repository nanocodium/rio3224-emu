# Rio3224-D2 — what is inside (from firmware v1.91, `rio3224d2_firm191.zip`)

## Update package: `rio3224-d2_v1_91_rrmt.bin`
Yamaha `#YAMAHA MBDFArchive` container (big-endian; `tools/mbdf_unpack.py`):

| entry | size (unpacked) | what |
|---|---|---|
| `data/FWUpdate_Rio3224-D2_v4020401_sw310.dnt` | 4.95 MB | Dante **Brooklyn II** image (MicroBlaze, U-Boot 1.3.81, Linux 2.4, 2× CramFS, Spartan-6 bitstream) |
| `data/FWUpdate_Rio3224-D2_v4020503_sw501.dnt` | 19.9 MB | Dante **Brooklyn III** image (Zynq-7000, U-Boot 2019.01, Linux 4.19.36, initramfs + SquashFS, PL bitstream) |
| `data/RIO2_PROG.BIN` | 4.33 MB | host MCU program ("bank1-shfirm.bin"), 16-byte header `V191 \0` + 0xFF, code from offset 0x10 |

Each `#FILE` record: magic(0x18) · u32 · u32 zlib-size · u32 · u16 namelen · u16 · u32 flags · name (4-aligned) · zlib data (4-aligned). Archive ends with `#END`.

## Host MCU: Renesas **SH7203** (SH-2A + FPU, 200 MHz), RTOS **HI7000/4 V.2.02.06** (µITRON 4.0)
Firmware is C++ (STLport), project `C:\Project\Git\16RioII\RioII\source\…` (modules: cmnd, dante/BCA, dantemgr, mem/fls_hndl, network, oled, r2d, scp, test/diag; pa_common: dbgc, ethernet, file/altStore; tech: fmdc (AMD NOR flash driver), hap_ctrl, miniLCD, mms (XML parameter tree, expat), vcom, vsock).

Memory map seen from the firmware (literal pools):

| range | what |
|---|---|
| 0x00000000 CS0 | NOR flash (AMD/CFI command set, `fmd_amd16/32`), holds boot loader + `bank1-shfirm.bin` |
| 0x04000000 CS1 | main FPGA register window (very heavily used: 0x04000000, 0x04004d00/4e00/6400/6d00, 0x04313xxx, 0x04323xxx …) |
| 0x0C000000 CS3 | SDRAM. Program loaded at **0x0C0F1000** (file offset 0x10), data/heap up to ≥0x0C5Fxxxx |
| 0x18000000 CS6 | a few refs (TBD: DM9000 / OLED / CPLD) |
| 0xFFF80000–0xFFF9FFFF | on-chip RAM 128 KB (kernel work area) |
| 0xFFFC0000… | BSC, UBC 0xFFFC0400, cache 0xFFFC1000 |
| 0xFFFE0000 WDT/CPG · 0xFFFE0800 INTC · 0xFFFE1000 DMAC · 0xFFFE3800 PFC · 0xFFFE4000 MTU2 · 0xFFFE5800 (TBD) · 0xFFFE8000/8800/9000/9800 SCIF0–3 · 0xFFFEC000 CMT · 0xFFFEE000.. IIC3 ch0–3 · 0xFFFFC000.. SSI · 0xFFFFF000 RSPI · 0xFFFFFC00 ADC | SH7203 on-chip peripherals |

Peripherals/board items named by the firmware: 4 FPGAs (Intel MAX 10 `10M02`/`10M04`, versions `FPGA_VER_1..4`, board IDs `FPGA_BID_1..4`, programmed in-system by the embedded **Altera Jam STAPL ByteCode Player 2.2**), "HA FPGA%d" over SPI, HA A/D "HAAD" boards (gain 0..?), AES/EBU transmitters **AK4101** and **CS8406** (two board variants), OLED front display (`MINILCD`, `oled_mainTask`), encoder + left/right switches, 4 DIP switches, +48V master switch, power LEDs A/B, 2 thermo sensors (inside/outside), fan (high/low), PSU signals `/UVLO /POFF1 /POFF2`, `/UNLOCK`, `/IRQ_PLD`, reset lines `res_dsp res_mac res_dnt res_ada res_hacpu`, `pllinh`, `gpo[0-3]`, "PLLPU", word clock ext 48/96, varipitch, DM9000 (diag item), L2 switch (via Dante module).

Debug console ("YAMAHA Rio2-Series Debug Monitor Program") on SCIF0 (`scif0_usr.c`) and as a TCP "DiagConsole". Top commands: `? log logm mset mrd mdmp`; modules: `uITRON` (task/sema/flag/mbox), `common`, HA (`set gain|48v|hpfon|hpffreq|gcon|gcgain`, `get …`, `setprm/getprm` MMS), network (`ifn ipadr dhcp netmask gwadr addmuladr testperftcp …`), reliability (`report config routing current hangup pwoncnt … ehist eset eclear mclear`), update (`get protocol|productname|version|updatestate`, `fileput xmodem-*`, `write`, `erase`, `reboot`).

Parameter tree (MMS, XML embedded in firmware): functions `Dante`, `Setup` (UI: LastScreenType, PanelLock, FanSpeed, UnitId 0..127, StartupMode, SwitchMode …), `Setup2` (OutputLoadImpedance), `NonVolatile` (32× InputChannel: HeadAmp 48V, Gain −6..66 dB; GainCompensation On/StartGain; HPF On/Frequency), `System`, `Reliability1..3`.

## Dante module ↔ host link
* `DNT_UART_A` → Brooklyn `dac` (Dante Application Core; Yamaha "BCA" protocol with SLIP framing, `bca_controltask.cpp`), `DNT_UART_B` → `conmon_haremote_client -port=1` (HA remote control from consoles via ConMon).
* SPI pipe (`/dev/spi/pipe8` on the module) → `vsock_core`: the host's TCP/UDP sockets are proxied through the Dante module's Linux IP stack ("YMH_SPIETHER" capability). R Remote / firmware update TCP server (`firmServer`) live on the host but are reached through this.
* GPIs `DNT GPI0/1`, `DNT AUXB1`, `DNT_MUTE`, audio TDM `DNT_AUD_RIO32`.
* Brooklyn III: DTS model "Audinate Brooklyn3", Zynq PS UART0/UART1 to host, PS SPI0 (16 CS) , QSPI flash with partitions skd/fsbl/safefpga/safeboot/fpga/boot/kernel/rootfs/userarea/brdinfo/license/capability/config, PL "akashi" Dante engine at 0x60000000 (irqs ts/rx/tx/rxerr/smi), Marvell 88E6350R switch via SMI. Userland: `dac`, `conmon_server`, `mdnsd`, `ptp`, `apec3`, `ipcore_manager`, Yamaha `ymhinit` → launches `dac -t`, `conmon_haremote_client`, `vsock_core`.
* Brooklyn II: MicroBlaze, Linux 2.4 (`linux-uBlaze-2-4`), same Yamaha userland (`ymhinit`, `vsock_core`, `dac`) on CramFS.

## Other model IDs known to the firmware
Rio3224-D/D2, Rio1608-D/D2, Ri8-D, Ro8-D, RMio64-D, RSio64-D, Tio1608-D, CL5/3/1, QL5/1, MTX5-D, XMV series, NUAGE IO, AIC128-D, MY16-AUD, NEXO NXDT104/NXAMP, Shure ULXD4Q/D, Aviom D800, AD8HR, DVS.

## Findings from running the firmware in the emulator (emu/)

* **MCU identity**: no single Renesas manual matches. The firmware uses SH7203-style peripheral layouts
  (SCIF with FIFO at 0xFFFE8000/0xFFFE8800, 4-byte register spacing; ADC at 0xFFFE5800..5822 with two
  conversion groups; IIC3 at 0xFFFEE000) **and** the SH7216-family interrupt vector table (MTU2 ch0
  TGIA=156, ch1 TGIA=164, ch5 TGIU/V/W=196..198 used as the 10 ms RTOS tick, SCIF0/1 ERI/RXI/TXI/TEI at
  240..247 with their priorities in IPR14) and an MTU2 with channel 5 (U/V/W counters at 0xFFFE4080..40B6).
  Manuals checked: SH7203, SH7205, SH7216, SH7239, SH7266/67, SH7280/86 (`docs/*.txt`).
* **Peripheral clock Pφ = 16 MHz**: SCIF0 SCBRR=12 → 38400 baud, MTU2 ch5 W = 40000 counts at Pφ/4 → 10 ms
  tick, MTU0 TGRA=7999 → 500 µs, MTU1 → 1 s; the IIC3 driver hard-codes 16,000,000 Hz.
* **Flash** (CS0, read through the cache-disabled mirror 0x20000000): two x16 AMD/Spansion-command-set chips
  on a 32-bit bus (`fmd_amd32`: commands doubled, 0x00AA00AA → byte offset 0x1554, CFI query 0x98 at 0x154);
  the driver is CFI-driven (reads the 100-byte query block, write-buffer programming 0x25/0x29).
  The firmware addresses part of its constant data 0x00-based (e.g. the module init table at
  0x002F8E38), so the program image must also be present in flash at offset **0x000F1000** (=
  SDRAM load address − 0x0C000000); the bank header lies at 0xF0FF0. `emu/` keeps it there.
* **Board PLD at 0x40001400** (16-bit): +0 reset/enable bits (1,2,3,5,7, bit4 inverted), +2 bits 0x100
  (inverted)/0x200, +4 bits 12:8 = board/sheet id (9 → Rio3224-D2, 0x19 → Rio1608-D2 family codes),
  +0xA inputs incl. DIP switches (active low) in bits 4:1.
* **Kernel**: HI7000/4 with register banks (IBNR=0x4000), VBR=0x0C500000, interrupt dispatch table at
  0x0C500400, idle task at 0x0C35FE00 incrementing a CPU-load counter at 0x0C7FFFF0.
  Debug monitor on SCIF0 (38400 8N1); sysdown report goes there too.
* **Parameter store** ("FROM"/altStore, `from_scene_drv.cpp`): two banks of 0xBE4-byte CRC-protected
  records in flash; a blank flash yields `WMID_MEMORY_ERROR` + asserts but the system keeps running.

## Findings, part 2: board devices reached from the host MCU (emu/ now models all of these)

### FPGA identification bus (PLD +2 bits 1..7)
7-bit command MSB first, one turnaround clock, then 8 reply bits sampled before each clock on the four MISO
lines (bits 4..7 = FPGA 1..4). Commands: `0` = device code (the model table at 0x0c2d25b8 expects
`(code<<8)|version` = 0x0301 for the Rio3224-D2, i.e. code 3, version 1), `1` = version, `2` = status with the
device type in bits 6:4 (1 = "AK (10M02)"). The four MAX 10 devices are the **head-amp FPGAs** ("HA FPGA%d"),
eight preamps each. The "UP port" flags the DSP task prints are PLD +6 bits 12..15, driven by the host itself.

### Main FPGA on CS5 (0x14000000, accessed through 0x34000000)
| offset | meaning |
|---|---|
| +0x0002 | clock source select: bits 5:3 = 0x00/0x08/0x10 inputs 1..3, 0x18 inputs 4..7, 0x20 Dante (source 8); bit0 = internal; bits 2:1 mode (firmware writes 0x0026) |
| +0x0004 | status, bit 0 and bit 15 polled by the clock controller |
| +0x0006 | lock bits: bit1..3 inputs 1..3, bit5 = Dante module word clock locked |
| +0x000A | period counter of the selected clock (10 bits) |
| +0x000C/0E/10/14 | period counters of inputs 1,2,3 and Dante: 24.576 MHz / fs (0x200 @48k, 0x22D @44.1k, 0x100 @96k; the DIO accepts 0x1DE..0x216 for 48k) |
| +0x0016/18/1A | output control bits (0,1,8,9 / bit0 / bit0) |
| +0x0800, +0x0810 | two 16-byte channel-control blocks (+6 bits 12:8 busy) |
| +0x1000 | OLED byte port: +0 control (bit0 busy, bits 5:4 mode), +2 data (low byte), +6 FIFO counts (64-byte TX FIFO) |
| +0x1802 | display/misc control bits |

The DIO task (`DIO: Clock Status[0].lock/sync/freq`, `LOCK !!`, `IDLE !!`, `bClockisLock = 1`) only needs the
Dante lock bit and a sane counter. Word-clock source is fixed to 8 (Dante module) on this model.

### OLED: Solomon SSD1322 behind the CS5 byte FIFO
D/C# is PLD +6 bit 7 (set = data), reset is PLD +6 bits 10/11. Init: `FD 12`, `15 17 60` (columns 0x17..0x60 =
296 px), `75 00 2F` (48 rows), `B3 00 CA 2F A2 00 A1 00 AB 01 A0 00 11 B4 A2 B5 C1 2C B8 <15 gray levels> 00 B1 92
… BB 1E B6 0F BE 03 A6`, then a full RAM clear (`15 00 77 75 00 7F 5C` + zeros). Pixels are packed by the
firmware as byte0 = px3:px2, byte1 = px1:px0 per column address (FUN_0c13495c). Panel = **296 × 48, 4 bpp**.
`rio-emu --oled file.pgm` dumps it; `--oled-ascii` prints it. At boot the firmware shows a dialog with an OK
button (the MAC-address / Dante-module error dialog).

### DSP interface on CS4 (0x10000000, accessed through 0x30000000)
Register transport (FUN_0c13b6a2 write / FUN_0c13b522 read): 32 data words at +0x00..+0x7C, command at +0x80
(`reg | (count-1)<<16`, bit31 busy, bit30 error), device at +0x84 (bit 8 = read). Devices: 7 and 8 = DSP
coefficient memories (regs 0x4A.. per input channel, 0x62..0x144 step 6, 0x166..0x16D), 0xD = DSP control
(regs 0x00..0x12; 0x11 holds the fs/mode word 0x11414), 0, 4, 6 = others. +0x88 config (0x238), +0x94 enable
(0x80), +0x8E 16-bit meter port (write `0x8000|index`, read level; 32+32+24 meters, FUN_0c160a54),
+0xA8 serial transmit FIFO of 32-bit frames for the converter chips (`0F000005…`, `42041000 …`).
The DSP task comes alive (`DSP: state DEAD -> ALIVE fs = 3`) without reading anything back except what it wrote.

### Front-panel LEDs (PLD +2 bits 0, 8, 9, 10, 12)
FUN_0c1c7914 builds a 199-word waveform: bit 8 held high, bit 9 = clock, bit 12 = data chain A, bit 0 = data
chain B, bit 10 = latch pulse; 96 bits per chain, written in one burst by FUN_0c1c87c6 (`mask 0x1701`).
Map for sheet 9: chain A carries LED indices 0x5F..0x00 (groups of 8, reversed), chain B carries 0x6F..0x60,
0x77..0x70 on its last 24 positions — 96 input-channel LEDs (3 per channel) + 24 output LEDs.

### Head-amp control link (SCIF1, 0xFFFE8800)
Two-byte frames from the host only: `0xC0 | fpga<<3 | (7-channel)` then `gain<<1 | phantom` (gain index
0..0x4F, FUN_0c173440/FUN_0c173532/FUN_0c17363a). The "HA FPGA%d VERSION" comes from the PLD ID bus, not from
SCIF1. SCIF0 is the debug console; SCIF2/3 are not touched directly by the code found so far.

### DIO ↔ DSP system-info protocol (why the DSP stays DEAD on a too-fast clock)
Master struct at 0x0C75F974 (0x1C bytes), updated by messages (FUN_0c1c3b1c): +0 state, +2 model, +8
leaderLock (0 → 2 locking → 1 locked), +0xA wordClock, +0xC lastFsCode, +0x10 currentFsCode (3 = 48 kHz),
+0x12/+0x14/+0x16 set by other senders (sound permission etc.). Each change raises flag 0x20000000 for the DSP
task, which compares against its own copies: `hSoundPermission` becomes 1 only on an event where the fs is valid
and `wordClock` did **not** change. The DIO reports the word clock long before the Dante module can supply a
clock, so on real hardware the lock events arrive separately; the emulator therefore makes the Dante clock
appear after a delay (`--dante-clock-ms 3000,10000,10400` = present, lost, present), after which the DSP logs
`DSP: state = DEAD: DSP_RESTART` / `DEAD -> ALIVE fs = 3` and the HA link sends the 32 initial gain frames.

### PLD register summary (update)
+0 control: bit0 res_dsp, bit1 res_mac, bit2 res_dnt, bit3 res_ada, bit7 res_hacpu, bits 4/5 pllinh, bits 8..11
gpo[0-3] (names from the `misc` debug module). +4: sheet id in bits 12:8, bits 2/13/14/15 outputs. +6: OLED D/C#
(bit7), OLED reset (bits 10/11), UP-port flags (bits 12..15). +0xA / +0x10: switch inputs.

### Dante module link (recovered 2026-10-02)
* **DNT_UART_A / DNT_UART_B are FPGA UARTs** in the main FPGA at CS5 +0x800 (channel 0, Brooklyn UART0 =
  `/dev/ttyS0`, the Yamaha `dac -t` process) and +0x810 (channel 1, Brooklyn UART1 = `/dev/ttyS1`,
  `conmon_haremote_client -port=1`): +0 control (bit1 = TX enable), +2 TX data, +4 RX data, +6 status
  (bits 3:0 TX FIFO fill of 8, bits 12:8 RX fill up to 31). The SH-2A's SCIF2/3 are unused.
* **Interrupts**: CS5 +0x1800 = interrupt status (read-to-clear events: 0x10/0x40 TX FIFO empty ch0/1,
  0x100|0x200 / 0x400|0x800 RX data ch0/1, 0x0001/0x1000 OLED port), +0x1802 = enable. The FPGA line is
  **PLD +4 bit 4, active low**, routed to the SH-2A IRQ1 (vector 65, level, IPR01 = 0xAA00). The IRQ1 handler
  (0x0C22F42C) first tests PLD +4 bit 6 (active low = power-off detect → `SCN: [POWER OFF] RECOVERY`),
  then bit 4 → reads +0x1800 and dispatches to FUN_0c1604dc (UARTs) / FUN_0c1608be (OLED).
  IRQ0 (vector 64) is the **DM9000 Ethernet controller on CS6 (0x18000000, index/data at +0/+2)**.
* **Module status lines on PLD +4**: bit0 DNT_GPI_0, bit1 DNT_GPI_1, bit3 DNT_AUXB (active high). A 10 ms task
  counts transitions per 200 polls: GPI0 = 2 → "DC alive", AUXB 19..21 → normal mode (10 Hz), 38..42 → safe mode
  (20 Hz), none for 3 windows → module down (`BK2_DC_HARD_RESET_DETECTED`). CS5 +4 bit 15 = DNT_MUTE.
* The host sends nothing on the UARTs until the module speaks (BCA over SLIP from `dac`), so the emulator
  bridges the two FPGA UARTs to TCP (`--dnt-uart 5550,5551`) for a Brooklyn III instance running in QEMU
  (`tools/brooklyn-run.sh`, flash image + 16 MB device tree from `tools/bk3-mkimage.py`; the Zynq QSPI flash is
  MTD drive **unit 8** in QEMU's xilinx-zynq-a9).

### Running the Dante module (Brooklyn III) in QEMU — practical notes
* `qemu-system-arm -M xilinx-zynq-a9` boots the extracted `zImage` + `kernel.dtb` + `initramfs.cpio.gz` (Linux
  4.19.36, Dante 4.2.5.3 userland: ipcore_manager, apec3, conmon_server, mdnsd, ptp). The Dante PL ("akashi")
  is absent, so the network engine only logs SMI timeouts; the `syd_*` character devices still exist.
* The QSPI flash is QEMU MTD drive **unit 8** (`-drive if=mtd,unit=8,...`); QEMU's model is 16 MB while the real
  part is 64 MB, so `tools/bk3-mkimage.py` rewrites the partition table in a copy of the device tree. However the
  vendor QSPI driver reads garbage through QEMU's controller model (every transfer starts with 4 zero bytes), so
  the user squashfs cannot be mounted from flash: `tools/bk3-debug-initramfs.sh` embeds `rootfs.squashfs` in the
  initramfs and loop-mounts it on `/usr/local`, where `S10YAMAHA_YMHINIT` starts `ymhinit`.
* The factory **capability partition** (Audinate "module config", big-endian structs, layout recovered from dac's
  DWARF in `fw/d2/bk3/qemu/module_config_types.txt`) is not in the update package. `tools/bk3-mkcap.py` synthesizes
  one (manufacturer capabilities `YMH_DAC | YMH_HAREMOTE | YMH_SPIETHER` = 0x1801, UART0 = mode 3 BRIDGE for
  `dac -t`, UART1 = mode 6 HAREMOTE); without it `ymhinit` sees 0xFFFFFFFF and starts `dac -t -e` (Ethernet mode)
  and `conmon_haremote_client -tcp_port=…`. `module_config -x` (stricter parser) still rejects the synthesized file.
* `dac` has full debug symbols (gdb inside the module works: `gdb --args /usr/local/bin/dac -t`); it exits via
  `mainExit()` when `dacExecuteTask()` returns 0x80000016 with no tasks, then crashes in `dacDevicePrintInfo` —
  i.e. its startup failed before the task loop (seen while the capability was unreadable).
* `/tmp/apps_done` is created by `ipcore_manager` only when the Dante apps are up; the QEMU initramfs variants
  touch it 20 s after mounting `/usr/local` so `S90userapps` proceeds.
* Zynq UART0 (`ttyPS0`, 0xE0000000, `/dev/ttyS0` = DNT_UART_A) is QEMU `-serial` #0; the kernel's early boot
  console always lands there, so the module must run with `console=ttynull` when UART A is bridged to the host.
* With the synthesized capability, `ymhinit` starts `dac -t`, `conmon_haremote_client -port=1 -speed=38400` and
  `vsock_core`; `dac` then loops on `[CONMON][INFO]Timeout wait DANTE_READY... retry`: it needs the conmon status
  "DANTE_READY" from `ipcore_manager`, which depends on the audio engine (`apec3`) — and apec3 aborts with
  `error opening '/cap/license.cert'` / `APE failed to communicate - Unlicensed` because the per-device
  **license partition** (and the Dante PL itself) is missing. The real `dac` therefore never speaks on the UART in
  QEMU. Consequence: the emulator needs a behavioural Dante-module model speaking dac's UART protocol (BCA over
  SLIP); dac's DWARF symbols are the specification (`fw/d2/bk3/qemu/dac_proto_index.txt`).

### Host ↔ Dante module protocol ("BCA", DNT_UART_A) — from dac's symbols
Frame `FF <esc(header+payload)> <esc(checksum)> FE`; bytes 0xFD..0xFF are escaped with 0xFD; checksum = sum of
all bytes between preamble and checksum (escapes included) mod 256. Header: category (1 SET, 2 GET, 3 NOTIFY,
|0x80 = response), cmd, ver=1, seq, result, origin, length (BE16 = payload bytes + escaped payload bytes).
Command numbers: `enum BKLYN_CMD` (BASE_INFO 0, BASE_MODE 2, BASE_CAPABILITY 3, BASE_MANUFACTURER 4, …,
DEVICE_FS 17, DEVICE_CLOCK_SYNC 23, DEVICE_LINK 24, BROWSE_* 32.., ROUTING_* 48.., NETWORK_* 64.., CONNECT_DEVICE 82,
HAREMOTE_BRIDGE_MODE 144, ERROR 240 …; full list in `fw/d2/bk3/qemu/dac_types_enums.txt`). Every payload starts
with the device id (BE16, 0 = local module); sizes per command in dac's `getCmdParamSize` table (0:28 1:3 2:4 3:18
4:130 5:18 7:7 17:4 22:5 23:4 24:4 25:22 33:34 64:18 65:9 82:4 144:4 …). On start dac sends NOTIFY BASE_INFO
(hard_id, aud_firm[4], aud_soft[4], ymh_firm[4], protocol, tx/rx channel counts, model_id[8]), then
CONNECT_DEVICE and BROWSE_COMPLETE notifications and a BASE_MODE notification (STARTUP→MAIN).
`emu/src/board/dantemodule.{h,cpp}` implements this module model (default; `--no-dante-model` or
`--dnt-uart` to bridge the UART to TCP instead).

## Findings part 3 (Dante link bring-up, board inputs)

### Host <-> module BCA link, verified against dac and the host
* The FPGA UART RX FIFO is **16 bytes deep**: the host ISR (FUN_0c16044a) reads `(status >> 8) & 0x1f` bytes
  into a 16-byte stack buffer, so a model offering more bytes corrupts the host stack (frames were lost until
  the emulator capped the FIFO at 16).
* Frame coding (dac `createWriteData`): `FF` + escaped(header + payload) + [`FD`] checksum + `FE`; bytes
  `FD/FE/FF` are escaped by a preceding `FD` and sent literally; the header `length` is the payload length plus
  the number of escaped payload bytes; the checksum is the 8-bit sum of every byte emitted after the preamble
  (escape bytes included). An escaped byte is data even when it is `FF` (the first model decoder restarted the
  frame on `FD FF` and lost the host's 220-byte GET PATCHES request).
* Notifications are sent through dac's `sendCommand` path one at a time: the host acknowledges each one with a
  response frame (category | 0x80, same seq) and dac waits for it. Sending the start-up burst back to back loses
  frames on the host. `origin` in responses is 0 (immediate) or 1 (deferred after a ConMon round trip); dac does
  not echo the request's origin.
* The host's request table (12-byte entries at 0x0C277A74: `u16 id, u8 2, u8 cbIdx, u8 category (1 SET / 2 GET),
  u8 cmd, u8 ver, u8 0, u16 len, u16 flags`) marks only GET CLOCK_STATUS (id 0x114, cmd 0x19) with flags = 1:
  that request is completed by a **NOTIFY** CLOCK_STATUS (dac defers the GET to ConMon and reports through
  `dacCmdSendClockStateNotify`). The matcher (FUN_0c105c32 / FUN_0c10965a) only looks at category-3 frames for
  such records; a plain response is ignored and the request times out (error 0x8005, "BCA_CALLBACK_RESULT_RETRY").
* The per-machine request records (0x91C bytes each at 0x0C5BD250) carry the retry timer; FUN_0c105f8c returns
  0x8005 when the retries are exhausted.
* `dacCmdSendStartNotify` = NOTIFY BASE_INFO(0) -> CONNECT_DEVICE(82) -> BROWSE_COMPLETE(38); then the mode
  notify (2). BROWSE_COMPLETE payload: dev, code (monitor state 3 COMPLETED -> 1, 4/5/6 errors -> 2/3/0x80),
  flags (sub_status, sub_keepalive, support_keepalive), dnssd != support_keepalive. Any code but 1 makes the
  host send SET BROWSE_RESTART forever.
* Observed host sequence after the start notify: 15 x SET L2SW_WRITE (Marvell switch registers), GETs
  3,10,11,104,105,106,65,64(x2),22(x2),23,25,69,24,18,16,20,19,17,33,44,7,0,4; SET MANUFACTURER (70 bytes),
  SET DEVICE_LABEL ("Y001-24-D2-000001" = unit id + model + 6 digits); GET PATCHES (24 rx channels); GET
  CHANNEL_LABELS (32 tx, then 24 rx; 35-byte entries {id, label[32], result}); then a GET CLOCK_STATUS poll
  (origin 2) about every second. The DSP goes DEAD -> ALIVE as soon as the Dante clock/info is accepted.
* dac parameter sizes per command are in `fw/d2/bk3/qemu/dac_cmd_sizes.txt` (getCmdParamSize table); the
  model answers unknown GETs with zero-filled parameters of that size so the host parser stays in bounds (a
  2-byte error reply to GET CHANNEL_LABELS crashed the host: jump through 0xFFFFFFFF).
* After SET DEVICE_LABEL dac renames the device (dr_device_rename), then NOTIFY DEVICE_LABEL(33), NOTIFY
  BROWSE_RESTART(36) and a new BROWSE_COMPLETE; after SET MANUFACTURER it answers with the 130-byte
  manufacturer parameter and later NOTIFY MANUFACTURER(4).

### Board-info flash records (host)
* Descriptor table at 0x0C2C8F90: four records in the boot bank, each `u32 0x00001E61` magic then data:
  0xE0000 MAC (6), 0xE0040 string (17, serial), 0xE0080 string (33, device label), 0xE00C0 (40).
  A blank MAC record shows "WMID_ILLEGAL_MAC_ADDRESS". The emulator writes defaults when the sector is erased
  (`--mac`).

### Fan and front-panel switches (PLD)
* PLD +4 bit 7 is the **fan alarm input, low = fan running** (FUN_0c1c82ca; high raises E00-05 "UNIT FAN HAS
  MALFUNCTIONED"). +0xA bits 5/6 (active low) are the fan speed status lines (FAN_HIGH / FAN_LOW).
* PNLSCAN task (FUN_0c1c7a48, object at 0x0C764C6C) reads six switch inputs through FUN_0c1c7700:
  [0] 4-bit DIP field (+0xA bits 4:1), [1] +0xA bit0, [3] +2 bit15, [4] +2 bit14, [5] +0 bit15 (all active low),
  debounced by FUN_0c1c772e. "TO UNLOCK - PRESS ALL BUTTONS FOR 3 SECONDS". Emulator: `--press SW@T0[-T1]`.
* UI messages go through FUN_0c241e18(WMID name); probing it (`--probe 0c241e18:r4,48`) lists the dialogs.

### Boot sequence as seen with the module model
1. Module announces (BASE_INFO/CONNECT_DEVICE/BROWSE_COMPLETE/MODE notifies, then NOTIFY NETWORK_SWMODE
   (dev, element 0, mode) as `dac`'s updateConMonChange does once ConMon reports the interface; each notify is
   acknowledged by the host).
2. Host: 15 x SET L2SW_WRITE, the GET burst (24 requests), SET MANUFACTURER (unconditional, every boot:
   model id 0, "Rio3224-D2", version 1.91 / "V1.91", the module's own device id), GET PATCHES, GET
   CHANNEL_LABELS (tx 32, rx 24), periodic GET CLOCK_STATUS (origin 2, completed by NOTIFY CLOCK_STATUS).
3. The device label is checked against "Y0<unit id hex>-": if it does not start with it the host prints
   `[DEV LABEL] ... LABEL:RESET` and renames the module to "Y001-" + the last 12 characters of the current
   name (`--dante-fresh`: "Rio3224-D2-123456" -> "Y001-24-D2-123456"); no restart is needed for that.
4. The switch mode: the host's settings sync (parameter 0x33, FUN_0c11cf0c -> FUN_0c11b21a) compares its
   stored module mode (status object +8, 2 = unknown) with the rear DIP setting; if different it queues
   manager command 5 (pending swmode), the apply loop sends SET NETWORK_SWMODE (dev, element 1, value) and
   the SET response (element 1) makes FUN_0c110ff8 request a restart: SET BASE_RESTART -> "[BK2 EVENT]
   START REBOOT SEQUENCE" -> module heartbeats stop -> heartbeat state 7 -> "START BK2 HARD RESET SEQUENCE"
   (PLD +0 bit 2 pulsed for 1 s) -> E00-03 DANTE MODULE ERROR until the module is back. Without the start-up
   swmode notify the host loses the race between that sync and the GET response and restarts the module at
   every boot.
5. The host also talks on DNT_UART_B (conmon_haremote_client): a 63-byte registration frame (`C0 'M' 'B'
   'C' 01 00 2A`, module MAC, FF x6, label) and five 32-byte parameter notifications; it does not wait for
   answers (`tools/uartb-decode.py`).
* Harmless host quirk: a diagnostics registry (FUN_0c1a9bb0 -> FUN_0c1a9b04, names like LED_POWER_A) is
  called with the first four bytes of a name as the pointer, so `strnlen` reads 0x4C45445F ("LED_") about
  200 times per minute (unmapped read, 0 in the emulator, bounded to 24 bytes on real hardware too).

### OLED dialogs and the front switches
* Error / information popups (blank text + "OK" in the emulator's rendering) are closed by front-panel switch
  5 = PLD +0 bit 15 (`--press 5@T`); switch 1 does nothing to them. Without a press the popup stays.

### AES/EBU transmitter (DIO "AES Device")
* PLD +0 bits 8..11 are a bit-banged serial port: bit8 nCS (gpo0), bit9 MOSI (gpo1), bit10 SCLK (gpo2), bit11
  MISO (input). The DIO task probes an **AK4101** (FUN_0c0fff32: 16 clocks of `0xC0, reg`, 8 clocks of data;
  present when reg 0 & 0x8C == 0x80) and otherwise a **CS8406** in SPI mode (FUN_0c13a148: chip address 0x20
  write / 0x21 read, MAP byte; present when reg 0x7F & 0xF0 == 0xE0). "AES(AK4101/CS8406) reset/init sequence
  start" follow. Emulator: `--aes 0|1|2`, `--log AES`.

### DM9000 Ethernet controller (CS6, IRQ0)
* Index port 0x18000000, data port 0x18000002 (16-bit). Driver init: NCR reset, PHY via EPCR/EPAR/EPDR
  (BMCR reset, ANAR 0x01E1, BMCR 0x1200), GPR power toggling, PAR = MAC, MAR broadcast hash, BPTR/FCTR/FCR,
  then IMR = 0x20: the stack waits for the **link-change interrupt** (ISR bit 5) before it enables reception.
  With the link up it runs DHCP (5 discovers, "ENET IP ADDRESS DHCP TIMEOUT") and falls back to a link-local
  address (ARP probes / announcements, "IP ADDRESS : 169.254.x.y"). Emulator: `--pcap FILE`, `--tap NAME`
  (Linux TAP, e.g. `sudo ip tuntap add dev tap0 mode tap user $USER`), `--log DM9000` / `--log ENET`.

### Unit ID / "encoder" (ADC)
* The PNLSCAN task's "encoder" (ROTARY_COUNT) is sampled through the SH-2A ADC (ADCSR 0xFFFE5820/22, ADDR
  0xFFFE5800..): a resistor-ladder rotary switch; the host derives UNIT ID "Y001" from it.

### AES transmitter: what the boot actually does
* The DIO "Lock check" stage (handler table 0x0C2FBBE8) probes the chip (FUN_0c14d540), prints
  "AES Device:AK4101", runs the reset step FUN_0c14d744 (AK4101 register 0 <- 0xC0 via FUN_0c0fffec: 24
  clocks `0xC1, reg, data`) and moves on. The "init sequence start" step (FUN_0c14d8ec) is only reachable from
  FUN_0c11fa9e, a debug-monitor `dio` sub-command dispatcher (0x0C11EF40.., sample-rate arguments 0/2/3), so
  it is not part of a normal boot. Device-table entries 27/28 (FUN_0c1ac196 "SYSTEM" parameters) hold the
  detected AES type (1 AK4101, 2 CS8406) and the DSP control words dev 0xD regs 0x10..0x12 are plain
  read-modify-write registers (0x00011414 / 0x00001414).

### DNT_UART_B (conmon_haremote_client) framing
* Host -> module messages are `len | C0 'M' 'B' 'C' 01 00 LL | payload | checksum`: `len` counts the bytes that
  follow it, the checksum is the one's complement of the 8-bit sum of those bytes (conmon_haremote_message_
  get_checksum), `LL` is the conmon payload length. The payload carries the host MAC, `FF x 6`, and Yamaha HA
  remote records (`10 07 11 05 00 01 00 00` + "Rio3224-D2-000001" in the first message, `10 07 11 08 ..`,
  `10 07 22 1x ..` later). The module only forwards these over Dante ConMon to a console; without a console
  there is nothing to answer, so the model leaves UART B silent and the host keeps sending (16 messages in
  the first 50 s).

### Front panel controls (from the usage counters)
* The maintenance report lists SWITCH_ON_COUNT(LEFT), SWITCH_ON_COUNT(RIGHT), SWITCH_ON_COUNT(ENCODER) and
  ROTARY_COUNT(ENCODER): the front panel has LEFT / RIGHT buttons and a push encoder (OLED menu), the encoder
  rotation being sampled through the ADC. PNLSCAN switch 5 (PLD +0 bit 15) acts as the popup "OK" button.
* OLED popups are a full-window redraw per frame (0x15 17 60 / 0x75 00 2F / 0x5C). The UI composes an 8-bit
  surface (frame object at 0x0CA686DC: pixels at 0x0CA6AD90, stride 0x24C, 48 rows) that FUN_0c13495c packs
  two pixels per byte through a brightness/palette table (0x0C2B56F8, FUN_0c134e90 = contrast index 0..20).
  The popup text IS present in that surface (value 0 on background 215/216) but comes out as uniform grey on
  the emulated panel while the OK button renders; the conversion or the SSD1322 unpacking for these values is
  still wrong somewhere (not resolved).

### Two OLED panels (resolves the "blank popup text")
* The front has **two SSD1322 panels** on the same CS5 byte port. PLD +6 bit 8 is the right panel's nCS,
  bit 9 the left panel's (both low at boot, so one init sequence programs both); bit 7 is D/C#. The UI composes
  a 589-pixel-wide canvas and flushes columns 0..295 to the left panel (channels 1-16, popup text) and
  296..588 to the right one (channels 17-32, popup OK button); FUN_0c1bc0b6(0/1) selects, FUN_0c1bc0e2
  deselects. The emulator keeps two controller models and dumps them side by side (`--oled`, 600x48).

### Front-panel LEDs decoded
* The firmware's LED controller (object at 0x0C764814, FUN_0c1c46be setter, 124 entries {on, mode, pattern}) is
  copied into the PNLSCAN object (+0x15) and shifted out through the two PLD chains using the sheet-9 map at
  0x0C2E56D0 (chain A carries ids 0..0x5F, chain B tail ids 0x60..0x77). Ids (FUN_0c1c5f84, three phases):
  0..31 = +48V per input (parameter 0x0B per channel, mode 1 while the +48V master is off), 32..63 = SIG per
  input, 64..95 = PEAK per input (meter object method +0x5C), 96..111 = SIG out 1-16, 112..119 = SIG AES out
  1-8, 120..123 = Dante primary / primary act / secondary / sync status (FUN_0c168bfe, set from the link states,
  not on the chains). The emulator's `--panel` output prints these by name.

## Findings part 4: the host's Dante manager (BCA) internals

* **Request table** 0x0C277A74 (12-byte entries {id, 2, cbIdx, cat, cmd, ver, 0, len, flags}): cbIdx is the
  callback index; a completed request becomes a *completion record* (0x90C bytes: u16 event code, u16
  request id, payload) with **event code = 0x200 + cbIdx** (0x1xx for notifies). Dispatcher 0x0C10E1F4
  (table 0x0C2AB03C {code, idx} -> handler table at *(object 0x0C598458) = 0x0C2FA440, 12-byte entries): the
  handlers 0x0C10E288.. only print `[BCA_CBK] BcaCallbackXxx(...)`; a record with code 0 prints
  `BCA_CALLBACK UNSUPPORTED : COMMAND=0, REQUEST_ID=n`. Records are produced by the converter
  FUN_0c1080d6 from a **conversion table** 0x0C277FCC (6-byte entries {code, cat (0x82 GET response / 0x81
  SET response / 0x03 notify), cmd, min length}); a (cat, cmd) pair missing from it leaves the code 0.
  BROWSE_DIV_DEVICE_LIST (44) exists only as a notify: `dac` answers the GET with result 2 (no payload) and
  sends one NOTIFY per device (dev, total, index (1-based, index == total ends the list), device id,
  connect type, label[32]); the host retries the GET after a timeout (`BCA_CALLBACK_RESULT_RETRY :
  ERROR CODE = 0xFFFF8005`) if the list never completes.
* **Response result codes** (frame header byte 4): 0/1 complete the request, 2 = "completed later by a
  notify" (the response is ignored, FUN_0c109378 gives it no flags), 3, 4 special, 0x80+ errors. `dac` puts
  the handler's return code there (dacCmdParse -> sendResponseCommand, origin always 0); the response
  header is {cat|0x80, cmd, ver 1, seq, result, origin, len16}.
* **Update events**: after the dispatcher, FUN_0c113114 re-queues the record into the update-event ring
  (0x910 entries, marker 0x13) processed by FUN_0c11d3f6/FUN_0c11d4ac (`EXECUTE DANTE UPDATE EVENT = n`
  when a handler rejects it, e.g. a bit depth other than 24/32 for event 0x210). Event handlers
  (FUN_0c10ec54 switch): 0x206 BASE_MODE -> ExecMode byte (+0x16B55: 0 startup, 1 main, 2 upgrade),
  0x207 BASE_CAPABILITY -> capability flags (+0x16B60, bit 1 lets the restart step run), 0x23A
  NETWORK_SWMODE -> FUN_0c110ff8 (payload = dev, element, value: element 1 requests a module restart,
  element 0 clears the pending swmode when it matches) and FUN_0c118cba (stored module mode, status +8).
* **Manager command ring** 0x0C638168 (0x100 x 0x1C, marker 0x13, enqueue FUN_0c11d430, dequeue
  FUN_0c11d39c, executor FUN_0c10f228): type 1 restart (normal), 2 factory reset, 4 manufacturer/version
  info, 5 set pending switch mode (value | 0x80), 6/7 encoding/sample rate, 0x21/0x22 hard-reset steps
  when ExecMode == 2.
* **Pending settings block** (manager object + 0x16B64, 14 bytes: {unit 1, 0, version 0x1910 = "1.91",
  then one byte per pending setting, -1 = none; byte 6 = switch mode, byte 12 = restart type}) is applied by
  FUN_0c113352 (apply functions FUN_0c1107d0 swmode ... FUN_0c1106a6 manufacturer, FUN_0c110528 label).
  Manager object = FUN_0c113a7c() (0x0C5FA478 in this image, 0x16B7C bytes); status object =
  FUN_0c11d2fe() (0x0C635ED4).
* **Module heartbeat state machine** (FUN_0c104210): states 1-3 alive variants, 5 normal running (sets
  +0x16B74 = 1), 6 safe mode, 7 down; a 5 -> 7 transition runs the hard-reset sequence (FUN_0c1104a0: PLD +0
  bit 2 high for 1000 ms) and posts event 0x7D7 -> "BK2_DC_HARD_RESET_DETECTED!!", error 10 = E00-03,
  WMID_DANTE_MODULE_ERROR; 0x7D8 safe mode, 0x7D9 error 13.
* **dacCmdParamConv layouts used**: SwMode (dev, element, value); DacMode (dev, code|flags, error) where the
  code is 0/1/2 for mode bits 0/1/2; DeviceCapability (dev, be32 flags, be16 x5, 2 bytes); Manufacturer
  (dev, manf id 8, manf name 32, model id 8, model name 32, be32 model ver, soft 8, firm 8, be32 caps,
  device id 8, string 16 = 130); SET Manufacturer (dev, model id 8, model name 32, version 4, device id 8,
  string 16 = 70); DivDeviceList (dev, total[, index, id, type, label 32]); bit depth (dev, bits, 0).

## Findings part 5: the audio hardware as the host programs it (basis of the PCM engine)

* **Head-amp link (SCIF1)**: 2-byte frames `0xC0 | fpga<<3 | (7 - channel)` then `gainIndex<<1 | phantom`, FPGA
  = channel / 8. gainIndex = HA gain + 6 (0..72 = -6..+66 dB, 1 dB steps; FUN_0c1205dc computes it as 10 - level
  where level is the nominal input level in dBu). The phantom bit already includes the +48V MASTER gating (the
  firmware sends it only while the master is on). Only sheets the firmware detected get frames.
* **DSP register map** (CS4 register transport, devices 7 = channels 0-7 and 16-23, 8 = channels 8-15 and 24-31;
  channels 16-31 live at +0xA8). Per bank of 8 channels (k = channel & 7), all words Q30 (0x40000000 = 1.0):
  `0x0A + 6k` filter-A block (word +0 control, +1..+5 = { -a2, -a1, b0, b1, b2 }; default a 100 Hz 2nd-order HPF),
  `0x3A + 2k` filter-A dry/wet pair (default {1, 0} = bypassed), `0x4A + k` gain word 1 (1.0; "trim" calibration,
  `dsp trim` sets 0.01 dB steps per 6 dB analog range), `0x52 + k` gain word 2 (0x4C106BA5 = +1.5 dB on every
  channel), `0x5A + k` gain word 3 (1.0), HPF blocks `0x62 + 6k` (k < 4) / `0x8A + 6(k-4)` (control word
  0x08000000, bit 7 pulsed twice on every HA gain change = the DSP's soft-mute/update strobe; +1..+5 biquad),
  HPF dry/wet pairs `0x7A + 2k` / `0xA2 + 2(k-4)` ({1,0} off, {0,1} on), `0x82`/`0xAA` x4 {0.5, 0, 0.5, 0}.
  The user HPF is 2nd order (12 dB/oct, -3 dB at the set frequency) and the frequency is quantised to the
  documented steps (set 120.0 -> 125 Hz; 800 -> 600 Hz max). Default 80 Hz off.
* **Meter port** (+0x8E): write index (| 0x8000 while the DSP task has meters disabled), read a 16-bit word whose
  bits 11:5 are a 7-bit level code (FUN_0c160b48: shown as code-1, 0x7F = over). Three banks via the index tables
  at 0x0C2A87F4 / 0x0C2A8834 / 0x0C2A8874 (32 input, 32 second input bank, 24 output meters). The DSP task reads
  the port only while a consumer asked for meters (event 0x100000 handler passes "wanted" to FUN_0c160a40).
* **Gain compensation** is not visible in the host's register writes: the host keeps GC on / start gain in its
  parameter tree (NONVOLATILE InputChannel/GainCompensation/On, /StartGain, storage 0x0C7A1FC8 / 0x0C7A2248 with
  20-byte records per channel; HeadAmp/48V 0x0C7A15C8, HPF/On 0x0C7A1848, HPF/Frequency 0x0C7A1AC8 (0.1 Hz),
  HeadAmp/Gain 0x0C7A1D48 (dB)) and the real DSP derives the compensation from the HA gain it receives over the
  head-amp data link. The engine reads the two GC parameters from that storage.
* **Debug monitor (SCIF0)**: `mms` module: `set gain|48v|hpfon|hpffreq <ch>|all <value>` (gain in 0.01 dB,
  hpffreq in 0.1 Hz), `get ...`, `getprm <ROOT> <path> <x> <y>` / `setprm` by name (fast-access ids only work for
  single parameters; `set gcon` is broken in V1.91: it zeroes StartGain), `getprm` alone lists all 101 parameter
  ids. `dsp meter` prints the DSP task's meter copies, `dsp trim`, `rep hgain|gcom|gcoms|hpfo|hpff|h48|switch|
  encoder` reports, `misc portr/portw/pwrseq`, `net setip/...`, `err info|set`. Lines longer than ~40
  characters are truncated. Emulator: `--console-script FILE` ("<ms> <text>" lines, markers on stderr).
* **Front panel inputs**: LEFT = PLD +2 bit 15 (switch 3), RIGHT = +2 bit 14 (switch 4), ENCODER push = +0 bit
  15 (switch 5), rotary encoder A/B = +0 bits 13/14 (FUN_0c1c8220: `(PLD[0] >> 13) & 3`), sampled by the
  firmware's timer interrupt into a 32-entry ring (FUN_0c165598 -> FUN_0c165476 quadrature decode, 2 transitions
  per count). On the GAIN home page any key selects INPUT 01, LEFT/RIGHT move the selection, turning the encoder
  changes that channel's HA gain immediately (1 dB per count, HA frame sent at once). Holding LEFT+RIGHT for ~2 s
  switches to the METER page ("INPUT nn [gain] NAME:" with 32 level bars). Emulator: `--rotate STEPS@T[,x,MS]`.

## Findings part 6: parameter store in flash, start-up mute, LED chain order

* **Parameter region at flash 0x700000** (the "FROM" bank the firmware restores at boot): 16-byte header
  `c1 01 00 01 | be32 size (0x0B3C) | be32 version (6) | ff fe | be16 check`, where check = two's complement of the
  byte sum of the payload [0x700010, 0x700000 + size) (verified against the firmware's own image, 0x9B7D). The
  payload is a sequence of serialized MMS banks, each `be32 length | "MMSXBIG\0" | pad | 32-char hash | schema
  (COL0 containers with child counts / record sizes, PR records {type, size, count, name}) | values`. Banks in V1.91:
  Setup at 0x700010 (values at 0x70035C: LastScreenType, PanelLock, FanSpeed, UnitId (0x70035F), **StartupMode
  (0x700360)**, SwitchMode, NetworkMode, Dimmer, ErrorMessage, Brightness, IP/mask/gateway), NonVolatile at
  0x700374 (values at 0x700580: 32 records of 11 bytes = {48V u8, Gain s16 in 0.01 dB, GC On u8, GC StartGain
  s16 in 0.01 dB, HPF On u8, HPF Frequency u32 in 0.1 Hz}; factory: off, -6.00 dB, off, -6.00 dB, off, 80.0 Hz),
  a third bank at 0x7006E0 (Dante / system). A second header form (`00 00 30 7c 00 00 00 06 ff ff crc MMSXBIG`)
  at 0x780000 holds Reliability3 (CRC not decoded). `tools/flash-set.py IN OUT startup=N|unitid=N|gain=CH:dB|...`
  edits these values and fixes the check word, i.e. it does what the unit's own "save" does.
* **Start-up mode decides the DIO mute** (the reason the emulator's meters stayed dark): at init FUN_0c14f724
  writes parameter 9 (Dev/DIO/Mute) once from Dev/UI/StartupMode (param 50; enum as the SETUP page names it:
  0 = REFRESH w/o MUTE, 1 = REFRESH, 2 = RESUME): mode 1 = REFRESH (factory default)
  writes 1 and the DIO stays muted ("Mute(DIO): ON", `rep current` Sync Status SYNCHRONIZATION, E01-02) until a
  controller (console / R Remote) refreshes the HA settings and clears the mute; modes 0 (REFRESH w/o MUTE) and 2 (RESUME)
  leave it 0, so once the clock state machine reaches IDLE the firmware runs the AES transmitter init
  ("AES(AK4101) init sequence start"), unmutes ("DIO: sound on state = on", soundOn=0->1, hSoundPermission=1)
  and starts polling the meter port (`dsp meter` shows live codes, SIG/PEAK LEDs follow). Verified with
  `tools/startup-test.sh 0|1|2`. Without a controller the emulator therefore boots from a flash copy with
  StartupMode 0 or 2 for audio tests (see tools/meter-cal.sh).
* **LED chain order**: the firmware clocks the sheet-9 map (0x0C2E56D0, 96 entries {chain A id, chain B id}) out
  in table order, entry 0 first, so entry 0 ends furthest down the chain. The PLD model stores the latched bits by
  table position (position p = (kLeds - 1 - clock index)). Ids: 0x00-0x1F +48V in 1-32, 0x20-0x3F SIG in 1-32,
  0x40-0x5F PEAK in 1-32 (meter object method +0x5C returns {sig, peak} in that order, used both by the LED
  phase 1 of FUN_0c1c5f84 and by `dsp meter`), 0x60-0x77 SIG out 1-16 / AES 1-8.
* **Meter word**: FUN_0c160b48 shows `((word >> 5) & 0x7F) - 1` and prints 0x7F ("over") when any of bits 15:12
  is set; the engine sets bit 12 for a clipped block.
* **Meter code calibration** (tools/meter-cal.sh, StartupMode 0 flash): the firmware's SIG flag (meter object
  method +0x5C, shown by `dsp meter` and on the LEDs) turns on at shown value 0x4A (code 75) and PEAK at 0x7B
  (code 124). The owner's manual specifies SIG at -40 dBFS or higher and PEAK at -3 dBFS or higher; both are
  matched exactly by dBFS = 0.75 x code - 96 (code 124 = -3.0 dBFS; code 75 = -39.75 dBFS is the first code at or
  above -40), i.e. the 7-bit code spans the 96 dB range of 16 bits in 0.75 dB steps and 0 dBFS is "over" (bit 12).
  The engine now publishes that mapping. METER page bars (5 px wide, 9 px pitch, baseline row 38, 23 rows):
  full at code 126, 0 rows at code 46 (~-60 dBFS); the top 13 rows (above about -18 dBFS, Yamaha's nominal) are
  drawn hatched.

## From the owner's manual (fw/d2/docs/rio3224d2_en_om_e0.pdf, text in .txt next to it)

* Front panel: [INPUT] 1-32 (XLR, -62..+10 dBu nominal range), per-input +48V / SIG (green, >= -40 dBFS) / PEAK
  (red, >= -3 dBFS) indicators ("if a serious error occurs all channels light or flash"; +48V indicators flash when
  the channel is on but the +48V MASTER switch is off), display, DEC/INC buttons, encoder with switch, power LED.
* Menu bar (left edge of the display): METER - GAIN - +48V ON - HPF ON - HPF F - GC ON - SETUP - INFORMATION - METER.
  Next-lower menu: press DEC and INC together (or hold DEC 1.5 s then press INC); next-higher: hold both 1.5 s (or
  hold INC 1.5 s then press DEC). DEC/INC select the port (long press jumps 8 ports); turning the encoder edits the
  selected value (GAIN: immediately, -6..+66 dB in 1 dB; +48V/HPF/HPF F/GC/SETUP: the field flashes until the
  encoder is pressed to confirm, moving the cursor discards). HPF frequency 20.0 Hz-600 Hz. The METER page shows
  input meters left, output meters right, with the selected port's +48V/HPF/GC icons on top; bars: >= -3 dBFS,
  > -18 dBFS, > -60 dBFS zones, PEAK HOLD always on (encoder press clears). Brightness: hold DEC+INC 1.5 s and turn.
  Panel lock: "FRONT PANEL LOCKED. TO UNLOCK - PRESS ALL BUTTONS FOR 3 SECONDS".
* SETUP items: UNIT ID (Y000-Y07F, default Y001), START UP MODE (REFRESH* / RESUME / REFRESH w/o MUTE - REFRESH
  initialises HA GAIN -6 dB, +48V off, HPF off 80.0 Hz, GC off, compensation gain -6 dB and "the input and output
  audio is muted until the HA parameter settings from a supported digital mixer is completed"; RESUME restores the
  state before power-off and does not mute; REFRESH w/o MUTE initialises without muting), SECONDARY PORT (DAISY
  CHAIN* / REDUNDANT), FAN SPEED (LOW* / HIGH), NETWORK MODE (AUTO* / STATIC IP) + IP/SUBNET/GW, DIMMER (10 s, 30 s,
  1, 2, 3*, 5 min), ERROR MESSAGES (LIGHT OFF* / KEEP MESSAGES), BRIGHTNESS (5-100 %, 70%*), INITIALIZE (FACTORY
  PRESET / HA PARAMETERS / DANTE SETTINGS, with confirmation). Device Lock from Dante Controller blocks UNIT ID and
  SECONDARY PORT. Status icons (right edge): ERROR / INFORMATION (upper), MUTE / DAISY CHAIN (middle), PANEL LOCK /
  HA READ ONLY (lower). HA Read Only is set when a supported mixer synchronises and stays until restart.
* Message codes (the firmware's `rep current` System/Sync Status lines use the same texts): SYSTEM: NORMAL,
  E00-03 DANTE MODULE ERROR!, E00-04 ILLEGAL MAC ADDRESS!, E00-06 UNIT FAN HAS MALFUNCTIONED!, E00-07 MEMORY ERROR!,
  E00-10 VERSION MISMATCH!, E00-11 DANTE AUDIO RESOURCE OVERFLOW!, E00-13 POWER SUPPLY FAILED!. SYNC: NORMAL,
  SYNCHRONIZATION (no id; "may take up to 45 seconds"), E01-03 WRONG WORD CLOCK!, E01-04 DANTE PORT DOES NOT HAVE
  CONNECTION!, E01-05 DANTE CONNECTION ERROR! ("other Dante devices cannot be found" - what the emulated unit shows
  while its module is alone on the virtual network), E01-06 DANTE IS NOT WORKING BY GIGA BIT!, E01-07 DANTE IS
  WORKING AT SECONDARY!, E01-08 ERROR OCCURRED AT SECONDARY PORT!, DANTE WORD CLOCK LEADER (info; the emulated unit
  is the leader, `err info` WORDCLOCK_LEADER = 1), E01-10/11/12 = the leader variants of E01-06/07/08. Alert texts:
  FIRMWARE VERSION MISMATCH, POWER SUPPLY "A"("B") FAILED, GIGABIT NETWORK LINK FAILED - SWITCHED TO 100Mbps,
  DANTE PRIMARY NETWORK LOST - SWITCHED TO SECONDARY PORT; warnings: DANTE AUDIO RESOURCE OVERFLOW, WRONG WORD CLOCK,
  DANTE PORTS HAVE NO NETWORK CONNECTION, DANTE CONNECTION ERROR, ERROR OCCURRED AT DANTE SECONDARY PORT, DANTE
  DEVICE LOCK ENABLED; information: FACTORY/HA MEMORY/DANTE INITIALIZE IN PROGRESS/COMPLETED, FRONT PANEL
  LOCKED/UNLOCKED.
* Specifications used by the engine: HPF 12 dB/oct, default off 80.0 Hz; Dante latency default 1.0 ms; sampling
  44.1/48 kHz (+-200 ppm, varispeed +4.1667 %, +0.1 %, -0.1 %, -4.0 %).
* **Verified in the emulator** (tools/menu-walk.sh, `--oled-at`): six short DEC+INC presses from the GAIN home page
  show +48V ON (all OFF), HPF ON (OFF), HPF F (80.0), GC ON, SETUP (UNIT ID Y001, START UP MODE, SECONDARY PORT
  DAISY CHAIN, FAN SPEED LOW, ...) and INFORMATION (SYSTEM / DANTE / LOG / REPORT); the status column shows the
  ERROR icon (E01-05, module alone on the network) and the DAISY CHAIN icon.
* **Start-up modes observed with saved gains** (flash store HA gain ch1 +20, ch2 +30, tools/menu-walk.sh with
  STARTUP=0/1/2): REFRESH (1) comes up with every channel at -6 dB (the HA parameters are initialised as the manual
  lists) and shows the INFORMATION and MUTE icons; RESUME (2) restores +20/+30 and is unmuted; REFRESH w/o MUTE (0)
  also showed the saved +20/+30 in V1.91 and is unmuted (the manual says it initialises like REFRESH).
* **Saving = the power-off sequence.** Editing a SETUP item on the panel (verified: SETUP -> INC twice -> START UP
  MODE, encoder +1 -> RESUME, push to confirm) changes the live parameter but writes nothing to flash. The firmware
  saves when the PSU signals power loss: /UVLO (PLD +4 bit 6, active low) -> event flag 8 / 0x40000 (the debug
  monitor's `misc pwrseq` sets the same flag: "execute poweroff sequence!!") -> "DIO: Power off return.", "DSP:
  Power off return.", DSP state -> DEAD, DIO unlock + mute, and a **new parameter-store record** is appended after
  the previous one (0x700000 record seq 1 -> seq 2 at 0x700BE8, same size 0xB3C, LastScreenType = 10 (SETUP),
  StartupMode = 2, two's-complement check verified) plus a new record of the small Setup2 store at 0x640000
  (OutputLoadImpedance, size 0x115, its check differs by one from the byte-sum rule). The newest record is restored at
  the next boot; `tools/flash-set.py` edits the newest record; `tools/flash-records.py` lists them.
* **PSU / panel inputs on the PLD**: +4 bit 4 = /IRQ_PLD, bit 5 = /UNLOCK, bit 6 = /UVLO (under-voltage = power
  going away); +0xA bit 0 = +48V MASTER switch (0 = ON), bit 5 = /POFF1, bit 6 = /POFF2 (PSU A/B "power off"
  flags, 0 = that supply failed -> E00-13 POWER SUPPLY FAILED), bits 4:1 DIP switches. `misc portr` prints them all.
* **PSU model verified** (tools/psu-test.sh, tools/setup-edit.sh POWEROFF=1): `--psu-fail A@30000-36000` makes the
  firmware print `ERR: POWER SUPPLY "A" FAILED`, raise **E00-12 POWER SUPPLY FAILED!** (V1.91 numbers it E00-12; the
  current manual lists E00-13), set System Status accordingly and clear it when the supply returns; `--48v-master`
  gives "48V Master: ON", lights the +48V LEDs of the channels whose +48V is on and sets the phantom bit in the
  head-amp frames (the engine then reports 48V on). `--power-off MS` drops /UVLO: the firmware runs the power-off
  sequence ("DIO/DSP: Power off return.") and appends a parameter-store record: the parameter-store write begins 31 ms
  after /UVLO (flash model log) - but 5.4 s after /UVLO when a SETUP item had just been edited on the panel (the UI
  finishes its edit/time-out first) - and while /UVLO stays low the sequence repeats every 1.17 s (13 records in 11 s).
  The emulator cuts the power `powerHoldMs` after the warning (`--power-off MS,HOLD`, default 8000 ms so that either
  case completes; a real supply holds far less) and stops.
* **Sync Status and the device list**: with the module alone (device list = this unit only) the firmware reports
  Sync Status 5 "DANTE CONNECTION ERROR!" (E01-05, other devices cannot be found) and the ERROR icon; as soon as the
  module's BROWSE_DIV_DEVICE_LIST notifications list a second device (the emulator feeds the virtual network's peers
  into the module model) the status becomes NORMAL and the icon clears (tools/dante-net-test.sh, RioB).

## Findings part 7: the console head-amp remote protocol (SCBP over DNT UART B)

* The host's HA remote stack is `HAP_CTRL::HapCtrl_SCBP` with 35 parameter classes (table at 0x0C2D3C80, 16 bytes
  per entry {0, vtable, name, state byte}): HapConvert, ParamSetVersion, ModelVersion, UnitId, ModelName, DeviceLabel,
  SystemMode, SystemStatus, SyncStatus, DeviceDiscovery, InputPortNum, OutputPortNum, P48vMasterSw, Brightness,
  PanelLock, AudioMute, ReadOnlyOption, OutputLoadImpedance, HaGain, P48vOn, HpfOn, HpfFreq, GcGain, GcOn, GainTrim,
  PortToPort, DeviceIdentify, InputPortIdentify, OutputPortIdentify, DeviceError, InputPreGcMeter, OutputPreGcMeter,
  InputPostGcMeter, OutputPostGcMeter. Per-class handlers sit at 0x0C178000.. (one every ~0x80 bytes); the RX error
  texts (HA_REMOTE_COMMAND_RX_BUFFER_FULL, "[HAP] HA_REMOTE_COMMAND_RX_ERROR = 0x%08x") belong to 0x0C17B0xx-0x0C17B5xx.
* Frames on the UART (both directions use the module's conmon_haremote serial format): `LEN | C0 'M' 'B' 'C' 01 00
  LL | src MAC[6] | dst MAC[6] | 00 00 LL | SCBP... | CS` with CS = ~(sum of C0..last payload byte) & 0xFF and LL =
  the SCBP length. The host's six boot-time notifies carry `10 07 op class u16 count u16 index value`: op 0x11 with
  class 0x05 = device label (32-byte string), op 0x22 with classes 0x08/0x09 (value 2), 0x10 (0), 0x14 (1), 0x15 (0).
* `--dntb-send MS:HEX` injects module-side bytes (a console frame). The host reads them (32 register reads of
  CS5 +0x814 for a 32-byte frame) but, with the module model never reporting an HA-remote connection, it neither
  answers nor changes a parameter and prints no [HAP] error - probing continues (tools/scbp-probe.py, scbp-probe2.py).
* Probe results so far (tools/scbp-probe2.py, frames `10 07 op class 00 01 00 00 [value]` to the unit): the HAP COM
  task receives every well-formed frame (LEN byte, C0 header, LL = SCBP length, checksum) silently; a frame whose
  LL does not match, one without the LEN prefix, and a 34-byte frame with op 0x21 and a 2-byte value make it print
  "EXECUTE MCBP RECV EVENT = -2147483648". The receive handler (0x0C17AAB8) requires frame byte 1 == 0xC0, selects
  the MCBP protocol object (selector 61) and calls its message iterator (vtable +0x20) and executor (+0x2C); the
  executor's op-code switch (0x0C173A88, on message byte 1) knows base codes 0x00 0x01 0x09 0x10 0x18 0x20-0x26 0x28
  0x2C 0x30-0x36 0x38 0x67 0x7E with |0x40 (response) and |0x80 (notify) variants and hands each to a per-op handler
  object (FUN_0c1741f6 checks the handler's min/max length at +4/+8); that path is a different protocol layer - the
  HA remote messages are handled by the MCBP executor described below (decoded 2026-10-03).
* SETUP cursor order (observed): UNIT ID, START UP MODE, SECONDARY PORT, FAN SPEED | NETWORK MODE, IP ADDRESS (4
  octet fields), SUBNET MASK (4), DEFAULT GW (4) | DIMMER, ERROR MESSAGES, BRIGHTNESS, INITIALIZE - 21 cursor
  positions over three pages (page dots at the bottom); the first DEC/INC press only shows the cursor.
* INITIALIZE: the encoder cycles FACTORY PRESET / HA PARAMETERS / DANTE SETTINGS; the switch opens the dialog "ARE YOU
  SURE YOU WANT TO INITIALIZE? - HA PARAMETERS -" with OK above CANCEL (CANCEL selected by default; DEC moves to OK,
  INC back to CANCEL; the switch executes the selection).
* **Decoded (by probing the emulated unit, tools/scbp-probe2.py + register probes):** the UART frame is
  `LEN | C0 'M' 'B' 'C' 01 | u16 region length | src MAC[6] | dst MAC[6] | 00 | region | CS` where CS = ~(sum of
  C0..last region byte) & 0xFF and the frame-level parser (0x0C1751EC) requires "MBC" at offset 2-4 and byte 20 == 0.
  The region is a sequence of messages `u16 length (including itself) | hdr | 07 | u16 command id | u16 count |
  u16 index | values` (iterator 0x0C175452 walks the lengths, at most region/10 messages; parser 0x0C175B52 fills
  ctx+20.. with length, hdr nibbles, command id, count, index, value pointer; executor 0x0C1754B8 dispatches on the
  command id, per-id value codecs at 0x0C1761xx copy the value bytes). hdr: 0x00 = request from the console, 0x10 =
  notify from the unit (its boot-time notifies and the arrays it sends after a change), 0x20/0x30 = acknowledgement
  of a request (echoes count/index/value), 0x40 = reply to a request the unit does not act on. The host rejects frames
  with a wrong region length, without the LEN byte, or with a message that does not fit ("EXECUTE MCBP RECV EVENT =
  -2147483648").
* **Command ids** (low byte = parameter class, high byte = group; index = 0-based channel for the 0x2216+ family,
  count = number of consecutive channels, values packed one after another): 0x1103 UnitId, 0x1105 DeviceLabel (32-byte
  string), 0x1107 SystemMode, 0x1108 SystemStatus (2 = NORMAL), 0x1109 SyncStatus (2 = SYNCHRONIZATION), 0x1F06
  DeviceDiscovery, 0x120A InputPortNum, 0x130A OutputPortNum, 0x2210 P48vMasterSw, 0x2212 Brightness (u8, 13 = 70 %),
  0x2213 PanelLock (u8), 0x2214 AudioMute (u8), 0x2215 ReadOnlyOption (u8), **0x2216 HaGain (int16, 0.01 dB)**,
  **0x2217 P48vOn (u8)**, **0x2218 HpfOn (u8)**, **0x2219 HpfFreq (u16, 0.1 Hz)**, **0x221A GcGain (int16, 0.01 dB)**,
  **0x221B GcOn (u8)**, 0x221C GainTrim, 0x221D PortToPort (compared by the firmware; not exercised). A console request
  `00 07 22 16 00 01 00 00 07 D0` sets input 1 to +20.00 dB: the firmware applies it to its parameter tree (`rep hgain`
  01:20, head-amp frame, engine), acknowledges with hdr 0x20 and then notifies the whole 32-channel array of every
  per-channel parameter from HaGain up to the one changed (count 32, index 0). Receiving console requests also turns
  the unit's "HA Read Only" state on, as the manual describes for a synchronised console. No "HA remote active"
  notification from the module is needed for the requests to be applied.
* Emulator support: `--dntb-send MS:HEX` (scripted frames), the virtual Dante network carries console frames to a
  unit (packet type 6) and the unit's UART B frames back (type 7); `tools/dante-console.py UNIT 'gain 1 20; 48v 2 on;
  hpf 3 on; hpffreq 3 120; gc 4 on; gcgain 4 10; lock on'` is the console stand-in (tools/console-test.sh).

## Findings part 8: the Dante network side (Controller-interoperable virtual endpoint, emu/src/net/)

**What this is and is not.** `--dante-ctl IFADDR` puts a network endpoint on a real interface so that Audinate's Dante
Controller (and other Dante devices) see the emulated Rio as a device. It is an independent implementation of the
publicly understood parts of the protocol family (the DNS-SD service model, the ARC request/response server, the
ConMon information channel, the flow-control exchange, the timestamped audio flows and PTPv1), written from the two
public reverse-engineering projects used as references only (`re/ref/inferno`, `re/ref/netaudio`, GPL, no code
copied) and from captures of Dante Virtual Soundcard on this PC (`tools/dante-pcap-*.py`). It implements no licensed
Audinate technology, is not certified, and is described as a *Controller-interoperable virtual Dante endpoint*,
nothing more. Layer A (the module model the Rio firmware talks to on DNT UART A, part 4) stays authoritative for what
the host programs - device label, channel labels, sample rate, preferred master, switch mode - and layer B (this
endpoint) is authoritative for what happens on the wire - link, clock role and lock, other devices, controller-made
renames and subscriptions; `AudioGlue::bridgeTick` carries changes both ways (a controller rename reaches the firmware
as NOTIFY BROWSE_DEVICE_LABEL + BROWSE_RESTART, a sample-rate change as NOTIFY DEVICE_FS and a re-rated DIO word
clock, clock role changes as DEVICE_CLOCK_STATUS, the device list as BROWSE_DIV_DEVICE_LIST, identify as
BROWSE_IDENTIFY).

**Discovery (mDNS/DNS-SD, 224.0.0.251:5353).** Instance `<name>` under `_netaudio-arc._udp` (SRV -> ARC port; TXT
`arcp_vers=2.7.41 arcp_min=0.2.4 router_vers=4.0.2 router_info=<board> mf=<manufacturer> model=<model id>`) and
`_netaudio-cmc._udp` (SRV -> CMC port; TXT `id=<16 hex device id> process=<n> cmcp_vers=1.2.0 cmcp_min=1.0.0
server_vers=4.0.2 channels=0x6000004d mf= model=`), one `<chan>@<name>` instance per TX channel under
`_netaudio-chan._udp` (SRV -> flow-control port; TXT `txtvers=2 dbcp1=0x1102 dbcp=0x1004 id=<n> rate=<fs> pcm=<bytes>
e enc=<bits> en=<bits> latency_ns=<min receiver latency> fpp=<max>,<min> nchan=<channels per flow>` plus `default` on
the factory-named instance), `<name>.local` A record. Names longer than 31 characters are ignored by Controller. The
device id is the EUI-64 of the module MAC (`00 1d c1 ff fe xx xx xx`). A device's address is the source address of
its own SRV answer, not the A record of the host name: on this PC `nano.local` is both Dante Virtual Soundcard's
device name and the Windows host name, and the Windows stack answers for it too.

**ARC (UDP 4440, 10-byte header: start code, total length, sequence, opcode, status; status 0 = request, 1 = ok,
0x8112 = more pages, 0x22 = error).** Dante Controller 4.18 asks a new device, in this order: 0x1000 counts and
limits, 0x1102 property directory (94 zero bytes accepted), 0x1002 name, 0x2000 TX channel pages (request `00 01 |
first channel | 00 00`, 16 per page, each item `{id, 7, common descriptor offset, name offset}`; the 16-byte common
descriptor is `fs u32, 01 01, bits, 0x0400, bits, bits, encoding capability bits 0x0e = 16/24/32`), 0x3000 RX pages
(items `{id, 6, common, tx channel name, tx host, friendly name, status u32, 0}`; status 0 none, 1 unresolved, 8 in
progress, 0x14 transmitter busy, 0x15 transmitter error, 0x01010009 receiving unicast), 0x2010 TX friendly names,
0x2200 TX flows, 0x2204 TX flow labels, 0x3200 RX flows, 0x1100 device settings (110 zero bytes accepted), 0x3300
receiver port ranges (`38 00 38 fd 38 fe 38 ff`, without it Controller reports a clock-domain mismatch), 0x2320
(answered with status 0x30), and later 0x1003 names. Controller writes 0x3010 subscriptions (`00 count | {rx id, tx
channel name offset, tx host offset}...` + strings; zero offsets clear), 0x3014 unsubscribe, 0x2013 / 0x3001
channel renames, 0x1001 device rename, 0x1101 device-settings write (the receive-latency form: `00 00`, a fixed
22-byte preamble, latency ns twice). Start codes seen: 0x2729 and 0x280f (the latter also carries 0x4100, not
understood and left unanswered). Dante Virtual Soundcard's 0x1003 reply places its strings differently from the
layout used here (its friendly/factory fields read as `Win32_x86_64` / a build date), so that layout is only partly
understood; Controller shows our name, model, versions and address correctly from it and from ConMon.

**ConMon (UDP 8700; 32-byte header: start code, length, sequence, process, device id[8], vendor[8], opcode[8]).**
The vendor field must read `Audinate` or Controller ignores the device entirely (it is the message family's owner,
not the product's maker). Requests arrive with opcode `07 xx 00 61` (board info -> `07 2a 00 60`, 200 bytes: fw
version major.minor.build(u16).patch at 0 and 0x23, hw version at 4, boot at 0x28, feature bits at 0x14-0x17, board
name at 12 and 0x38, 0x1f at 0xbb), `00 c1` (product info -> `00 c0`, 336 bytes: manufacturer at 0 and 0x2c, board
name at 8, model name at 0xac, version words at 0x18/0x1c), `00 21` (clock status -> `00 20`), `00 13` (network ->
`00 11`: link speed, MAC, IP, mask, gateway) and `00 77` (-> `00 78`). Replies go to 224.0.0.231:8702, the 1 Hz
heartbeat to 224.0.0.233:8708 (blocks 0x8001 frequency offset ppb, 0x8002 TX/RX peak bytes (0 = full scale, 0.25 dB
per step), 0x8003 receive-flow latencies). The same port carries the `0xFFFF` settings family (`00 00 | requester
MAC | 00 00 | Audinate | 07 | family 0x3a system, 0x27 audio config, 0x3e clear | kind u16 | 100 u32 | body`):
kind 0x0081 sample rate (body `mode u32 0 probe / 1 set, value u32`, reply `07 2a 00 80`: `00 18 00 04 | current |
pending | 00 02 00 00 | 44100 48000 88200 96000`), 0x0083 encoding (reply `00 82`, 24/16/32), 0x0063 identify,
0x0085 pull-up, 0x0090 reboot / factory reset (refused). Dante Controller's Device Config tab drives the unit's
sample rate through 0x0081; the endpoint applies it, the module model notifies DEVICE_FS and the firmware's DIO
re-rates (`currentFsCode 3 -> 2` for 44.1 kHz in the firmware log).

**Flows.** A receiver resolves `<chan>@<host>._netaudio-chan._udp`, then sends 0x0100 to the transmitter's flow-control
port with the start code from the TXT `dbcp1` (Virtual Soundcard uses 0x1200): `strings offset u16, fs u32, bits u32,
1, channel count, remote descriptor offset, channel ids..., 0x1c+2n, 0x0a00, 2, frames per packet, flow name offset,
12 zero bytes, strings (host name, flow name), pad to 8, 08 02 | rx port | rx ip`. The reply carries a 6-byte flow
handle; 0x0102 updates the channel list, 0x0101 stops. Virtual Soundcard refuses frames-per-packet values outside its
TXT `fpp=48,48` (status 0x0314), rejects other sample rates with 0x0301, allocates 4-slot flows, and appends 16 extra
bytes before its strings that the receiving side does not need to understand. Audio packets: `02 | seconds u32 |
sample-in-second u32 | frames x channels x 24-bit big-endian PCM`; the receiver answers every 250 ms with the 9-byte text
`KEEPALIVE` to the packet's source port (captured from Dante Virtual Soundcard; the `13 37` code the public references use
is not accepted by it - it stopped our flows after 4 s until we sent the text); a transmitter drops a flow that stays
silent for 8 s. A receiver plays
at the larger of its own latency and the transmitter's TXT `latency_ns` (Virtual Soundcard asks for 6 ms). Receive
ports start at 14336 (15000 when Audinate software owns that range on the same host).

**PTPv1 (UDP 319/320, 224.0.1.129, sub-domain `_DFLT`).** Audinate's own implementation (Virtual Soundcard, decoded
with `tools/dante-pcap-ptp.py`) sends Sync 4 times a second with header flags 0x000c (assist + boundary clock),
grandmaster stratum 233, identifier `DFLT`, variance 28768, grandmaster port 0, and a Follow_Up with the precise
origin time; its time base is seconds since its own start. A PTPv1 receiver qualifies a master only after two Syncs
within four of its own Sync intervals, so a 1 Hz master is invisible to Dante devices - the endpoint uses the same
parameters. Between emulator instances the clock works as expected (one grandmaster, the others lock below 1 ms,
Delay_Req/Delay_Resp every 1-2.5 s). **Open:** Virtual Soundcard on the same PC never follows our clock nor announces
ours as better, even at stratum 232, and keeps its own time base, so its audio arrives with timestamps 1.79e9 s away
from ours ("other clock domain" in the diagnostics, not played) and Controller lists the leader as "Unknown Device".
The likely cause is that it discards PTP (and attributes clock identities) by source IP address, which we share with
it on a single host; the test that would settle it is a second IP address on the LAN adapter for the emulator
(`New-NetIPAddress`, needs administrator rights) or a second machine. Everything else with Virtual Soundcard works: it
requests our TX flows and keeps them alive with keepalives, it serves our RX flow requests, Controller shows both.

**Host-level behaviour found on the way.** Windows delivers unicast UDP to the most specific binding, so a device
bound to the host's own address on 4440/4455/8700/8800 steals traffic from Dante Controller's ConMon service and from
Virtual Soundcard; when Audinate software is detected on the host the endpoint uses 4450/8810/8710/4465 and DNS-SD /
CMC carry the ports. The LAN address lives on a Hyper-V virtual switch here; when another virtual NIC appeared the
stack silently dropped every existing multicast membership of running processes (unicast kept working) - the endpoint
re-joins its groups when mDNS and PTP both fall silent for 10 s. The audio engine is clocked by the PTP media clock on
its own real-time thread (like the unit's DSP from the Dante word clock) instead of by emulated time: with the
emulation paced by the CPU emulator, 20-70 ms of timing jitter on a loaded host produced late packets and dropouts;
clocked by the media clock two instances exchanged 165 000 packets each way in 120 s with no re-anchor, at most one
late packet and no dropout after the flows had started (`/dante.json`: `late`, `maxLateMs`, `minMarginMs`,
`dropouts`, `resyncs`).

**Acceptance against Dante Controller 4.18.1 on this PC (host NANO, 192.168.0.118, with Dante Virtual Soundcard
running).** Appears automatically in the Network View and the Device Info tab with device name, model name
"Rio3224-D2", Dante version, primary address and link speed (screenshots `out/DanteController-*.png`); Device View
opens (Receive / Transmit / Status / Latency / Device Config / Network Config pages load without error);
subscriptions made in Controller reach the endpoint (0x3010), its receive side resolves and requests the flows, the
transmit side serves Controller-made subscriptions from Virtual Soundcard and from a second emulator instance with
real PCM (meters on the receiving unit reproduce the transmitting unit's tones to the dB); Controller's sample-rate
setting changes the unit's rate; renames from Controller reach the firmware. Not met on this single host: a shared
PTP clock domain with Virtual Soundcard (above), hence no audible audio with it; the "Primary Leader Clock" column
names our device as unknown. Not implemented: multicast TX flows (the counts message says so), AES67, device lock,
redundant-secondary audio (the secondary interface only reports link state and the switch mode; one NIC here).
Diagnostics: `/dante` on the panel server (identity, ports, clock, discovery, controller requests, other devices,
subscriptions with status and dropout counters, TX/RX flows, counters, event log) and `trace=FILE.pcap` (every
datagram, Wireshark-readable raw IPv4).
