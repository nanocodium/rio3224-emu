// Dante transport abstraction.  The Rio hands its 32 TX channels to the transport every block and takes its 24 RX
// channels back.  Everything that moves is PCM at the word-clock rate.
//   LoopbackTransport  - in-process virtual network: a patch table (rx <- tx of this or any registered device),
//                        a receive latency buffer, optional packet (block) loss; used for tests and single-process setups
//   VirtualDanteTransport (dante_virtual.*) - UDP between emulator instances / virtual consoles, primary + secondary
#pragma once
#include "audio_types.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace rio {

struct DanteStatus {
    bool primaryLink = true, secondaryLink = false;   // cable/link state as seen by the module
    bool primaryActive = true;                         // which port currently carries the audio (redundant mode)
    bool redundant = false;                            // switch mode: redundant vs daisy chain (from the firmware)
    uint32_t fs = 48000;
    uint32_t latencyFrames = 48;                       // receive latency (1 ms default)
    uint64_t txBlocks = 0, rxBlocks = 0, rxLost = 0, rxLate = 0;
    bool clockMaster = true;
};

class IDanteTransport {
public:
    virtual ~IDanteTransport() = default;
    virtual const char* name() const = 0;
    virtual void setFormat(uint32_t fs, int blockFrames) = 0;
    // called once per block with the Rio's TX channels (floats, 0 dBFS = 1.0); sampleTime = first frame's index on the word clock
    virtual void sendTx(const float* const* tx, int nch, size_t frames, uint64_t sampleTime) = 0;
    // fills the Rio's RX channels for the same block (silence where nothing is subscribed / nothing arrived in time)
    virtual void receiveRx(float* const* rx, int nch, size_t frames, uint64_t sampleTime) = 0;
    virtual const DanteStatus& status() const = 0;
    // subscription: this device's rx channel <- (deviceName, txChannel) ; empty name = unsubscribe
    virtual bool subscribe(int rxChannel, const std::string& device, int txChannel) = 0;
    virtual std::string subscription(int rxChannel) const = 0;
    virtual void setLatencyFrames(uint32_t n) = 0;
};

// A virtual device on the loopback "network": any producer of TX channels (the Rio itself, a virtual console...)
struct LoopbackDevice {
    std::string name;
    int txChannels = 0;
    std::vector<std::vector<float>> ring;    // per tx channel ring buffer (power-of-two length)
    uint64_t writePos = 0;                   // sample index of the next frame to be written
};

class LoopbackTransport : public IDanteTransport {
public:
    explicit LoopbackTransport(const std::string& selfName = "Rio3224-D2") { self = addDevice(selfName, kDanteTx); }
    const char* name() const override { return "loopback"; }
    void setFormat(uint32_t fs, int) override { st.fs = fs; }
    int addDevice(const std::string& n, int txChannels) {
        LoopbackDevice d; d.name = n; d.txChannels = txChannels; d.ring.assign(txChannels, std::vector<float>(kRing, 0.f));
        devices.push_back(std::move(d)); return (int)devices.size() - 1;
    }
    // producers other than the Rio push their TX here (e.g. a virtual console); frames are placed at their sampleTime
    void pushDeviceTx(int dev, const float* const* tx, int nch, size_t frames, uint64_t sampleTime) {
        auto& d = devices[dev];
        for (int c = 0; c < nch && c < d.txChannels; c++)
            for (size_t i = 0; i < frames; i++) d.ring[c][(sampleTime + i) & (kRing - 1)] = tx[c][i];
        d.writePos = sampleTime + frames;
    }
    std::function<void(const float* const* tx, int nch, size_t frames)> txTap;   // e.g. a WAV recorder = virtual Dante receiver of all TX channels
    void sendTx(const float* const* tx, int nch, size_t frames, uint64_t sampleTime) override {
        st.txBlocks++;
        if (txTap) txTap(tx, nch, frames);
        if (dropPercent > 0 && drop(rng) < dropPercent) { st.rxLost++; return; }   // the whole block is lost on the (single) path
        pushDeviceTx(self, tx, nch, frames, sampleTime);
    }
    void receiveRx(float* const* rx, int nch, size_t frames, uint64_t sampleTime) override {
        st.rxBlocks++;
        for (int c = 0; c < nch; c++) {
            const Sub& s = subs[c];
            if (s.dev < 0 || !(st.primaryLink || (st.redundant && st.secondaryLink))) { for (size_t i = 0; i < frames; i++) rx[c][i] = 0.f; continue; }
            const auto& d = devices[s.dev];
            for (size_t i = 0; i < frames; i++) {
                uint64_t t = sampleTime + i;                     // play what was sent latencyFrames ago
                rx[c][i] = (t >= st.latencyFrames) ? d.ring[s.ch][(t - st.latencyFrames) & (kRing - 1)] : 0.f;
            }
        }
    }
    const DanteStatus& status() const override { return st; }
    DanteStatus& mutableStatus() { return st; }
    bool subscribe(int rxChannel, const std::string& device, int txChannel) override {
        if (rxChannel < 0 || rxChannel >= kDanteRx) return false;
        if (device.empty()) { subs[rxChannel] = {}; return true; }
        for (size_t i = 0; i < devices.size(); i++)
            if (devices[i].name == device && txChannel >= 0 && txChannel < devices[i].txChannels) { subs[rxChannel] = { (int)i, txChannel }; return true; }
        return false;
    }
    std::string subscription(int rxChannel) const override {
        const Sub& s = subs[rxChannel]; if (s.dev < 0) return "";
        return devices[s.dev].name + "@" + std::to_string(s.ch + 1);
    }
    void setLatencyFrames(uint32_t n) override { st.latencyFrames = n; }
    double dropPercent = 0.0;
    // "rx@tx,rx@tx,..." : patch rx channels (1-based) to this device's own tx channels (the Rio looped onto itself)
    void patchSelf(const std::string& spec) {
        std::string cur; auto apply = [&](const std::string& p) {
            size_t at = p.find('@'); if (at == std::string::npos) return;
            int rx = atoi(p.substr(0, at).c_str()), tx = atoi(p.substr(at + 1).c_str());
            if (rx >= 1 && rx <= kDanteRx && tx >= 1 && tx <= kDanteTx) subscribe(rx - 1, devices[self].name, tx - 1);
        };
        for (char ch : spec) { if (ch == ',') { apply(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty()) apply(cur);
    }
    int self = 0;
    std::vector<LoopbackDevice> devices;
private:
    static constexpr size_t kRing = 1 << 16;
    struct Sub { int dev = -1; int ch = 0; };
    Sub subs[kDanteRx];
    DanteStatus st;
    std::mt19937 rng{ 99 }; std::uniform_real_distribution<double> drop{ 0.0, 100.0 };
};

} // namespace rio
