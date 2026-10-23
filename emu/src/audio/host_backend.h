// Host audio connection layer: the "virtual XLR cables".  A backend delivers host input channels (which the
// engine maps onto the 32 input sockets) and takes the Rio's 16 analog and 8 AES/EBU outputs.
//   FileBackend  - WAV in / WAV out (tests, offline processing)
//   ToneBackend  - test signal generators per socket (sine, log sweep, impulse, noise) with levels in dBu
//   NullBackend  - silence in, outputs discarded
// A real-device backend (WASAPI/ASIO/CoreAudio/ALSA...) implements the same interface and drives the engine
// from its own audio thread.
#pragma once
#include "audio_types.h"
#include "wav.h"
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace rio {

class IHostAudioBackend {
public:
    virtual ~IHostAudioBackend() = default;
    virtual const char* name() const = 0;
    virtual uint32_t sampleRate() const = 0;          // host rate (0 = follows the Rio)
    virtual int inputChannels() const = 0;
    virtual int outputChannels() const = 0;
    // fill host input channels (planar float, host full scale = 1.0); channels beyond inputChannels() are zeroed
    virtual void pullInputs(float* const* ch, int nch, size_t frames) = 0;
    // deliver the Rio's outputs (host full scale = 1.0 after the output calibration)
    virtual void pushOutputs(const float* const* analog, int nAnalog, const float* const* aes, int nAes, size_t frames) = 0;
    virtual bool finished() const { return false; }
};

class NullBackend : public IHostAudioBackend {
public:
    const char* name() const override { return "null"; }
    uint32_t sampleRate() const override { return 0; }
    int inputChannels() const override { return 0; }
    int outputChannels() const override { return 0; }
    void pullInputs(float* const* ch, int nch, size_t frames) override { for (int c = 0; c < nch; c++) for (size_t i = 0; i < frames; i++) ch[c][i] = 0.f; }
    void pushOutputs(const float* const*, int, const float* const*, int, size_t) override {}
};

// WAV files: input file channels -> host inputs 1..N ; outputs -> analog file (16 ch) and AES file (8 ch)
class FileBackend : public IHostAudioBackend {
public:
    const char* name() const override { return "file"; }
    bool openInput(const std::string& path, bool loop) { if (!in.open(path)) return false; in.loop = loop; return true; }
    bool openAnalogOut(const std::string& path, uint32_t fs) { return outA.open(path, kAnalogOuts, fs, 32); }
    bool openAesOut(const std::string& path, uint32_t fs) { return outAes.open(path, kAesOuts, fs, 24); }
    uint32_t sampleRate() const override { return in.channels ? in.sampleRate : 0; }
    int inputChannels() const override { return in.channels; }
    int outputChannels() const override { return kAnalogOuts; }
    void pullInputs(float* const* ch, int nch, size_t frames) override {
        if (in.channels) in.read(ch, nch, frames);
        else for (int c = 0; c < nch; c++) for (size_t i = 0; i < frames; i++) ch[c][i] = 0.f;
    }
    void pushOutputs(const float* const* analog, int nAnalog, const float* const* aes, int nAes, size_t frames) override {
        if (outA.frames || analogOpen()) outA.write(analog, nAnalog, frames);
        if (aesOpen()) outAes.write(aes, nAes, frames);
    }
    bool finished() const override { return in.channels && in.eof(); }
    void close() { outA.close(); outAes.close(); in.close(); }
private:
    bool analogOpen() const { return outA.frames >= 0 && outAOpened(); }
    bool outAOpened() const { return outAopen; }
    bool aesOpen() const { return outAesOpen; }
public:
    bool outAopen = false, outAesOpen = false;   // set by the owner after open*()
    WavReader in; WavWriter outA, outAes;
};

// Test generators per host input channel. Levels are given in dBu at the virtual XLR; the backend converts them to
// host floats with the same calibration the engine uses (hostInFsDbu), so "a -20 dBu sine on input 1" is exact.
class ToneBackend : public IHostAudioBackend {
public:
    enum class Kind { Off, Sine, Sweep, Impulse, Noise, Square };
    struct Gen { Kind kind = Kind::Off; double freq = 1000.0, freq2 = 20000.0, levelDbu = 0.0, sweepSec = 10.0; double phase = 0.0, t = 0.0; bool done = false; };
    Gen gen[kInputs];
    double hostInFsDbu = 24.0;
    uint32_t fs = 48000;
    const char* name() const override { return "tone"; }
    uint32_t sampleRate() const override { return 0; }
    int inputChannels() const override { return kInputs; }
    int outputChannels() const override { return 0; }
    // spec: "ch:kind:freq:dBu[:freq2:seconds]"  kinds: sine sweep impulse noise square ; ch 1-based or "all"
    bool parse(const std::string& spec) {
        std::vector<std::string> parts; std::string cur;
        for (char c : spec) { if (c == ':') { parts.push_back(cur); cur.clear(); } else cur += c; }
        parts.push_back(cur);
        if (parts.size() < 4) return false;
        Gen g; const std::string& k = parts[1];
        g.kind = k == "sine" ? Kind::Sine : k == "sweep" ? Kind::Sweep : k == "impulse" ? Kind::Impulse : k == "noise" ? Kind::Noise : k == "square" ? Kind::Square : Kind::Off;
        g.freq = atof(parts[2].c_str()); g.levelDbu = atof(parts[3].c_str());
        if (parts.size() > 4) g.freq2 = atof(parts[4].c_str());
        if (parts.size() > 5) g.sweepSec = atof(parts[5].c_str());
        if (parts[0] == "all") { for (auto& x : gen) { x = g; } return true; }
        int ch = atoi(parts[0].c_str()); if (ch < 1 || ch > kInputs) return false;
        gen[ch - 1] = g; return true;
    }
    void pullInputs(float* const* ch, int nch, size_t frames) override {
        for (int c = 0; c < nch; c++) {
            float* out = ch[c];
            if (c >= kInputs || gen[c].kind == Kind::Off) { for (size_t i = 0; i < frames; i++) out[i] = 0.f; continue; }
            Gen& g = gen[c];
            float amp = (float)std::pow(10.0, (g.levelDbu - hostInFsDbu) / 20.0);    // dBu -> host float (peak of a sine = level)
            for (size_t i = 0; i < frames; i++) {
                double v = 0.0;
                switch (g.kind) {
                case Kind::Sine:   v = std::sin(g.phase); g.phase += 2 * M_PI * g.freq / fs; break;
                case Kind::Square: v = std::sin(g.phase) >= 0 ? 1.0 : -1.0; g.phase += 2 * M_PI * g.freq / fs; break;
                case Kind::Sweep: {   // exponential sweep freq -> freq2 over sweepSec, then silence
                    if (g.t < g.sweepSec) {
                        double f = g.freq * std::pow(g.freq2 / g.freq, g.t / g.sweepSec);
                        v = std::sin(g.phase); g.phase += 2 * M_PI * f / fs;
                    } else { v = 0.0; g.done = true; }
                    break; }
                case Kind::Impulse: v = (g.t >= 0.5 && !g.done) ? 1.0 : 0.0; if (v != 0.0) g.done = true; break;   // one sample at t = 0.5 s
                case Kind::Noise:   v = rnd(rng); break;
                default: break;
                }
                if (g.phase > 2 * M_PI) g.phase -= 2 * M_PI;
                g.t += 1.0 / fs;
                out[i] = (float)v * amp;
            }
        }
    }
    void pushOutputs(const float* const*, int, const float* const*, int, size_t) override {}
private:
    std::mt19937 rng{ 1234 }; std::uniform_real_distribution<double> rnd{ -1.0, 1.0 };
};

} // namespace rio
