// Controller-interoperable virtual Dante endpoint ("layer B" of the Rio's Dante port).
//
// What this is: an independent implementation of the publicly documented / reverse-engineered behaviour a Dante
// device shows on the wire, so that Audinate's Dante Controller running on the real network sees the emulated
// Rio3224-D2 as a device, opens its pages, and can route its 32 transmit / 24 receive channels with real PCM:
//   * DNS-SD over mDNS (dns_sd.*): `_netaudio-arc._udp`, `_netaudio-cmc._udp`, one `_netaudio-chan._udp` per TX channel
//   * ARC request/response server (UDP 4440): channel counts, names, TX/RX channel pages, subscriptions, flows
//   * CMC (UDP 8800): device advertisement
//   * ConMon information multicast (UDP 8700 -> 224.0.0.231:8702 / 224.0.0.233:8708): board/product info,
//     1 Hz heartbeat with clock / meter blocks, clock and network status
//   * flow control (UDP 4455, start code 0x1102): other devices request / stop / update unicast audio flows
//   * audio flows: 9-byte timestamped packets (1 byte, PTP seconds, sample-in-second) with 24-bit PCM, keepalives
//   * PTPv1 (ptp_v1.*) as the media clock, master or slave
// It is NOT a licensed Dante implementation and is not certified by Audinate; the parts of the protocol that are
// not publicly understood are listed in docs/HARDWARE.md part 8 together with what Dante Controller does with them.
//
// Threading: a network thread owns every socket and all protocol state; the audio engine (emulator thread) only
// exchanges PCM through two sample-time indexed ring buffers under a short lock, so timing of the protocol side does
// not depend on the emulated CPU.  The emulator reads a status snapshot for the Dante module model and the panel.
#pragma once
#include "../audio/dante_transport.h"
#include "../platform.h"
#include "dns_sd.h"
#include "ptp_v1.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rio {

struct DanteEndpointConfig {
    std::string ifAddr;                       // primary interface IPv4 (dotted), required
    std::string ifAddr2;                      // secondary interface (redundant mode), optional
    std::string name;                         // device name (<= 31 chars), what the Rio host programmed into the module
    std::string factoryName;                  // "Rio3224-D2-xxxxxx"
    std::string manufacturer = "Yamaha Corporation", modelName = "Rio3224-D2", boardName = "Brooklyn II";
    std::string vendor = "Audinate";              // ConMon message-family id in the 32-byte multicast header (the protocol's owner, not the product's maker)
    int64_t txAheadNs = 1000000;                  // audio timestamps run this far ahead of the moment the samples exist (transmit margin; costs the same on receive)
    std::string modelId = "_0000000000000000";   // Audinate model id as "mf"/"model" TXT entries carry it (from the module)
    uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };    // MAC reported for the device (the module's MAC)
    uint8_t deviceId[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint16_t processId = 0;
    uint8_t fwVersion[4] = { 4, 2, 0, 5 }, fwPatch = 3, hwVersion[4] = { 4, 1, 0, 3 }, productVersion[4] = { 1, 91, 0, 0 };   // Controller shows fw as major.minor.build(u16).patch
    uint32_t fs = 48000; int bits = 24;
    uint16_t arcPort = 4440, cmcPort = 8800, flowsPort = 4455, infoPort = 8700;
    bool allowAltPorts = true;                // fall back to other ports when the standard ones are taken (DNS-SD carries them)
    uint32_t rxLatencyNs = 5000000;           // our receive latency and the minimum we ask of receivers of our channels (the hardware unit offers 0.25-5 ms; 5 ms is what a software endpoint on a PC holds reliably)
    uint16_t fppMaxAdvertised = 32, fppMin = 2, maxChannelsPerFlow = 8;
    int maxTxFlows = 32, maxRxFlows = 32;
    bool redundant = false, preferredMaster = false, slaveOnly = false;
    int8_t ptpLogSyncInterval = -2;         // four Syncs per second, as Dante devices do
    uint8_t ptpStratum = 233;
    std::string tracePath;                    // pcap file of every datagram the endpoint sends / receives
    std::string persistPath;                  // names and subscriptions survive a restart here
    std::vector<std::string> txLabels, rxLabels;   // friendly channel names (default "01".."32" / "01".."24")
};

// what the emulator reads back (copied under the lock)
struct DanteNetStatus {
    bool running = false;
    bool linkPrimary = false, linkSecondary = false;
    uint32_t ip = 0, ip2 = 0; uint16_t arcPort = 0, cmcPort = 0, flowsPort = 0, infoPort = 0;
    std::string name, factoryName;
    uint32_t fs = 48000;
    ptp::State clockState = ptp::State::Off; bool clockLocked = false, clockMaster = false, preferred = false; std::string masterId; int64_t ptpOffsetNs = 0; double ptpFreqPpb = 0;
    std::vector<std::string> peers;           // other `_netaudio-arc` devices heard on the link
    struct Sub { std::string txChannel, txHost; uint32_t status = 0; uint64_t dropouts = 0; double dropoutMs = 0; };
    std::vector<Sub> subs;                    // per RX channel
    std::vector<std::string> txLabels, rxLabels;
    int txFlows = 0, rxFlows = 0;
    uint64_t controllerQueries = 0, lastControllerQueryMs = 0; std::string lastControllerQuery;
    uint64_t txPackets = 0, rxPackets = 0, rxLate = 0, resyncs = 0, errors = 0;
    bool nameChangedByController = false;     // set when a rename came from the network (the module model notifies the host)
    bool identify = false;                    // a controller asked the device to identify itself (cleared by clearNameChangedFlag())
    int bits = 24;
};

class DanteEndpoint : public IDanteTransport {
public:
    DanteEndpoint();
    ~DanteEndpoint() override;
    bool start(const DanteEndpointConfig& c, std::string& err);
    void stop();

    // IDanteTransport (called from the audio engine, emulator thread)
    const char* name() const override { return "dante-controller-endpoint"; }
    void setFormat(uint32_t fs, int blockFrames) override;
    void sendTx(const float* const* tx, int nch, size_t frames, uint64_t sampleTime) override;
    void receiveRx(float* const* rx, int nch, size_t frames, uint64_t sampleTime) override;
    const DanteStatus& status() const override { return st; }
    bool subscribe(int rxChannel, const std::string& device, int txChannel) override;   // device = Dante device name, txChannel 0-based (its factory name "NN")
    bool subscribeNamed(int rxChannel, const std::string& txChannel, const std::string& txHost);
    std::string subscription(int rxChannel) const override;
    void setLatencyFrames(uint32_t n) override;

    // emulator side
    DanteNetStatus snapshot();                 // thread-safe copy
    void clearNameChangedFlag();
    void setDeviceName(const std::string& n);  // from the Rio host (module programmed) - re-announces
    void setChannelLabel(bool tx, int index, const std::string& label);
    void setSampleRate(uint32_t fs);
    void setPreferredMaster(bool p);
    void setRedundant(bool r);
    void failLink(bool secondary, bool failed);   // test hook: pretend the cable was pulled
    std::string statusJson();                   // diagnostics page
    std::string logText(size_t maxLines = 200);
    std::string describe() const;
    int64_t mediaTimeNs();                      // the PTP-disciplined media clock (ns), for whoever paces the audio engine
    static int64_t ptpSampleCount(int64_t ptpNs, uint32_t fs);   // samples since the PTP epoch at fs
    std::function<void(const float* const* tx, int nch, size_t frames)> txTap;
    void setLog(bool verbose) { verboseLog = verbose; }

private:
    struct Chan { std::string factoryName, friendlyName; };
    struct TxFlow {
        bool active = false; uint8_t handle[6] = { 0, 0, 0, 0, 0, 0 }; int sock = -1; uint32_t dstIp = 0; uint16_t dstPort = 0;
        std::string rxHost, rxFlowName; std::vector<int> chan; int fpp = 16, bps = 3; uint64_t nextSample = 0; bool bootstrapped = false;
        int64_t lastKeepaliveMs = 0, createdMs = 0; uint64_t pkts = 0, keepalives = 0; std::vector<uint8_t> pkt; uint32_t epoch = 0;
    };
    struct ChanInfo {                           // a remote TX channel resolved through DNS-SD
        std::string host; uint32_t ip = 0; uint16_t flowsPort = 0; uint16_t id = 0, dbcp1 = 0x1102; int nchan = 8, bits = 24, fppMax = 32, fppMin = 2; uint32_t rate = 0; uint32_t latencyNs = 0;
        bool haveSrv = false, haveTxt = false, haveA = false; int64_t lastSeenMs = 0, queriedMs = 0; std::string target;
    };
    struct RxFlow {
        bool active = false; int sock = -1; uint16_t port = 0; std::string txHost; uint32_t txIp = 0; uint16_t txFlowsPort = 0, dbcp1 = 0x1102;
        uint8_t handle[6] = { 0, 0, 0, 0, 0, 0 }; bool haveHandle = false; std::vector<uint16_t> txIds; std::vector<std::vector<int>> local;   // slot -> RX channels fed
        int bps = 3, fpp = 16; uint32_t lastSrcIp = 0; uint16_t lastSrcPort = 0; int64_t lastPacketMs = 0, nextKeepaliveMs = 0, requestMs = 0, nextRequestMs = 0; int retries = 0;
        uint16_t reqSeq = 0; bool reqPending = false; uint8_t reqOp = 0; uint64_t pkts = 0, late = 0; int64_t maxLatencyNs = 0; int id = 0; std::string flowName;
        uint32_t txLatencyNs = 0;                 // the transmitter's minimum receive latency (its TXT latency_ns); ours is raised to it while the flow exists
        uint64_t otherDomain = 0; int64_t domainGapS = 0;   // packets whose timestamps are not in our clock domain (different grandmaster): counted, not played
        int64_t maxLateSamples = 0, minMarginSamples = 1 << 30;   // worst arrival after the play-out point / smallest margin before it (per heartbeat interval)
    };
    void updateEffectiveLatency();
    struct Peer { std::string name; int64_t lastSeenMs = 0; uint32_t ip = 0; };

    // state (protected by mu)
    mutable std::mutex mu;
    DanteEndpointConfig cfg; DanteStatus st; bool running = false, verboseLog = false;
    uint32_t ifIp = 0, ifIp2 = 0, netmask = 0, gateway = 0; uint16_t linkMbps = 1000; bool link1Failed = false, link2Failed = false;
    std::vector<Chan> txChans, rxChans;
    std::vector<DanteNetStatus::Sub> subs;
    struct RxQuality { uint32_t zeroRun = 0; uint64_t dropouts = 0, dropoutSamples = 0; bool inDropout = false; } rxq[kDanteRx];   // runs of silence on a subscribed, established channel (a lost / late packet, or genuine silence)
    std::vector<TxFlow> txFlows; std::vector<RxFlow> rxFlows; std::map<std::string, ChanInfo> chanInfo; std::map<std::string, Peer> peers;
    bool nameChangedByController = false, recordsDirty = true, persistDirty = false, identifyRequested = false;
    uint64_t controllerQueries = 0, lastControllerQueryMs = 0; std::string lastControllerQuery; uint64_t errors = 0, txPackets = 0, rxPackets = 0, rxLate = 0; std::atomic<uint64_t> resyncs{ 0 };
    std::deque<std::string> logLines;
    // audio rings and the media mapping (engine <-> network thread) live under their own short lock, so the audio
    // engine never waits for protocol work: audioMu is taken after mu when both are needed (network thread only)
    mutable std::mutex audioMu;
    static constexpr size_t kRing = 1 << 16;
    std::vector<float> txRing[kDanteTx], rxRing[kDanteRx];
    uint64_t txWritePos = 0; bool txStarted = false;
    // media mapping: engine sample s <-> PTP sample count S = sampleBase + s (S counts samples since the PTP epoch at fs)
    int64_t sampleBase = 0; bool anchored = false; uint32_t anchorEpoch = 0; uint64_t lastEngineSample = 0; int64_t lastEngineMonoNs = 0;
    uint32_t audioFs = 48000; int64_t audioAheadNs = 1000000; uint32_t audioLatencyFrames = 240;
    bool rxActive[kDanteRx] = {};             // RX channel has an established flow (silence on it is a dropout, not 'nothing subscribed')
    void updateRxActive();
    struct ClockParams { bool open = false; int64_t base = 0, ref = 0; double freq = 0; } aclk;   // copy of the PTP clock's parameters
    int64_t audioPtpNs(int64_t mono) const { return aclk.open ? mono + aclk.base + (int64_t)((double)(mono - aclk.ref) * aclk.freq) : mono + 1500000000LL * 1000000000LL; }
    // sockets (network thread)
    dnssd::Responder mdns; ptp::ClockV1 ptpClock;
    int arcSock = -1, cmcSock = -1, infoSock = -1, flowsSock = -1, ctlSock = -1;
    uint16_t infoSeq = 1, ctlSeq = 1; uint16_t rxPortBase = 14336;   // receive flow ports; moved up when Audinate software owns the usual range on this host
    std::thread thread; std::atomic<bool> quit{ false };
    FILE* trace = nullptr; std::mutex traceMu;
    int64_t nextHeartbeatNs = 0, nextBrowseNs = 0, nextHousekeepNs = 0, startNs = 0, lastRejoinNs = 0, lastInfoRejoinNs = 0; uint64_t rejoins = 0;
    void rejoinMulticast(const char* why);

    void run();
    static int64_t monoNs();
    int64_t nowMs() const;
    void logf(const char* fmt, ...);
    void tapPacket(const uint8_t* p, size_t n, uint32_t srcIp, uint16_t srcPort, uint32_t dstIp, uint16_t dstPort);
    bool openUdp(int& sock, uint16_t& port, const char* what, bool allowAlt, std::string& err);
    void sendTo(int sock, const uint8_t* p, size_t n, uint32_t ip, uint16_t port);
    // DNS-SD
    void rebuildRecords();
    std::string hostName() const { return cfg.name + ".local"; }
    void onMdnsAnswer(const dnssd::Answer& a, uint32_t fromIp);
    // ARC
    void pollArc();
    void handleArc(const uint8_t* p, size_t n, uint32_t fromIp, uint16_t fromPort);
    void arcRespond(uint16_t startCode, uint16_t seq, uint16_t op, uint16_t code, const std::vector<uint8_t>& content, uint32_t ip, uint16_t port);
    std::vector<uint8_t> arcChannelsAndFlows();
    std::vector<uint8_t> arcDeviceNames();
    std::vector<uint8_t> arcTxChannels(const uint8_t* req, size_t n, uint16_t& code);
    std::vector<uint8_t> arcTxFriendly(const uint8_t* req, size_t n, uint16_t& code);
    std::vector<uint8_t> arcRxChannels(const uint8_t* req, size_t n, uint16_t& code);
    std::vector<uint8_t> arcTxFlows(const uint8_t* req, size_t n, uint16_t& code);
    std::vector<uint8_t> arcRxFlows(const uint8_t* req, size_t n, uint16_t& code);
    void arcRenameTx(const uint8_t* p, size_t n);
    void arcRenameRx(const uint8_t* p, size_t n);
    void arcSetSubscriptions(const uint8_t* p, size_t n);
    // CMC / info
    void pollCmc();
    void pollInfo();
    void infoSend(uint32_t ip, uint16_t port, uint16_t startCode, const uint8_t opcode[8], const std::vector<uint8_t>& content);
    void sendBoardInfo(); void sendProductInfo(); void sendHeartbeat(); void sendClockStats(); void sendNetworkInfo();
    void sendSampleRateInfo(); void sendEncodingInfo();
    void handleSettings(const uint8_t* op, const uint8_t* c, size_t n, uint32_t fromIp);   // 0xFFFF "Audinate" control messages (sample rate, encoding, identify ...)
    void applySampleRate(uint32_t fs, const char* who);
    // flows control (TX side) and our TX flows
    void pollFlowsControl();
    void handleFlowRequest(const uint8_t* p, size_t n, uint32_t fromIp, uint16_t fromPort);
    int addTxFlow(uint32_t dstIp, uint16_t dstPort, const std::vector<int>& chan, int fpp, int bps, const std::string& rxHost, const std::string& rxFlowName, std::string& err);
    void removeTxFlow(int i, const char* why);
    void pumpTx(int64_t mono);
    // RX side
    void pollRx(int64_t mono);
    void resolveSubscriptions(int64_t mono);
    void requestRxFlow(RxFlow& f, int64_t mono);
    void rxControlResponse(const uint8_t* p, size_t n, uint32_t fromIp);
    void removeRxFlow(int i, const char* why, bool tellTx);
    void rebuildRxFlows();
    void onAudioPacket(RxFlow& f, const uint8_t* p, size_t n, int64_t mono);
    // misc
    void housekeeping(int64_t mono);
    void persist();
    void loadPersisted();
    std::string ipText(uint32_t ip) const;
};

} // namespace rio
