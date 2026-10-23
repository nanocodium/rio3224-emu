// Glue between the emulated board and the audio engine: builds the engine's hardware snapshot from what the
// firmware wrote (head-amp link frames, DSP coefficient registers, parameter tree), drives the engine in lockstep
// with emulated time (one block per emulated millisecond), answers the DSP meter port, and owns the host
// backend / Dante transport chosen on the command line.
#pragma once
#include "audio/engine.h"
#include "audio/host_backend.h"
#include "audio/wasapi_backend.h"
#include "audio/dante_transport.h"
#include "audio/dante_virtual.h"
#include "net/dante_endpoint.h"
#include "audio/wav.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct Machine;

class AudioGlue {
public:
    explicit AudioGlue(Machine& m);
    // command-line configuration (filled by main.cpp before start())
    std::string inPath; bool inLoop = false;
    std::string outPath, aesPath, txPath;          // analog outputs (16 ch float WAV), AES/EBU outputs (8 ch 24-bit), Dante TX capture (32 ch)
    std::vector<std::string> tones;                // ToneBackend specs
    std::string devIn, devOut; double devLatencyMs = 30.0;   // --audio-dev in=SPEC,out=SPEC[,latency=MS]: real host interface (WASAPI on Windows)
    std::string patchSpec;                         // patches "rx@tx,..." (own TX) or "rx@Device:tx,..." (virtual network)
    std::string netPrimary, netSecondary;          // --dante-net [GROUP:PORT[@IFADDR]] / --dante-net2: UDP virtual Dante network instead of the in-process loopback
    std::string netName;                           // --dante-name: device name on the virtual network (default: the module's device label)
    bool netClockLeader = true;
    // --dante-ctl IFADDR[;KEY=VALUE...]: Controller-interoperable Dante endpoint on a real interface (keys: sec=IFADDR2,
    // trace=FILE.pcap, latency=MS, prefer=0|1, ports=ARC,CMC,INFO,FLOWS, persist=FILE, verbose=1, slaveonly=1, name=NAME)
    std::string ctlSpec; bool endpointVerbose = false;
    struct NetFail { uint64_t ms; bool secondary; bool failed; bool done = false; };
    std::vector<NetFail> netFails;                 // --dante-fail primary|secondary@MS[-MS2]: cable pulled / plugged back
    std::string mapSpec;                           // "rio:host,..." input socket <- host channel (1-based), e.g. "1:3,2:4"
    double latencyMs = 1.0, dropPercent = 0.0;
    double hostInDbu = 24.0, hostOutDbu = 24.0;
    int traceInput = 0;                            // print the inspector line of this input (1-based) every traceMs
    int traceMs = 500, meterPrintMs = 0;
    bool enabled = false;

    bool start();
    void tick(uint64_t ms);                        // from Machine::tick
    void shutdown();
    uint16_t meterRead(int index) const;           // DSP meter port (+0x8E)
    void buildSnapshot(rio::HwSnapshot& s) const;  // public for the inspector / tests

    rio::RioAudioEngine engine;
    std::unique_ptr<rio::IHostAudioBackend> backend;
    std::unique_ptr<rio::LoopbackTransport> loop;
    std::unique_ptr<rio::VirtualDanteTransport> vnet;
    std::unique_ptr<rio::DanteEndpoint> endpoint;  // --dante-ctl: the real-network endpoint (Dante Controller sees it)
    rio::IDanteTransport* transport = nullptr;     // whichever of the two is active
    std::unique_ptr<rio::WavWriter> txTap;
    // firmware parameter storage (RIO2_PROG V1.91): NonVolatile per-channel records of 5 words, stride 20 bytes
    uint32_t paramGcOn = 0x0C7A1FC8, paramGcStart = 0x0C7A2248, paramGain = 0x0C7A1D48, paramHpfOn = 0x0C7A1848, paramHpfFreq = 0x0C7A1AC8, param48V = 0x0C7A15C8;
    uint32_t paramStride = 20;
private:
    Machine& m;
    uint64_t lastMs = 0, nextTraceMs = 0, nextMeterMs = 0, nextPeerMs = 0, nextBridgeMs = 0;
    // layer A <-> layer B bridge state (what the module model last saw / told)
    std::string bridgeName; std::vector<std::string> bridgeTxLabels, bridgeRxLabels; bool bridgeLink = true, bridgeMaster = true, bridgeLocked = true, bridgePreferred = false; uint32_t bridgeFs = 0; int bridgeSw = -1;
    void bridgeTick(uint64_t ms);
    // With the network endpoint the engine runs on its own real-time thread clocked by the PTP media clock - like the
    // unit's DSP runs from the Dante word clock - so the audio timing does not depend on how fast the CPU emulation
    // happens to run.  The emulator thread only publishes the firmware's hardware programming (snapshot) to it.
    std::thread rtThread; std::atomic<bool> rtQuit{ false }; std::atomic<uint32_t> rtPendingFs{ 0 };
    int64_t rtBase = 0; bool rtAnchored = false; uint64_t rtStalls = 0, rtBlocks = 0;
    void rtLoop();
    std::vector<uint8_t> haTx;                     // UART B frame being assembled from the host's TX bytes (LEN-prefixed)
    double frameAcc = 0.0;
    rio::HwSnapshot snap;
    int8_t meterBank[256]; uint8_t meterPos[256];
    bool warnedGain = false;
};
