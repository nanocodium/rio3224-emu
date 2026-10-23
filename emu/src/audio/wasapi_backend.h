// Windows host audio backend (WASAPI shared mode): real audio interfaces' inputs become the Rio's input sockets
// and the Rio's analog outputs feed an interface's outputs ("virtual XLR cables").  Several capture devices can be
// used at once (their channels are concatenated onto the host input channels, e.g. an interface on host 1-2 and a
// software source such as a stream mixer on host 3-4; `--audio-map` then assigns host channels to sockets).  Capture
// may also be the WASAPI loopback of a playback device (what Windows is playing).  Each device runs at its own rate and
// clock; the backend resamples to the Rio's 44.1/48 kHz with a windowed-sinc converter and a fill-level servo that
// absorbs the drift between the clocks (resampler.h).  On other platforms the class exists but open() fails.
#pragma once
#include "host_backend.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rio {

struct HostDeviceInfo { std::string id, name; bool capture = false; int channels = 0; uint32_t rate = 0; bool isDefault = false; };
std::vector<HostDeviceInfo> wasapiListDevices(std::string* err = nullptr);

class WasapiBackend : public IHostAudioBackend {
public:
    WasapiBackend();
    ~WasapiBackend() override;
    // inSpec: "" = none, or one or more specs separated by ';' - each "default", "loopback" / "loopback:NAME" (capture a
    // playback device's output) or a case-insensitive substring of a capture device name.  outSpec: one render device
    // ("default" or a name substring).  rioFs = the Rio's sample rate; targetLatencyMs = buffering per direction.
    bool open(const std::string& inSpec, const std::string& outSpec, uint32_t rioFs, double targetLatencyMs, std::string& err);
    void close();
    const char* name() const override { return "wasapi"; }
    uint32_t sampleRate() const override { return 0; }          // resampled internally: the engine keeps the Rio's rate
    int inputChannels() const override { return inCh; }
    int outputChannels() const override { return outCh; }
    void pullInputs(float* const* ch, int nch, size_t frames) override;
    void pushOutputs(const float* const* analog, int nAnalog, const float* const* aes, int nAes, size_t frames) override;

    struct DirStats {
        std::string name; bool loopback = false; int channels = 0; uint32_t rate = 0; int firstHostChannel = 0;
        uint64_t frames = 0;                               // frames delivered to / taken from the engine (Rio rate)
        uint64_t engineStarved = 0;                        // capture: ring empty when the engine pulled; render: ring full when it pushed
        uint64_t deviceGlitches = 0;                       // capture: device data dropped (ring full); render: silence inserted (ring empty)
        double ratioScale = 1.0;                           // drift servo state (1.0 = nominal)
        double fillMs = 0;                                 // current buffering
    };
    std::vector<DirStats> inputStats() const;
    DirStats outputStats() const;
    std::string describe() const;

    int inCh = 0, outCh = 0;
private:
    struct Impl; Impl* impl = nullptr;
};

} // namespace rio
