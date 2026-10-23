// IEEE 1588-2002 (PTP version 1) ordinary clock over UDP/IPv4 (event port 319, general port 320, 224.0.1.129),
// the clock protocol Dante devices use for their media clock.  One port, software time stamps (the time the
// datagram was handed to us / handed to the socket).  It runs the Best Master Clock decision from the Sync
// messages it hears (PTPv1 carries the grandmaster data set in every Sync), acts as master (Sync + Follow_Up at
// the configured interval, Delay_Resp to Delay_Req) when it wins, and otherwise as slave (offset from Sync /
// Follow_Up, path delay from Delay_Req / Delay_Resp, PI servo on a software clock).  The disciplined time is what
// the Dante endpoint stamps its audio packets with.  Written from the standard's message layouts; the few
// Dante-specific choices (sub-domain "_DFLT", stratum / identifier of a default clock) are noted where they matter.
#pragma once
#include "../platform.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ptp {

struct Config {
    uint32_t ifAddr = 0;                 // host order
    uint8_t uuid[6] = { 0, 0, 0, 0, 0, 0 };
    uint16_t portId = 1;
    bool preferred = false;              // "preferred master" flag in our grandmaster data set
    bool slaveOnly = false;              // never become master (stratum 255)
    // Defaults follow what Audinate's own implementation puts on the wire (Dante Virtual Soundcard, decoded with
    // tools/dante-pcap-ptp.py): Sync four times a second, stratum 233, variance 28768, assist + boundary flags.  A
    // PTPv1 receiver only qualifies a master it hears at least twice within four of its own Sync intervals, so a
    // 1 Hz master is invisible to a Dante device.
    int8_t logSyncInterval = -2;         // Sync every 2^n seconds when master (Dante: 0.25 s)
    uint8_t stratum = 233;               // Dante devices announce 233 (lower = better clock)
    int16_t variance = 28768;            // as Dante devices announce
    uint16_t flags = 0x000c;             // header flags: PTP_ASSIST (Follow_Up carries the precise time) + PTP_BOUNDARY_CLOCK, as Dante sets them
    const char* identifier = "DFLT";
};

enum class State { Off, Listening, Master, Slave };

struct Status {
    State state = State::Off;
    uint8_t masterUuid[6] = { 0, 0, 0, 0, 0, 0 };   // grandmaster we follow (or ourselves)
    uint8_t masterStratum = 0; bool masterPreferred = false;
    int64_t offsetNs = 0;                // last measured offset (ours - master's), slave only
    int64_t meanPathDelayNs = 0;
    double freqPpb = 0;                  // servo frequency correction
    bool locked = false;                 // |offset| small for a while
    uint64_t syncRx = 0, syncTx = 0, followUpRx = 0, followUpTx = 0, delayReqRx = 0, delayReqTx = 0, delayRespRx = 0, delayRespTx = 0, badPackets = 0, steps = 0;
    uint64_t lastSyncMs = 0;             // from the current master
    int foreignMasters = 0;
};

class ClockV1 {
public:
    bool open(const Config& c, std::string& err);
    void close();
    bool isOpen() const { return evSock >= 0; }
    void rejoin();                        // leave and re-join 224.0.1.129 on both sockets
    int64_t lastRxNs = 0;                 // when any PTP message was last received
    // call often (every millisecond or so) with the monotonic clock in ns
    void poll(int64_t monoNs);
    // PTP time (ns since the 1970 epoch, as the network sees it) for a monotonic time
    int64_t ptpNs(int64_t monoNs) const;
    int64_t monoForPtp(int64_t ptpNs) const;
    void params(int64_t& baseOut, int64_t& refOut, double& freqOut) const { baseOut = base; refOut = ref; freqOut = freq; }
    const Status& status() const { return st; }
    void setPreferred(bool p) { cfg.preferred = p; }
    std::function<void(const std::string&)> log;
    std::function<void(const uint8_t*, size_t, uint32_t srcIp, uint16_t srcPort, uint32_t dstIp, uint16_t dstPort)> tap;
    // for tests / diagnostics
    static constexpr const char* kGroup = "224.0.1.129";
    static std::string uuidText(const uint8_t* u);

private:
    struct Foreign {
        uint8_t uuid[6]; uint8_t stratum; bool preferred; int16_t variance; char identifier[4]; uint16_t portId;
        int8_t syncInterval; uint16_t stepsRemoved; uint32_t ip; int64_t lastSeenNs; int count;
    };
    Config cfg; Status st;
    int evSock = -1, genSock = -1;
    // software clock: ptp = mono + base + (mono - ref) * freq
    int64_t base = 0, ref = 0; double freq = 0;      // freq as a fraction (ppb / 1e9)
    // master
    uint16_t syncSeq = 0; int64_t nextSyncNs = 0;
    // slave
    std::vector<Foreign> foreign;
    int selected = -1;
    uint16_t pendingSyncSeq = 0; int64_t pendingSyncRxNs = 0; bool pendingSync = false;   // waiting for the Follow_Up
    uint16_t delaySeq = 0; int64_t delayReqTxNs = 0; bool delayPending = false; int64_t nextDelayReqNs = 0;
    int64_t lastMasterSyncNs = 0; int lockedCount = 0; int64_t lastDecisionNs = 0;
    double pathDelayFiltered = 0; bool havePathDelay = false;
    void send(int sock, const uint8_t* p, size_t n, uint32_t toIp);
    void sendSync(int64_t monoNs);
    void sendDelayReq(int64_t monoNs);
    void sendDelayResp(const uint8_t* req, int64_t rxNs, uint32_t fromIp);
    void onPacket(const uint8_t* p, size_t n, int64_t rxNs, uint32_t fromIp, bool event);
    void decide(int64_t monoNs);
    bool betterThanUs(const Foreign& f) const;
    static int compare(uint8_t preferredA, uint8_t stratumA, int16_t varA, const uint8_t* uuidA, uint8_t preferredB, uint8_t stratumB, int16_t varB, const uint8_t* uuidB);
    void servo(int64_t offsetNs, int64_t monoNs);
    void header(uint8_t* p, bool event, uint8_t control, uint16_t seq) const;
    void grandmasterSelf(uint8_t* gm) const;
};

} // namespace ptp
