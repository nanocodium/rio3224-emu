#!/usr/bin/env python3
"""apply_docs_part5.py : documentation for the audio engine work (HARDWARE.md findings part 5, README audio section, AUDIT.md)."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

def rw(path, pairs, append=None):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
    if append: s = s.rstrip('\n') + '\n' + append
    open(p, 'w', encoding='utf-8', newline='').write(s)

rw('docs/HARDWARE.md', [], append="""
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
""")

rw('README.md', [
("""## Analysis tools (tools/)""",
 """## Audio engine (PCM)

The emulated board drives a real signal chain (`emu/src/audio/`): 32 input sockets -> analog head amp (gain
index from the firmware's SCIF1 frames, 1 dB steps, clip at the modelled rail) -> 24-bit converter (full scale
+25.5 dBu at the head-amp output so that the documented +30 dBu maximum input @ -6 dB lands on 0 dBFS after the
firmware's +1.5 dB DSP word) -> the DSP words and biquads exactly as the firmware wrote them (gain words, filter A,
user HPF with the firmware's 2nd-order coefficients and dry/wet pair) -> gain compensation (start gain - current
gain, from the firmware's parameter tree) -> Dante TX 1-32. Dante RX 1-16 -> DAC (0 dBFS = +24 dBu) -> analog
outputs, RX 17-24 -> AES/EBU 1-8 (24-bit PCM). Audio runs in lockstep with emulated time (one block per
emulated millisecond) or paced to the wall clock with `--realtime`; it is muted until the firmware's DIO locks.

| option | effect |
|---|---|
| `--tone CH:sine\\|sweep\\|impulse\\|noise\\|square:FREQ:dBu[:FREQ2:SEC]` | test signal on socket CH (1-32 or `all`), level in dBu at the XLR |
| `--audio-in FILE.wav[,loop]` | host input channels from a WAV file (channel n -> socket n, or `--audio-map "rio:host,..."`) |
| `--audio-out FILE.wav`, `--aes-out FILE.wav` | record the 16 analog outputs (float) / the 8 AES outputs (24-bit) |
| `--tx-out FILE.wav` | record all 32 Dante TX channels (a virtual Dante receiver) |
| `--dante-patch "rx@tx,..."` | loopback transport: subscribe RX channels to this unit's own TX channels |
| `--dante-latency-ms X`, `--dante-drop PCT` | receive latency (default 1 ms) and block-loss simulation |
| `--host-in-dbu X`, `--host-out-dbu X` | calibration: host full scale at the sockets / at the outputs (default +24 dBu) |
| `--trace-input N [--trace-ms M]`, `--meter-print M` | signal inspector line for socket N / engine meters every M ms |
| `--realtime` | pace the emulation to wall-clock time |
| `--console-script FILE`, `--rotate STEPS@T[,x,MS]` | type into the firmware's debug monitor at given times; turn the front-panel encoder |

Tests: `tools/audio-test-tones.sh` (32 unique tones -> 32 TX channels, exact frequencies and levels),
`tools/audio-test-hpf.sh` (HPF switched through the firmware's parameter tree; measured attenuation equals the
programmed biquad to 0.01 dB). See `docs/AUDIT.md` for the subsystem-by-subsystem state.

## Analysis tools (tools/)"""),
])

rw('docs/AUDIT.md', [
("""| Front panel: OLED menus, encoder, buttons, panel lock | REAL (control) / PARTIAL (input) | two SSD1322 panels rendered; switches via `--press`; encoder rotation (ADC-sampled) not modelled yet | model the encoder; live panel view |""",
 """| Front panel: OLED menus, encoder, buttons, panel lock | REAL | two SSD1322 panels rendered; LEFT/RIGHT/push via `--press 3|4|5`, rotary via `--rotate` (PLD +0 bits 13/14, firmware quadrature decode); gain changes from the encoder reach the head-amp frames and the engine | live panel view |"""),
("""| Meters | MISSING | DSP meter port (+0x8E) returns 0; `dsp meter` shows 00 | engine publishes peak levels into the meter port in the firmware's 7-bit code |""",
 """| Meters | PARTIAL | engine publishes 7-bit level codes on the DSP meter port (+0x8E); the firmware only reads the port while a consumer requests meters (METER page / remote) - request path still to be traced | finish the meter-request path, calibrate SIG/PEAK codes |"""),
("""| Analog inputs x32: head amp | PARTIAL | firmware sends the 2-byte SCIF1 frames (fpga, channel, gain index 0..72 = -6..+66 dB, phantom bit) and the DSP fine-gain word (Q30, 0..5 dB); no PCM | engine: 32 input chains with analog coarse gain, headroom/clip model, ADC, fine gain |""",
 """| Analog inputs x32: head amp, ADC | REAL | 32 chains process host PCM with the gain index from the firmware's head-amp frames (soft-mute on steps), analog rail clip, 24-bit converter at +25.5 dBu FS; `tools/audio-test-tones.sh`: 32 tones land on 32 TX channels at the predicted -50.0 dBFS | analog noise model optional |"""),
("""| HPF (20-600 Hz) | PARTIAL | firmware writes DSP coefficient registers (6 per channel) when HPF changes; no filter runs | engine runs the biquad with the firmware's coefficients |""",
 """| HPF (20-600 Hz) | REAL | engine runs the firmware's biquad words and dry/wet pair; `tools/audio-test-hpf.sh` measures -3.01 dB at 125 Hz, -24 dB at 31.5 Hz, equal to the programmed filter | - |"""),
("""| Gain compensation | PARTIAL | firmware parameter + DSP gain word; no PCM | engine applies the firmware's GC gain word to PCM |""",
 """| Gain compensation | PARTIAL | engine applies start gain - current gain digitally after the DSP filters, reading GC on/start gain from the firmware's parameter storage; GC cannot yet be switched on in tests (the debug monitor's `set gcon` is broken in V1.91, the front-panel GC page not yet reached) | drive GC from the panel / R Remote, run the acceptance test |"""),
("""| Dante TX 32 / RX 24 PCM | MISSING | module model carries control only | VirtualDanteTransport (multichannel PCM, subscriptions, clock, latency, loss, redundancy) |""",
 """| Dante TX 32 / RX 24 PCM | PARTIAL | TX/RX carry real PCM through the in-process LoopbackTransport (subscriptions, latency buffer, loss simulation, TX capture); no inter-process/UDP transport yet | VirtualDanteTransport over UDP (primary/secondary), virtual console |"""),
("""| Analog outputs x16 / DAC | MISSING | SIG-out LEDs decoded only | engine output chains fed from Dante RX routing |""",
 """| Analog outputs x16 / DAC | REAL (model) | RX 1-16 -> 24-bit DAC word -> +24 dBu full scale -> host output calibration; muted until the DIO locks | loop test with a real host device |"""),
("""| AES/EBU outputs x8 | PARTIAL | AK4101/CS8406 transmitter chips detected and configured by the firmware; no PCM | AES3 formatter carrying the routed PCM, virtual receiver |""",
 """| AES/EBU outputs x8 | PARTIAL | RX 17-24 -> 24-bit PCM to the AES outputs (`--aes-out` records them); AES3 channel-status framing not modelled | AES3 block framer + virtual receiver |"""),
("""| Host audio I/O (ASIO/WASAPI/...) | MISSING | none | backend abstraction + implementations (file/tone for tests, network cable, native) |""",
 """| Host audio I/O (ASIO/WASAPI/...) | PARTIAL | IHostAudioBackend with File (WAV in/out) and Tone generators; no live device backend yet (Windows: MSVC 2022 + Windows SDK with Audioclient.h available for a WASAPI backend) | WASAPI backend + Windows build |"""),
])
print('docs part 5 applied')
