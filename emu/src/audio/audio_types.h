// Shared types of the Rio3224-D2 audio engine.
//
// The host firmware (running unmodified in the emulator) programs the audio hardware through two paths that the
// emulator observes:  the head-amp link (SCIF1 2-byte frames: fpga, channel, gain index | phantom) and the DSP
// register transport on CS4 (per-channel coefficient and gain words, Q30 fixed point).  The engine is handed a
// snapshot of those registers every emulated millisecond (HwSnapshot) and does the actual PCM work with them, so
// a gain or HPF change made on the front panel, by R Remote or by a console reaches the audio the same way it
// reaches the real DSP: through the firmware's register writes.
#pragma once
#include <cstdint>
#include <cstddef>

namespace rio {

constexpr int kInputs = 32;        // analog mic/line inputs = Dante TX channels
constexpr int kDanteTx = 32;
constexpr int kDanteRx = 24;       // Dante RX 1-16 -> analog outputs 1-16, 17-24 -> AES/EBU outputs 1-8 (fixed on the Rio3224-D2)
constexpr int kAnalogOuts = 16;
constexpr int kAesOuts = 8;
constexpr int kMaxBlock = 4096;

// One input channel as the firmware programmed it.
struct InputHw {
    uint8_t gainIndex = 0;          // HA frame value >> 1: analog gain = index - 6 dB (0 .. 72 -> -6 .. +66 dB)
    bool phantomBit = false;        // HA frame bit 0 (the firmware already clears it while the +48V master is off)
    bool present = true;            // head-amp sheet present (the firmware does not program absent sheets)
    float gain1 = 1.f;              // DSP word 0x4A + c  (Q30 linear; 1.0 on this firmware, "trim" calibration)
    float gain2 = 1.f;              // DSP word 0x52 + c  (0x4C106BA5 = +1.5 dB on this firmware)
    float gain3 = 1.f;              // DSP word 0x5A + c  (1.0)
    float filtA[5] = { 0.f, 0.f, 1.f, 0.f, 0.f };   // DSP block 0x0A + 6c, words +1..+5 = { -a2, -a1, b0, b1, b2 } (100 Hz HPF, bypassed by default)
    float filtADry = 1.f, filtAWet = 0.f;           // DSP pair 0x3A + 2c
    float hpf[5] = { 0.f, 0.f, 1.f, 0.f, 0.f };     // DSP block 0x62.. words +1..+5 (the user HPF: 2nd order, 20..600 Hz in the firmware's steps)
    float hpfDry = 1.f, hpfWet = 0.f;               // DSP pair 0x7A + 2c: {1,0} = HPF off, {0,1} = HPF on
    bool gcOn = false;              // gain compensation (firmware parameter; the real DSP derives it from the HA gain it receives)
    float gcStartGainDb = 0.f;      // HA gain at which GC was engaged (dB); compensation = start - current
};

struct HwSnapshot {
    InputHw in[kInputs];
    uint32_t fs = 48000;            // word clock the DIO/Dante module runs at
    bool clockLocked = false;       // DIO locked (firmware DSP "ALIVE"); audio is muted until then, like the real unit
    bool outputMute = true;         // firmware mute (DIO mute / DNT_MUTE)
    bool phantomMaster = false;     // rear +48V MASTER switch (PLD input)
    uint32_t serial = 0;            // bumped on every publish
};

// What the engine publishes per input for the firmware (DSP meter port) and for the developer inspector.
struct InputMeter {
    float peakDbfs = -200.f;        // post-GC (what goes to Dante), with the DSP-style fall-back ballistics
    float rmsDbfs = -200.f;
    bool sig = false, peak = false; // SIG / PEAK indications (thresholds in EngineConfig)
    bool analogClip = false;        // the analog front end was driven past its headroom in the last block
    bool adcClip = false;           // the converter hit full scale in the last block
};

struct OutputMeter {
    float peakDbfs = -200.f;
    bool sig = false;
    bool clip = false;
};

struct EngineConfig {
    // calibration: how a host float sample relates to volts at the virtual XLR, and the converters' references
    double hostInFsDbu = 24.0;      // host 1.0 (0 dBFS) at an input socket = +24 dBu (set to your interface's line-in full scale)
    double hostOutFsDbu = 24.0;     // host 1.0 at an output = +24 dBu (keep the Rio's own +24 dBu maximum)
    double adcFsDbu = 25.5;         // converter full scale at the head-amp output; with the firmware's +1.5 dB word this puts
                                    // +30 dBu @ -6 dB gain (the documented maximum input level) at exactly 0 dBFS on Dante
    double analogClipDbu = 27.5;    // where the analog front end itself saturates (2 dB above the ADC: converter clips first)
    double dacFsDbu = 24.0;         // 0 dBFS -> +24 dBu at the analog outputs (documented maximum output level)
    double sigThresholdDbfs = -40.0;  // owner's manual: SIG lights at -40 dBFS or higher, PEAK at -3 dBFS or higher
    double peakThresholdDbfs = -3.0;
    double meterFallDbPerSec = 12.0;    // meter release
    // DSP meter port word: 7-bit code in bits 11:5, dBFS = 0.75 * code - 96 (128 codes = the 96 dB range of 16 bits;
    // code 128 = 0 dBFS is signalled as "over" by bit 12). Calibrated against the firmware: its PEAK test is code >= 124
    // (= -3 dBFS) and its SIG test is code >= 75 (first code at or above -40 dBFS), exactly the manual's thresholds.
    int meterCodeFullScale = 128;
    double meterDbPerCode = 0.75;
    bool dither = false;            // TPDF dither at the ADC/DAC word boundaries (off = cleaner than the hardware)
    double haFadeMs = 4.0;          // soft mute around an analog gain step (the DSP strobes its channel block on every HA change)
    double gainSmoothMs = 10.0;     // digital gain ramps (GC, DSP gain words)
};

} // namespace rio
