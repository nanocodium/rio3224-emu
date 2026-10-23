# rio3224-emu — Yamaha Rio3224-D2 full-system emulator

Runs the **unmodified** Rio3224-D2 host firmware (`RIO2_PROG.BIN` V1.91 from the R Remote update
package) on an emulated Renesas SH-2A board: CPU, HI7000/4 RTOS timers and interrupts, NOR flash with the
parameter store, board PLD, the four head-amp FPGAs' identification bus, the main FPGA (clock manager +
OLED), the DSP interface, the front-panel LED chains and the head-amp UART link.

```
fw/d2/        extracted firmware: RIO2_PROG.BIN, Dante module images (bk2/, bk3/ with kernel, rootfs ...)
re/           reverse-engineering artefacts: Ghidra decompilation (RIO2_PROG.decomp.c), scripts
docs/         HARDWARE.md (everything recovered about the hardware), Renesas manuals as text
emu/          the emulator (C++20, CMake)
tools/        extraction, analysis and test-run scripts (run under WSL)
```

This repository contains the emulator, the reverse-engineering notes and the tools. It does **not**
redistribute copyrighted material: the extracted firmware and Dante module images (`fw/`, which you
need to extract from Yamaha's own update package as described in `docs/HARDWARE.md`), the vendor
manual PDFs in `docs/`, and the third-party reference checkouts under `re/ref/` are excluded by
`.gitignore`.

## Build (WSL2 / Linux, or Windows with MSVC)

```bash
mkdir -p ~/rio3224/build && cd ~/rio3224/build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo /mnt/d/rio3224-emu/emu && ninja
```

Native Windows build (Visual Studio 2022 Build Tools + CMake + Ninja on the PATH): run `toolsuild-msvc.cmd`, which
configures `build-msvc` with cl.exe and builds `build-msvc
io-emu.exe`. The same sources build with MSVC and GCC
(`emu/src/platform.h` wraps the few OS calls: the TCP bridges for the module UARTs, the interactive console, TAP on Linux);
on Windows the WASAPI host-audio backend is compiled in (`--audio-dev`). Run it from `emu` so the default firmware
and flash paths resolve, e.g. `..uild-msvc
io-emu.exe --max 3000000000 --no-raw --flash %TEMP%lash.bin --panel`.

## Run

```bash
cd /mnt/d/rio3224-emu/emu
~/rio3224/build/rio-emu --oled oled.pgm --panel
```

The debug monitor appears on the terminal (SCIF0, `top>` prompt; type `?`; modules `ha dio dsp net err
mms misc rep itron perf`). Ctrl-] quits and saves the flash image (`fw/d2/flash.bin`, 16 MB, holds the
formatted parameter store and the board-info records). A behavioural Dante module (dac's BCA protocol on
the FPGA UART, GPI heartbeats, word clock) is attached by default. Useful options:

| option | effect |
|---|---|
| `--max N` | stop after N instructions |
| `--log DEV` | register-level log: `PLD PLDS INTC MTU2 ADC flash SCIF0..3 CS4 CS5 DSPIF OLED HA DNTUART DANTE` or any on-chip block name |
| `--watch ADDR,LEN` | report accesses to a physical address range (with PC); `--watch-max N` reports per address/size/direction (default 4) |
| `--probe PC[:rN,LEN]` | dump registers (and memory at rN) whenever PC is reached (`--probe 0c241e18:r4,48` lists the OLED dialogs) |
| `--hook PC[@PR]:rN=VAL` | force a register value at PC |
| `--dante-boot-ms N` | when the Dante module model comes up (default 12000) |
| `--dante-resp-ms A[,B]` | module response latency, immediate / deferred commands (3 / 30 ms) |
| `--dante-browse c,f,g` | BROWSE_COMPLETE payload bytes (default 1,7,0 = monitoring completed) |
| `--dante-swmode N` | switch mode stored in the module (0 = switched, the host's rear DIP default; a different value makes the host rewrite it and restart the module) |
| `--dante-fresh` | factory-fresh module: default device name `Rio3224-D2-<mac>`; the host renames it (`[DEV LABEL] ... LABEL:RESET`) |
| `--no-dante-model`, `--dnt-uart A,B` | no module / bridge the two module UARTs to TCP ports (e.g. a Brooklyn III in QEMU) |
| `--dante-clock-ms T1[,T2,…]`, `--dante-fs HZ` | scripted word clock without the model |
| `--mac XX:XX:XX:XX:XX:XX` | host MAC written into the board-info flash record when the sector is blank |
| `--fan-hz F` | fan running when > 0 (0 raises E00-05 UNIT FAN HAS MALFUNCTIONED) |
| `--press SW@T0[-T1]` | hold front-panel switch SW (1,3,4,5) from T0 to T1 ms |
| `--pld-in MASK`, `--pld-in10 VAL` | PLD +0xA input mask (DIP switches) and +0x10 inputs |
| `--aes 0|1|2` | AES/EBU transmitter fitted: none / AK4101 (default) / CS8406 |
| `--pcap FILE`, `--tap NAME` | dump the DM9000's Ethernet frames to a pcap file / bridge them to a Linux TAP device |
| `--oled FILE` / `--oled-ascii` | dump / print both 296x48 OLED panels at exit (600x48 PGM, `tools/pgm2png.py` converts) |
| `--oled-full FILE` | dump both SSD1322 controllers' whole RAM (480x256 PGM) |
| `--oled-at MS=FILE` | capture both panels at emulated time MS (repeatable; `tools/menu-walk.sh` uses it) |
| `--idle-pc ADDR` / `--no-idle-skip` | idle-task loop fast forward (on by default at 0C35FE14; ~4x faster boots) |
| `--panel` | print the front-panel LEDs by function (+48V / SIG / PEAK per input, output SIG, Dante/sync status) at exit |
| `--disasm ADDR[,N]`, `--dump ADDR,LEN`, `--trace…`, `--pc-hist`, `--debug-exc` | debugging aids |

Typical boot (the home screen is up after about 25 s of emulated time; a 60 s run of 11e9 instructions
takes ~17 s wall time at 650-690 Minsn/s): the debug monitor banner with the MAC address, the DIO clock lock
sequence, `DSP: state DEAD -> ALIVE`, `[DANTE] AUDIO_NETWORK_DEVICE_DETECTED!!`, the Dante start-up exchange
(`[DEV LABEL] UNIT:Y001-, LABEL:Y001-24-D2-123456`) and the home screen (head-amp gains) on the two OLED
panels (left: channels 1-16, right: 17-32). The module model boots as a *configured* unit (device label,
switch mode and manufacturer record as the Rio host programs them), so the host does not rename or restart
it; `--dante-fresh` gives the factory-fresh path (rename), `--dante-swmode 1` the "switch mode changed"
path (SET NETWORK_SWMODE, SET BASE_RESTART, module away, `E00-03 DANTE MODULE ERROR` until it is back,
dismiss with `--press 5@T`). `tools/long-run.sh` and `tools/press-run.sh NAME MAXINSNS [args]` script
full runs (`FLASH=/tmp/x-flash.bin` reuses the flash image of an earlier run).

## Status

Working: SH-2A core (incl. FPU, register banks, MOVI20, bit ops), INTC/CMT/MTU2/SCIF/ADC, flash + FMD
parameter store + board-info records, PLD (DIP/front switches, fan alarm, Dante GPIs, IRQ routing), FPGA ID
bus, CS5 main FPGA (clock lock, counters, two module UARTs with 16-byte FIFOs, OLED FIFO -> SSD1322 model),
CS4 DSP interface (register transport, meter port, serial FIFO capture), LED chains, HA UART frames, DIO
lock sequence, DSP alive, and a behavioural Dante module model that takes the host through its whole BCA
start-up (device info, switch registers, labels, patches, clock status and device list by notification,
switch-mode notify, manufacturer record, requested module restart). The boot is clean: no host error prints
(`BCA_CALLBACK UNSUPPORTED`, `EXECUTE DANTE UPDATE EVENT`, `RESULT_RETRY` and the E00-03 module restart
are all gone), the UI reaches the home screen on both OLED panels, and popups render completely (text on
the left panel, OK button on the right).

Also working: the DM9000 Ethernet controller (the host runs DHCP and falls back to a link-local address;
bridge it with `--tap` to reach it with R Remote), the AES/EBU transmitter detection (AK4101 or CS8406 on
the PLD's bit-banged serial port), the fan alarm, the board-info flash records (MAC), the front-panel switches
(`--press`; switch 5 closes OLED popups), the unit-ID rotary (ADC).

Speed: 650-690 Minsn/s on a modern PC (a 60 s boot run of 11e9 instructions takes ~17 s wall time; it was
150 s before the speed work): the RTOS idle loop is fast-forwarded, interrupts are only re-evaluated when a
line or the mask changes, and both instruction fetches and data accesses to SDRAM bypass the bus
(`tools/perf-run.sh` profiles a boot; what is left is the instruction decoder itself).

Since then: audio (below), the console HA-remote protocol on the module's second UART, and the Dante network side
(`--dante-ctl`, below). Not yet: R Remote and console integration over a real Dante network. One harmless firmware quirk remains visible as `[bus] unmapped read
4C45445F`: a diagnostics registry looks a name up with the first four bytes of the string "LED_..." as the
pointer (`strnlen` on an unmapped address, bounded to 24 bytes). The real Brooklyn III Linux image boots in QEMU from
`fw/d2/bk3` but its `dac` cannot run without the licensed Dante PL, so the model stands in for it (or bridge
a real module with `--dnt-uart`). See `docs/HARDWARE.md` for the recovered register maps and protocol notes.

## Audio engine (PCM)

The emulated board drives a real signal chain (`emu/src/audio/`): 32 input sockets -> analog head amp (gain
index from the firmware's SCIF1 frames, 1 dB steps, clip at the modelled rail) -> 24-bit converter (full scale
+25.5 dBu at the head-amp output so that the documented +30 dBu maximum input @ -6 dB lands on 0 dBFS after the
firmware's +1.5 dB DSP word) -> the DSP words and biquads exactly as the firmware wrote them (gain words, filter A,
user HPF with the firmware's 2nd-order coefficients and dry/wet pair) -> gain compensation (start gain - current
gain, from the firmware's parameter tree) -> Dante TX 1-32. Dante RX 1-16 -> DAC (0 dBFS = +24 dBu) -> analog
outputs, RX 17-24 -> AES/EBU 1-8 (24-bit PCM). Audio runs in lockstep with emulated time (one block per
emulated millisecond), paced to the wall clock with `--realtime`, or - with the Dante endpoint - on its own real-time
thread clocked by the PTP media clock; it is muted until the firmware's DIO locks.

The unit's saved **start-up mode** decides whether the firmware ever unmutes without a console: the factory flash has
REFRESH, which keeps the DIO muted (and the meters dark) until a console or R Remote refreshes the head-amp settings.
For stand-alone audio tests boot from a flash copy saved with RESUME (`startup=2`) or REFRESH w/o MUTE (`startup=0`):
`python3 tools/flash-set.py fw/d2/flash.bin /tmp/my-flash.bin startup=2` (the tool edits the firmware's own parameter
store like a save: `unitid=N`, `gain=CH:dB`, `48v=CH:1`, `hpfon=CH:1`, `hpffreq=CH:Hz`, `gcon=CH:1`,
`gcstart=CH:dB`, `all:gain=20`), then `--flash /tmp/my-flash.bin`. The meters then appear on the METER page, the
SIG/PEAK LEDs and `dsp meter` with the calibrated code mapping (dBFS = 0.75 x code - 96; SIG at -40 dBFS, PEAK at
-3 dBFS as in the owner's manual).

| option | effect |
|---|---|
| `--tone CH:sine\|sweep\|impulse\|noise\|square:FREQ:dBu[:FREQ2:SEC]` | test signal on socket CH (1-32 or `all`), level in dBu at the XLR |
| `--audio-in FILE.wav[,loop]` | host input channels from a WAV file (channel n -> socket n, or `--audio-map "rio:host,..."`) |
| `--audio-out FILE.wav`, `--aes-out FILE.wav` | record the 16 analog outputs (float) / the 8 AES outputs (24-bit) |
| `--tx-out FILE.wav` | record all 32 Dante TX channels (a virtual Dante receiver) |
| `--dante-net [GROUP:PORT[@IFADDR]]`, `--dante-net2 [...]` | UDP virtual Dante network between emulator instances / tools (primary, optional secondary = redundant mode); `--dante-name NAME` sets the device name, `--dante-clock leader|follower`, `--dante-fail primary|secondary@MS[-MS2]` pulls a cable |
| `--dante-patch "rx@tx,..."` or `"rx@Device:tx,..."` | subscribe RX channels to this unit's own TX channels (loopback) or to another device on the virtual network |
| `--dante-latency-ms X`, `--dante-drop PCT` | receive latency (default 1 ms) and block-loss simulation |
| `--dante-ctl IFADDR[;KEY=VALUE...]` | **Controller-interoperable Dante endpoint on the real network** (see below): the unit appears in Audinate's Dante Controller on that interface. Keys: `sec=IFADDR2`, `trace=FILE.pcap` (every datagram, for Wireshark), `persist=FILE` (names, format, subscriptions survive a restart; default dante-endpoint.cfg), `mac=xx:xx:..` (module MAC = identity, for a second instance), `name=`, `latency=MS` (receive latency, default 5), `ahead=MS` (timestamp lead, default 1), `prefer=1` (preferred master), `stratum=N`, `sync=LOG2` (PTP Sync interval), `ports=ARC,CMC,INFO,FLOWS`, `verbose=1`; implies `--realtime` |
| `--host-in-dbu X`, `--host-out-dbu X` | calibration: host full scale at the sockets / at the outputs (default +24 dBu) |
| `--trace-input N [--trace-ms M]`, `--meter-print M` | signal inspector line for socket N / engine meters every M ms |
| `--realtime` | pace the emulation to wall-clock time |
| `--run-ms MS` | stop after MS of emulated time |
| `--panel-http PORT` | live front/rear panel page at http://127.0.0.1:PORT/ (OLED, LEDs, meters; DEC/INC/encoder, +48V MASTER, PSU failure, power-off) - use with `--realtime` |
| `--dntb-send MS:HEX` | inject raw bytes on the module side of DNT UART B at MS, i.e. a console HA-remote frame arriving at the unit (`tools/scbp-probe.py`) |
| `--console-script FILE`, `--rotate STEPS@T[,x,MS]` | type into the firmware's debug monitor at given times; turn the front-panel encoder |
| `--audio-list`, `--audio-dev in=SPEC[;SPEC2],out=SPEC[,latency=MS]` | Windows build: list WASAPI devices; use real devices (SPEC = `default`, `loopback[:NAME]` or part of a device name); implies `--realtime` |
| `--power-off MS`, `--psu-fail A|B@MS[-MS2]`, `--48v-master [on|off@MS]` | PSU / rear-panel model: /UVLO (firmware saves, the unit loses power HOLD ms later, default 8 s), PSU A/B failure, +48V MASTER switch |

Tests: `tools/audio-test-tones.sh` (32 unique tones -> 32 TX channels, exact frequencies and levels),
`tools/audio-test-hpf.sh` (HPF switched through the firmware's parameter tree; measured attenuation equals the
programmed biquad to 0.01 dB), `tools/startup-test.sh 0|1|2` (DIO mute behaviour per start-up mode), `tools/audio-test-gc.sh` (gain compensation
acceptance: constant Dante level across an HA step, clipping not undone),
`tools/meter-cal.sh NAME dBu...` (firmware SIG/PEAK flags, LEDs and METER page against known socket levels),
`tools/menu-walk.sh NAME N` (front-panel menu navigation per the owner's manual, OLED capture per step), `tools/setup-edit.sh`
(change START UP MODE from the panel, then `misc pwrseq` or `POWEROFF=1` -> the firmware saves a parameter-store record),
`tools/psu-test.sh` (PSU A failure / recovery, +48V MASTER), `tools/dante-net-test.sh` (two instances on the virtual Dante network:
RioB receives RioA's tones at exactly -50.0 dBFS; with `REDUNDANT=1` the primary cable is pulled for 4 s and the audio
continues through the secondary network without a gap; the firmware's Sync Status becomes NORMAL once another device is
listed), `tools/long-run-audio.sh` (10-minute stability run: 32 tones through TX and back on RX 1-24; levels stayed at -50.0 dBFS to
the last second, no block loss, no firmware errors, 600 s emulated in 260 s), `tools/factory-init.sh` (SETUP -> INITIALIZE ->
HA PARAMETERS from the panel: confirmation dialog, "HA MEMORY INITIALIZE COMPLETED", all head amps back to -6 dB),
`tools/console-test.sh` (console stand-in over the virtual Dante network sets HA gain, +48V, HPF, GC and panel lock; the
firmware applies them - `rep` reports, head-amp frames, engine trace - and notifies the 32-channel arrays back). Live operation: `--realtime --panel-http 8321`, then open
http://127.0.0.1:8321/ for the front/rear panel page. See
`docs/AUDIT.md` for the subsystem-by-subsystem state.

### Virtual Dante network

`--dante-net` replaces the in-process loopback with a UDP multicast network between processes: every device announces
its name and channel counts once a second and multicasts its 32 TX channels as one 1 ms block per packet (24-bit PCM
with the sender's sample time and a sequence number); receivers keep per-device subscriptions (RX <- device@tx), a
fixed receive latency (`--dante-latency-ms`), count lost/late/duplicate blocks and, with `--dante-net2`, run a second
network in parallel: each block is sent on both and the first copy wins, so pulling one cable (`--dante-fail`) does
not interrupt the audio. Devices on it: emulator instances (`--dante-name`), `tools/dante-player.py` (a transmitting
device: WAV file or tones, i.e. a console output or another rack) and `tools/dante-ctl.py` (list devices, change a
device's subscriptions at run time, like a controller) and `tools/dante-console.py` (a console / R Remote stand-in: `dante-console.py RioA 'gain 1 20; 48v 2 on; hpf 3 on;
hpffreq 3 120; gc 4 on; gcgain 4 10; lock on'` sends the unit's own head-amp remote messages as packet type 6, which the
emulator hands to the firmware on DNT UART B like the Dante module does; the firmware applies them and its replies and
32-channel notifies come back as type 7 and are printed decoded; `tools/console-test.sh` is the end-to-end test). Two emulations run on their own clocks, so a receiver learns
the sender's time offset from the first block and re-synchronises (counted, audible as a short dropout) if a stream
drifts out of the latency window; use `--realtime` on all instances. This is the emulator's own protocol: it is not
Audinate's Dante, implements none of its licensed technology and does not interoperate with real Dante devices.

### Controller-interoperable Dante endpoint (real network)

`--dante-ctl 192.168.0.118` (the address of the interface Dante Controller is on) makes the emulated unit a device on
the real network: DNS-SD/mDNS services, the ARC request/response server, the CMC advertisement, the ConMon
information channel (board/product/clock/network status, heartbeat with meters, the settings family), the
flow-control exchange, timestamped 24-bit audio flows with keepalives, and a PTPv1 clock (master or slave, with the
parameters Audinate's own implementation uses). The identity is the module model's: the device label the Rio host
programmed, model Rio3224-D2, manufacturer Yamaha Corporation, the module MAC as EUI-64 device id, the firmware's
version words. It is an independent implementation of the publicly understood protocol behaviour (references
`re/ref/inferno`, `re/ref/netaudio`, captures of Dante Virtual Soundcard), not Audinate's code, not certified and not
"Dante" in the licensing sense; `docs/HARDWARE.md` part 8 lists what is understood, what is accepted without being
understood (ARC 0x1100/0x1003 details, 0x4100) and what is not implemented (multicast TX flows, AES67, device lock,
redundant secondary audio).

Verified on this PC with Dante Controller 4.18.1 and Dante Virtual Soundcard: the unit appears automatically with
name, model, Dante version, address and link speed; the Device View pages open; subscriptions, renames, the sample
rate (44.1/48/88.2/96 kHz, the firmware's DIO re-rates) and the receive latency set in Controller reach the unit and
the firmware; Virtual Soundcard requests and receives our transmit flows and serves ours; two emulator instances
(`mac=` gives the second one its own identity) exchange real PCM through Controller-made subscriptions with the
receiving unit's meters reproducing the transmitting unit's tones to the dB, no re-anchor and at most one late
packet in 120 s. Not achieved on a single host: a shared PTP clock domain with Virtual Soundcard (it keeps its own
grandmaster, most likely because our packets come from its own IP address), so its audio is counted as "other clock
domain" and not played; run the emulator from its own IP address or another machine to test that part. The audio
engine runs on a real-time thread clocked by the PTP media clock, independent of how fast the CPU emulation runs.

On a host that also runs Dante Controller or Virtual Soundcard the standard ports (4440/4455/8700/8800) belong to
Audinate's processes; the endpoint then uses 4450/8810/8710/4465 and receive ports from 15000 (DNS-SD and the CMC
advertisement carry them). Diagnostics: `--panel-http 8321` then http://127.0.0.1:8321/dante (identity, ports, clock
state and master, discovery and controller requests, other devices, subscriptions with status and dropout counters,
TX/RX flows with packet, late and keepalive counters, event log; `/dante.json`, `/dante.log`). Tools:
`tools/dante-arc.py HOST:PORT info|tx|rx|flows|sub RX CHAN HOST|unsub RX|rename-tx N NAME|rename-rx N NAME` (the
controller side of ARC), `tools/dante-settings.py HOST:INFOPORT rate [HZ]|enc [BITS]|identify|board|product|clock|net`
(the controller side of the ConMon settings family), `tools/dante-pcap-summary.py / -flows.py / -ptp.py /
-ptp-timeline.py FILE.pcap` (decode a trace), `tools/ptp-listen.py IFADDR` (who is sending PTP), `tools/mdns-sniff.py`,
`tools/win-capture.ps1` (screenshots of the Dante Controller windows), `tools/rt-check.ps1` (real-time ratio).

```
cd build-msvc
rio-emu.exe --no-raw --no-unmapped --flash ..\out\ctl-flash.bin --panel-http 8321 --tone 1:sine:1000:-20 ^
   --dante-ctl "192.168.0.118;trace=..\out\dante-ctl.pcap;persist=..\out\dante-endpoint.cfg"
rio-emu.exe --no-raw --no-unmapped --flash ..\out\ctl-flash-b.bin --panel-http 8322 --tone 1:sine:440:-20 ^
   --dante-ctl "192.168.0.118;mac=00:1d:c1:ab:cd:ef;persist=..\out\dante-endpoint-b.cfg"
```

### Live test with real interfaces (Windows)

```
cd emu
..\build-msvc\rio-emu.exe --audio-list
..\build-msvc\rio-emu.exe --flash ..\out\flash-live.bin --dante-boot-ms 15000 --panel ^
   --audio-dev "in=TonePort;MIXLINE Stream,out=Speakers (MIXLINE)" --audio-map "1:1,2:3,3:4" --dante-patch "1@1,2@2" ^
   --meter-print 5000 --tx-out tx.wav
```

`flash-live.bin` is the factory flash saved with START UP MODE = RESUME (so the unit unmutes without a console) and
HA gains +20/+10/+10 dB on inputs 1-3 (`tools/flash-set.py`). Observed (2026-10-02): the interface's left input lands on
Rio INPUT 1 and the stream mixer's L/R on INPUT 2/3 with the saved head-amp gains, the firmware releases the DIO mute
at lock ("DIO: sound on state = on"), its own `dsp meter` codes and the SIG/PEAK LEDs follow the live levels, the Dante
TX capture carries the audio, and both capture devices stay locked to the Rio clock by the drift servo (about -0.03 %,
no underruns or drops over 42 s of real time). The outputs patched back from the inputs (`--dante-patch`) play on the
selected Windows playback device.

## Analysis tools (tools/)

Static: `str-refs.py` (string -> literal-pool slots -> functions), `lit-refs.py ADDR` (which slots hold a
value and who uses them), `fn-grep.py PATTERN FUNC...` (grep inside decompiled functions), `slot-ctx.sh`
(disassembly around the instruction that loads a slot), `dac-fn.sh / dac-ctx.sh / dac-fns-matching.sh /
dac-notify-sites.sh` (the Dante module's `dac` disassembly in /tmp/dac_text.txt). Dynamic: `--probe` with
`probe-recs.py` / `probe-list.sh` (completion records per dispatcher call), `--watch` + `--watch-max`,
`unmapped-summary.sh`, `flash-diff.py` (what the host wrote to flash), `uart-decode.py` / `uartb-decode.py`,
`dante-frames.sh`, `perf-run.sh`, `flash-dump.py` / `flash-set.py` / `flash-records.py` (parameter-store records in the flash image),
`oled-bars.py` (measure METER page bars in an OLED capture).
