// Rio3224-D2 audio engine: 32 input chains (analog front end -> head amp -> ADC -> DSP: gains, filter A, HPF,
// gain compensation -> Dante TX), 24 Dante RX -> 16 analog outputs (DAC) + 8 AES/EBU outputs.
// Parameters come from the firmware's hardware writes (HwSnapshot), audio from a host backend, transport from an
// IDanteTransport.  No UI code in here; meters are measurements published for the firmware and the inspector.
#pragma once
#include "audio_types.h"
#include "biquad.h"
#include "host_backend.h"
#include "dante_transport.h"
#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rio {

struct InputTrace {              // developer signal inspector, one input: peak levels of the last block at each stage
    float hostPeakDbfs = -200.f;     // host float (interface full scale)
    float socketPeakDbu = -200.f;    // at the virtual XLR
    float haPeakDbu = -200.f;        // after the analog gain (before the converter)
    float adcPeakDbfs = -200.f;      // converter output
    float hpfPeakDbfs = -200.f;      // after the DSP filters and gain words
    float txPeakDbfs = -200.f;       // after gain compensation = Dante TX
    float analogGainDb = -6.f, gcGainDb = 0.f;
    bool analogClip = false, adcClip = false, phantom = false, hpfOn = false, gcOn = false;
};
struct OutputTrace {
    float rxPeakDbfs = -200.f, dacPeakDbu = -200.f, hostPeakDbfs = -200.f;
    bool clip = false;
};

class RioAudioEngine {
public:
    RioAudioEngine();
    EngineConfig cfg;

    void setBackend(IHostAudioBackend* b) { backend = b; }
    void setTransport(IDanteTransport* t) { transport = t; }
    void setSampleRate(uint32_t fs);
    uint32_t sampleRate() const { return fs; }
    // virtual cables: Rio input socket <- host input channel (-1 = nothing plugged in); host output <- Rio analog output
    void mapInput(int rioInput, int hostChannel) { if (rioInput >= 0 && rioInput < kInputs) inputMap[rioInput] = hostChannel; }
    void mapOutput(int hostOutput, int rioOutput) { if (hostOutput >= 0 && hostOutput < kMaxHostOut) outputMap[hostOutput] = rioOutput; }
    int inputMapOf(int rioInput) const { return inputMap[rioInput]; }

    // emulation side: the firmware's current hardware programming (any thread; copied, cheap)
    void publishHw(const HwSnapshot& s);
    // audio side: one block.  Pulls host inputs, runs every chain, exchanges Dante blocks, pushes host outputs.
    void processBlock(size_t frames);

    // firmware-facing: DSP meter port word for (bank 0 = inputs, 1 = inputs pre-DSP, 2 = outputs), index within bank
    uint16_t meterWord(int bank, int index) const;
    InputMeter inputMeter(int ch) const;
    OutputMeter outputMeter(int ch) const;
    const InputTrace& trace(int ch) const { return traces[ch]; }
    const OutputTrace& outTrace(int ch) const { return outTraces[ch]; }
    uint64_t framesProcessed() const { return sampleTime; }
    std::string describeInput(int ch) const;     // one-line inspector text
    const HwSnapshot& currentHw() const { return hw[hwIdx.load(std::memory_order_acquire)]; }

    static constexpr int kMaxHostIn = 64, kMaxHostOut = 64;
private:
    struct Chain {
        Biquad fA, fH;
        Smooth g1, g2, g3, gc, fADry, fAWet, hDry, hWet, mute;
        uint8_t gainIndex = 0; uint8_t pendingIndex = 0; bool fading = false;
        float analogGainLin = 0.5f;          // -6 dB
        float meterPeak = 0.f;               // post-GC, with release
        float meterPeakPre = 0.f;            // ADC output level
        float rmsAcc = 0.f;
        bool analogClip = false, adcClip = false;
    };
    struct OutChain { Smooth mute; float meterPeak = 0.f; bool clip = false; };

    IHostAudioBackend* backend = nullptr;
    IDanteTransport* transport = nullptr;
    uint32_t fs = 48000;
    uint64_t sampleTime = 0;
    HwSnapshot hw[2]; std::atomic<int> hwIdx{ 0 };
    Chain ch[kInputs]; OutChain outCh[kAnalogOuts + kAesOuts];
    int inputMap[kInputs]; int outputMap[kMaxHostOut];
    std::vector<float> hostIn[kMaxHostIn], tx[kDanteTx], rx[kDanteRx], hostOut[kMaxHostOut], aesOut[kAesOuts], zeros;
    float meterFall = 1.f;
    std::atomic<uint16_t> meterCodes[3][kInputs];
    InputTrace traces[kInputs]; OutputTrace outTraces[kAnalogOuts + kAesOuts];
    float hostInScale = 1.f, analogClipLin = 1.f, dacToHost = 1.f;
    void recomputeCalibration();
    uint16_t codeFor(float peakLin, bool clip) const;
};

} // namespace rio
