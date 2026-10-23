#include "engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rio {

static inline float dbToLin(double db) { return (float)std::pow(10.0, db / 20.0); }
static inline float linToDb(double lin) { return lin > 1e-10 ? (float)(20.0 * std::log10(lin)) : -200.f; }
static inline float quant24(float v) { return (float)(std::lrint(v * 8388608.0) / 8388608.0); }

RioAudioEngine::RioAudioEngine() {
    for (int i = 0; i < kInputs; i++) inputMap[i] = i;               // socket n <- host channel n by default
    for (int i = 0; i < kMaxHostOut; i++) outputMap[i] = i < kAnalogOuts ? i : -1;
    for (auto& bank : meterCodes) for (auto& m : bank) m.store(0);
    setSampleRate(48000);
}

void RioAudioEngine::setSampleRate(uint32_t f) {
    fs = f ? f : 48000;
    for (auto& b : hostIn) b.assign(kMaxBlock, 0.f);
    for (auto& b : tx) b.assign(kMaxBlock, 0.f);
    for (auto& b : rx) b.assign(kMaxBlock, 0.f);
    for (auto& b : hostOut) b.assign(kMaxBlock, 0.f);
    for (auto& b : aesOut) b.assign(kMaxBlock, 0.f);
    zeros.assign(kMaxBlock, 0.f);
    for (auto& c : ch) {
        c.fA.reset(); c.fH.reset();
        for (Smooth* s : { &c.g1, &c.g2, &c.g3, &c.gc, &c.fADry, &c.fAWet, &c.hDry, &c.hWet }) s->setTime(cfg.gainSmoothMs, fs);
        c.mute.setTime(cfg.haFadeMs / 4.0, fs); c.mute.jump(1.f);
        c.gc.jump(1.f); c.fADry.jump(1.f); c.fAWet.jump(0.f); c.hDry.jump(1.f); c.hWet.jump(0.f);
    }
    for (auto& o : outCh) { o.mute.setTime(5.0, fs); o.mute.jump(1.f); }
    meterFall = (float)std::pow(10.0, -cfg.meterFallDbPerSec / 20.0 / fs);
    recomputeCalibration();
    if (transport) transport->setFormat(fs, 0);
}

void RioAudioEngine::recomputeCalibration() {
    hostInScale = dbToLin(cfg.hostInFsDbu - cfg.adcFsDbu);      // host 1.0 -> socket level in converter-full-scale units
    analogClipLin = dbToLin(cfg.analogClipDbu - cfg.adcFsDbu);  // analog rail relative to converter full scale
    dacToHost = dbToLin(cfg.dacFsDbu - cfg.hostOutFsDbu);       // DAC 1.0 (+24 dBu) -> host float
}

void RioAudioEngine::publishHw(const HwSnapshot& s) {
    int cur = hwIdx.load(std::memory_order_acquire), nxt = cur ^ 1;
    hw[nxt] = s;
    hwIdx.store(nxt, std::memory_order_release);
}

uint16_t RioAudioEngine::codeFor(float peakLin, bool clip) const {
    if (clip) return 128;                                   // over
    if (peakLin <= 0.f) return 0;
    double db = 20.0 * std::log10(peakLin);
    double code = cfg.meterCodeFullScale + db / cfg.meterDbPerCode;
    if (code < 0.5) return 0;
    if (code >= 127.5) return 128;                          // >= 0 dBFS: over
    return (uint16_t)std::lrint(code);
}

void RioAudioEngine::processBlock(size_t frames) {
    if (frames > (size_t)kMaxBlock) frames = kMaxBlock;
    const HwSnapshot& h = hw[hwIdx.load(std::memory_order_acquire)];
    const bool audioEnabled = h.clockLocked && !h.outputMute;

    // 1. host inputs (virtual XLR cables)
    int nHost = backend ? std::min(backend->inputChannels(), kMaxHostIn) : 0;
    float* hostPtrs[kMaxHostIn]; for (int i = 0; i < kMaxHostIn; i++) hostPtrs[i] = hostIn[i].data();
    if (backend && nHost > 0) backend->pullInputs(hostPtrs, nHost, frames);

    // 2. input chains
    for (int c = 0; c < kInputs; c++) {
        Chain& k = ch[c]; const InputHw& p = h.in[c]; InputTrace& tr = traces[c];
        const float* src = (inputMap[c] >= 0 && inputMap[c] < nHost) ? hostIn[inputMap[c]].data() : zeros.data();
        float* out = tx[c].data();

        // head-amp step: fade out, switch, fade in (the DSP strobes the channel block on every HA change)
        if (p.gainIndex != k.gainIndex && !k.fading) { k.fading = true; k.pendingIndex = p.gainIndex; k.mute.target = 0.f; }
        // DSP words
        k.g1.target = p.gain1; k.g2.target = p.gain2; k.g3.target = p.gain3;
        k.fADry.target = p.filtADry; k.fAWet.target = p.filtAWet; k.hDry.target = p.hpfDry; k.hWet.target = p.hpfWet;
        float analogGainDb = (float)k.gainIndex - 6.f;
        float gcDb = p.gcOn ? (p.gcStartGainDb - analogGainDb) : 0.f;
        k.gc.target = dbToLin(gcDb);

        float hostPk = 0.f, sockPk = 0.f, haPk = 0.f, adcPk = 0.f, hpfPk = 0.f, txPk = 0.f;
        bool aClip = false, dClip = false;
        for (size_t i = 0; i < frames; i++) {
            float x = src[i];
            hostPk = std::max(hostPk, std::fabs(x));
            float sock = x * hostInScale;                       // socket level, converter-full-scale units
            sockPk = std::max(sockPk, std::fabs(sock));
            float m = k.mute.next();
            if (k.fading && k.mute.cur < 0.02f && k.mute.target == 0.f) { k.gainIndex = k.pendingIndex; k.analogGainLin = dbToLin((double)k.gainIndex - 6.0); k.mute.target = 1.f; }
            if (k.fading && k.mute.target == 1.f && k.mute.cur > 0.98f) k.fading = false;
            float a = sock * k.analogGainLin * m;               // head amp
            if (a > analogClipLin) { a = analogClipLin; aClip = true; } else if (a < -analogClipLin) { a = -analogClipLin; aClip = true; }
            haPk = std::max(haPk, std::fabs(a));
            // converter: full scale +-1.0, 24-bit word
            float d = a;
            if (d > 1.f) { d = 1.f; dClip = true; } else if (d < -1.f) { d = -1.f; dClip = true; }
            d = quant24(d);
            adcPk = std::max(adcPk, std::fabs(d));
            // DSP: gain words, filter A, user HPF
            d *= k.g1.next() * k.g2.next() * k.g3.next();
            float fa = k.fA.process(d, p.filtA);
            d = k.fADry.next() * d + k.fAWet.next() * fa;
            float fh = k.fH.process(d, p.hpf);
            d = k.hDry.next() * d + k.hWet.next() * fh;
            hpfPk = std::max(hpfPk, std::fabs(d));
            // gain compensation
            d *= k.gc.next();
            if (!audioEnabled || !p.present) d = 0.f;
            if (d > 1.f) d = 1.f; else if (d < -1.f) d = -1.f;   // Dante word is 24-bit: nothing above full scale leaves
            txPk = std::max(txPk, std::fabs(d));
            out[i] = d;
        }
        // meters with release ballistics
        k.meterPeak = std::max(txPk, k.meterPeak * std::pow(meterFall, (float)frames));
        k.meterPeakPre = std::max(adcPk, k.meterPeakPre * std::pow(meterFall, (float)frames));
        k.analogClip = aClip; k.adcClip = dClip;
        meterCodes[0][c].store(codeFor(k.meterPeak, txPk >= 0.999f), std::memory_order_relaxed);
        meterCodes[1][c].store(codeFor(k.meterPeakPre, dClip), std::memory_order_relaxed);
        tr.hostPeakDbfs = linToDb(hostPk); tr.socketPeakDbu = (float)(linToDb(sockPk) + cfg.adcFsDbu);
        tr.haPeakDbu = (float)(linToDb(haPk) + cfg.adcFsDbu); tr.adcPeakDbfs = linToDb(adcPk);
        tr.hpfPeakDbfs = linToDb(hpfPk); tr.txPeakDbfs = linToDb(k.meterPeak);   // TX shown with the meter's hold/release (1 ms blocks under-read low frequencies)
        tr.analogGainDb = analogGainDb; tr.gcGainDb = gcDb; tr.analogClip = aClip; tr.adcClip = dClip;
        tr.phantom = p.phantomBit && h.phantomMaster; tr.hpfOn = p.hpfWet > 0.5f; tr.gcOn = p.gcOn;
    }

    // 3. Dante
    const float* txPtrs[kDanteTx]; float* rxPtrs[kDanteRx];
    for (int i = 0; i < kDanteTx; i++) txPtrs[i] = tx[i].data();
    for (int i = 0; i < kDanteRx; i++) rxPtrs[i] = rx[i].data();
    if (transport) { transport->sendTx(txPtrs, kDanteTx, frames, sampleTime); transport->receiveRx(rxPtrs, kDanteRx, frames, sampleTime); }
    else for (int i = 0; i < kDanteRx; i++) memset(rx[i].data(), 0, frames * sizeof(float));

    // 4. outputs: RX 1-16 -> DAC -> analog outputs, RX 17-24 -> AES/EBU 1-8
    for (int o = 0; o < kAnalogOuts + kAesOuts; o++) {
        OutChain& oc = outCh[o]; OutputTrace& tr = outTraces[o];
        const float* in = rx[o].data();
        float* out = o < kAnalogOuts ? hostOut[o].data() : aesOut[o - kAnalogOuts].data();
        oc.mute.target = audioEnabled ? 1.f : 0.f;
        float rxPk = 0.f, outPk = 0.f; bool clip = false;
        for (size_t i = 0; i < frames; i++) {
            float d = in[i] * oc.mute.next();
            rxPk = std::max(rxPk, std::fabs(d));
            if (d > 1.f) { d = 1.f; clip = true; } else if (d < -1.f) { d = -1.f; clip = true; }
            d = quant24(d);                                  // DAC / AES word
            float v = o < kAnalogOuts ? d * dacToHost : d;   // analog: +24 dBu full scale -> host float; AES: 24-bit PCM as is
            outPk = std::max(outPk, std::fabs(v));
            out[i] = v;
        }
        oc.meterPeak = std::max(rxPk, oc.meterPeak * std::pow(meterFall, (float)frames)); oc.clip = clip;
        if (o < kDanteRx) meterCodes[2][o].store(codeFor(oc.meterPeak, clip), std::memory_order_relaxed);
        tr.rxPeakDbfs = linToDb(rxPk); tr.dacPeakDbu = o < kAnalogOuts ? (float)(linToDb(rxPk) + cfg.dacFsDbu) : -200.f;
        tr.hostPeakDbfs = linToDb(outPk); tr.clip = clip;
    }

    // 5. host outputs (virtual cables from the analog XLR outputs)
    if (backend) {
        int nOut = std::min(backend->outputChannels(), kMaxHostOut);
        const float* outPtrs[kMaxHostOut]; const float* aesPtrs[kAesOuts];
        for (int j = 0; j < kMaxHostOut; j++) { int r = (j < nOut) ? outputMap[j] : -1; outPtrs[j] = (r >= 0 && r < kAnalogOuts) ? hostOut[r].data() : zeros.data(); }
        for (int j = 0; j < kAesOuts; j++) aesPtrs[j] = aesOut[j].data();
        backend->pushOutputs(outPtrs, std::max(nOut, 0), aesPtrs, kAesOuts, frames);
    }
    sampleTime += frames;
}

uint16_t RioAudioEngine::meterWord(int bank, int index) const {
    if (bank < 0 || bank > 2 || index < 0 || index >= kInputs) return 0;
    uint16_t code = meterCodes[bank][index].load(std::memory_order_relaxed);
    // 7-bit level in bits 11:5; the firmware (FUN_0c160b48) treats any word with bits 15:12 set as "over" (shows 0x7F)
    return code >= 128 ? (uint16_t)(0x1000 | (127u << 5)) : (uint16_t)(code << 5);
}

InputMeter RioAudioEngine::inputMeter(int c) const {
    InputMeter m; if (c < 0 || c >= kInputs) return m;
    const Chain& k = ch[c];
    m.peakDbfs = linToDb(k.meterPeak); m.rmsDbfs = -200.f;
    m.sig = m.peakDbfs > cfg.sigThresholdDbfs; m.peak = m.peakDbfs > cfg.peakThresholdDbfs;
    m.analogClip = k.analogClip; m.adcClip = k.adcClip; return m;
}
OutputMeter RioAudioEngine::outputMeter(int o) const {
    OutputMeter m; if (o < 0 || o >= kAnalogOuts + kAesOuts) return m;
    m.peakDbfs = linToDb(outCh[o].meterPeak); m.sig = m.peakDbfs > cfg.sigThresholdDbfs; m.clip = outCh[o].clip; return m;
}

std::string RioAudioEngine::describeInput(int c) const {
    char b[320]; const InputTrace& t = traces[c]; const InputHw& p = currentHw().in[c];
    snprintf(b, sizeof b, "in%02d host %6.1f dBFS | socket %6.1f dBu | HA %+3.0f dB -> %6.1f dBu%s | ADC %6.1f dBFS%s | DSP %6.1f | GC %s %+5.1f dB -> TX %6.1f dBFS | HPF %s | 48V %s",
             c + 1, t.hostPeakDbfs, t.socketPeakDbu, t.analogGainDb, t.haPeakDbu, t.analogClip ? " CLIP" : "", t.adcPeakDbfs, t.adcClip ? " FS" : "",
             t.hpfPeakDbfs, p.gcOn ? "on " : "off", t.gcGainDb, t.txPeakDbfs, t.hpfOn ? "on " : "off", t.phantom ? "on" : "off");
    return b;
}

} // namespace rio
