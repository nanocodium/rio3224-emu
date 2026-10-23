#!/usr/bin/env python3
"""apply_docs_part4.py : one-off documentation update after the Dante start-up work (README + HARDWARE.md)."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

def patch(path, pairs):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:60])
        s = s.replace(old, new)
    return p, s

# ---------- README ----------
p, s = patch('README.md', [
("""| `--dante-browse c,f,g` | BROWSE_COMPLETE payload bytes (default 1,7,0 = monitoring completed) |""",
 """| `--dante-browse c,f,g` | BROWSE_COMPLETE payload bytes (default 1,7,0 = monitoring completed) |
| `--dante-swmode N` | switch mode stored in the module (0 = switched, the host's rear DIP default; a different value makes the host rewrite it and restart the module) |
| `--dante-fresh` | factory-fresh module: default device name `Rio3224-D2-<mac>`; the host renames it (`[DEV LABEL] ... LABEL:RESET`) |"""),
("""| `--watch ADDR,LEN` | report accesses to a physical address range (with PC) |""",
 """| `--watch ADDR,LEN` | report accesses to a physical address range (with PC); `--watch-max N` reports per address/size/direction (default 4) |"""),
("""Typical boot (about 50 s of emulated time including the Dante module reboot; ~30 s wall time at roughly 400
Minsn/s with the idle-loop fast forward): the debug monitor banner with the MAC address,
the DIO clock lock sequence, `DSP: state DEAD -> ALIVE`, `[DANTE] AUDIO_NETWORK_DEVICE_DETECTED!!`, the
first-boot Dante initialisation (`[DEV LABEL] UNIT:Y001-, LABEL:Y001-24-D2-000001`), a requested module
reboot (`[BK2 EVENT] START REBOOT SEQUENCE`) and finally the home screen (head-amp gains of channels 17-32)
on the two OLED panels (left: channels 1-16, right: 17-32). `tools/long-run.sh` and `tools/press-run.sh`
script this.""",
 """Typical boot (the home screen is up after about 25 s of emulated time; a 60 s run of 11e9 instructions
takes ~17 s wall time at 650-690 Minsn/s): the debug monitor banner with the MAC address, the DIO clock lock
sequence, `DSP: state DEAD -> ALIVE`, `[DANTE] AUDIO_NETWORK_DEVICE_DETECTED!!`, the Dante start-up exchange
(`[DEV LABEL] UNIT:Y001-, LABEL:Y001-24-D2-123456`) and the home screen (head-amp gains) on the two OLED
panels (left: channels 1-16, right: 17-32). The module model boots as a *configured* unit (device label,
switch mode and manufacturer record as the Rio host programs them), so the host does not rename or restart
it; `--dante-fresh` gives the factory-fresh path (rename), `--dante-swmode 1` the "switch mode changed"
path (SET NETWORK_SWMODE, SET BASE_RESTART, module away, `E00-03 DANTE MODULE ERROR` until it is back,
dismiss with `--press 5@T`). `tools/long-run.sh` and `tools/press-run.sh NAME MAXINSNS [args]` script
full runs (`FLASH=/tmp/x-flash.bin` reuses the flash image of an earlier run)."""),
("""lock sequence, DSP alive, and a behavioural Dante module model that takes the host through its whole BCA
start-up (device info, switch registers, labels, patches, clock status by notification, module reboot).
The UI reaches the home screen on both OLED panels, and popups render completely (text on the left panel,
OK button on the right).""",
 """lock sequence, DSP alive, and a behavioural Dante module model that takes the host through its whole BCA
start-up (device info, switch registers, labels, patches, clock status and device list by notification,
switch-mode notify, manufacturer record, requested module restart). The boot is clean: no host error prints
(`BCA_CALLBACK UNSUPPORTED`, `EXECUTE DANTE UPDATE EVENT`, `RESULT_RETRY` and the E00-03 module restart
are all gone), the UI reaches the home screen on both OLED panels, and popups render completely (text on
the left panel, OK button on the right)."""),
("""Speed: a full boot (11e9 instructions, including the Dante module reboot) takes about 20 s on a modern PC
(500+ Minsn/s): the RTOS idle loop is fast-forwarded, interrupts are only re-evaluated when a line or the
mask changes, and code is fetched straight from the SDRAM array.""",
 """Speed: 650-690 Minsn/s on a modern PC (a 60 s boot run of 11e9 instructions takes ~17 s wall time; it was
150 s before the speed work): the RTOS idle loop is fast-forwarded, interrupts are only re-evaluated when a
line or the mask changes, and both instruction fetches and data accesses to SDRAM bypass the bus
(`tools/perf-run.sh` profiles a boot; what is left is the instruction decoder itself)."""),
("""Not yet: the module's second UART (conmon_haremote_client protocol: the host sends its device info there,
a Dante console would answer), audio, and anything that needs a real Dante network (consoles, R Remote over
the module).""",
 """Not yet: the module's second UART (conmon_haremote_client protocol: the host registers its device there and
sends head-amp parameter notifications - 6 frames per boot, `tools/uartb-decode.py` - which a Dante console
would consume; the host does not wait for answers), audio, and anything that needs a real Dante network
(consoles, R Remote over the module). One harmless firmware quirk remains visible as `[bus] unmapped read
4C45445F`: a diagnostics registry looks a name up with the first four bytes of the string "LED_..." as the
pointer (`strnlen` on an unmapped address, bounded to 24 bytes)."""),
])
s = s.rstrip('\n') + """

## Analysis tools (tools/)

Static: `str-refs.py` (string -> literal-pool slots -> functions), `lit-refs.py ADDR` (which slots hold a
value and who uses them), `fn-grep.py PATTERN FUNC...` (grep inside decompiled functions), `slot-ctx.sh`
(disassembly around the instruction that loads a slot), `dac-fn.sh / dac-ctx.sh / dac-fns-matching.sh /
dac-notify-sites.sh` (the Dante module's `dac` disassembly in /tmp/dac_text.txt). Dynamic: `--probe` with
`probe-recs.py` / `probe-list.sh` (completion records per dispatcher call), `--watch` + `--watch-max`,
`unmapped-summary.sh`, `flash-diff.py` (what the host wrote to flash), `uart-decode.py` / `uartb-decode.py`,
`dante-frames.sh`, `perf-run.sh`.
"""
open(p, 'w', encoding='utf-8', newline='').write(s)

# ---------- HARDWARE.md ----------
p = os.path.join(root, 'docs', 'HARDWARE.md'); s = open(p, encoding='utf-8', newline='').read()
a = s.index('### First-boot sequence as seen with the module model')
b = s.index('### OLED dialogs and the front switches')
new_sec = """### Boot sequence as seen with the module model
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

"""
s = s[:a] + new_sec + s[b:]
s = s.rstrip('\n') + """

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
"""
open(p, 'w', encoding='utf-8', newline='').write(s)
print('docs updated')
