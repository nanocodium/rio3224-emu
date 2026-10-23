#include "ptp_v1.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace ptp {

static const uint16_t kEventPort = 319, kGeneralPort = 320;
static void be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void be32(uint8_t* p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v & 0xFFFF); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void putTs(uint8_t* p, int64_t ptpNs) { int64_t s = ptpNs / 1000000000LL, ns = ptpNs % 1000000000LL; if (ns < 0) { ns += 1000000000LL; s--; } be32(p, (uint32_t)s); be32(p + 4, (uint32_t)ns); }
static int64_t getTs(const uint8_t* p) { return (int64_t)rd32(p) * 1000000000LL + (int64_t)(int32_t)rd32(p + 4); }

std::string ClockV1::uuidText(const uint8_t* u) { char b[32]; snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", u[0], u[1], u[2], u[3], u[4], u[5]); return b; }

static int openSock(uint16_t port, uint32_t ifAddr, std::string& err) {
    int s = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { err = std::string("socket: ") + sockError(); return -1; }
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, (sockaddr*)&sa, sizeof sa) < 0) { err = "bind " + std::to_string(port) + ": " + sockError(); sockClose(s); return -1; }
    ip_mreq mr{}; mr.imr_multiaddr.s_addr = inet_addr(ClockV1::kGroup); mr.imr_interface.s_addr = htonl(ifAddr);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) < 0) { err = std::string("join 224.0.1.129: ") + sockError(); sockClose(s); return -1; }
    in_addr ia{}; ia.s_addr = htonl(ifAddr); setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof ia);
    unsigned char ttl = 1; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
    int loop = 1; setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);
    int tos = 0xB8; setsockopt(s, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof tos);   // EF, as Dante marks its clock packets (best effort on Windows)
    sockNonBlocking(s);
    return s;
}

bool ClockV1::open(const Config& c, std::string& err) {
    platformInit(); cfg = c;
    evSock = openSock(kEventPort, cfg.ifAddr, err); if (evSock < 0) return false;
    genSock = openSock(kGeneralPort, cfg.ifAddr, err); if (genSock < 0) { sockClose(evSock); evSock = -1; return false; }
    // software clock starts at the system's idea of the epoch time
    int64_t mono = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t sys = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    base = sys - mono; ref = mono; freq = 0;
    st = Status{}; st.state = State::Listening; memcpy(st.masterUuid, cfg.uuid, 6);
    lastDecisionNs = mono; nextSyncNs = 0; nextDelayReqNs = mono + 1000000000LL;
    if (log) log("PTPv1 clock listening on " + std::string(kGroup) + " (uuid " + uuidText(cfg.uuid) + ", stratum " + std::to_string(cfg.slaveOnly ? 255 : cfg.stratum) + (cfg.preferred ? ", preferred" : "") + ")");
    return true;
}

void ClockV1::rejoin() {
    for (int s : { evSock, genSock }) {
        if (s < 0) continue;
        ip_mreq mr{}; mr.imr_multiaddr.s_addr = inet_addr(kGroup); mr.imr_interface.s_addr = htonl(cfg.ifAddr);
        setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mr, sizeof mr);
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr);
        in_addr ia{}; ia.s_addr = htonl(cfg.ifAddr); setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof ia);
    }
}

void ClockV1::close() { if (evSock >= 0) sockClose(evSock); if (genSock >= 0) sockClose(genSock); evSock = genSock = -1; st.state = State::Off; }

int64_t ClockV1::ptpNs(int64_t monoNs) const { return monoNs + base + (int64_t)((double)(monoNs - ref) * freq); }
int64_t ClockV1::monoForPtp(int64_t p) const { return (int64_t)((double)(p - base - ref) / (1.0 + freq)) + ref; }

void ClockV1::send(int sock, const uint8_t* p, size_t n, uint32_t toIp) {
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(sock == evSock ? kEventPort : kGeneralPort); to.sin_addr.s_addr = htonl(toIp);
    sendto(sock, (const char*)p, (int)n, 0, (sockaddr*)&to, sizeof to);
    if (tap) tap(p, n, cfg.ifAddr, sock == evSock ? kEventPort : kGeneralPort, toIp, sock == evSock ? kEventPort : kGeneralPort);
}

void ClockV1::header(uint8_t* p, bool event, uint8_t control, uint16_t seq) const {
    memset(p, 0, 40);
    be16(p, 1); be16(p + 2, 1);                       // versionPTP, versionNetwork
    memcpy(p + 4, "_DFLT", 5);                        // subdomain
    p[20] = event ? 1 : 2;                            // messageType
    p[21] = 1;                                        // sourceCommunicationTechnology = Ethernet
    memcpy(p + 22, cfg.uuid, 6); be16(p + 28, cfg.portId); be16(p + 30, seq);
    p[32] = control;                                  // 0 Sync 1 Delay_Req 2 Follow_Up 3 Delay_Resp
    be16(p + 34, cfg.flags);
}

void ClockV1::grandmasterSelf(uint8_t* gm) const {
    memset(gm, 0, 28);
    gm[1] = 1; memcpy(gm + 2, cfg.uuid, 6); be16(gm + 8, 0); be16(gm + 10, syncSeq);              // grandmaster port id 0, as Dante devices send it
    gm[15] = cfg.slaveOnly ? 255 : cfg.stratum; memcpy(gm + 16, cfg.identifier, 4); be16(gm + 22, (uint16_t)cfg.variance);
    gm[25] = cfg.preferred ? 1 : 0; gm[27] = (cfg.flags & 0x0004) ? 1 : 0;
}

void ClockV1::sendSync(int64_t monoNs) {
    uint8_t p[124]; header(p, true, 0, syncSeq);
    uint8_t* b = p + 40; memset(b, 0, 84);
    int64_t t1 = ptpNs(monoNs);
    putTs(b, t1);                                     // originTimestamp (approximate; the precise one follows)
    be16(b + 8, 0); be16(b + 10, 0);                  // epoch, currentUTCOffset
    grandmasterSelf(b + 12);
    b[43] = (uint8_t)cfg.logSyncInterval;
    be16(b + 46, (uint16_t)cfg.variance); be16(b + 50, 0);
    b[55] = cfg.slaveOnly ? 255 : cfg.stratum; memcpy(b + 56, cfg.identifier, 4);
    b[61] = 1; memcpy(b + 62, cfg.uuid, 6); be16(b + 70, 0);
    be16(b + 74, 0); be32(b + 76, 0); b[83] = 0;
    send(evSock, p, sizeof p, ntohl(inet_addr(kGroup)));
    int64_t t1precise = ptpNs(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    st.syncTx++;
    uint8_t f[52]; header(f, false, 2, syncSeq);
    memset(f + 40, 0, 12); be16(f + 42, syncSeq); putTs(f + 44, t1precise);   // associatedSequenceId, preciseOriginTimestamp
    send(genSock, f, sizeof f, ntohl(inet_addr(kGroup)));
    st.followUpTx++;
    syncSeq++;
}

void ClockV1::sendDelayReq(int64_t monoNs) {
    if (selected < 0) return;
    const Foreign& m = foreign[selected];
    uint8_t p[124]; header(p, true, 1, ++delaySeq);
    uint8_t* b = p + 40; memset(b, 0, 84);
    putTs(b, ptpNs(monoNs));
    // grandmaster data set as learnt from the master's Sync
    b[13] = 1; memcpy(b + 14, m.uuid, 6); be16(b + 20, m.portId); b[27] = m.stratum; memcpy(b + 28, m.identifier, 4); be16(b + 34, (uint16_t)m.variance); b[37] = m.preferred ? 1 : 0;
    b[43] = 0x7f;                                      // as Dante slaves send it
    be16(b + 46, (uint16_t)cfg.variance); be16(b + 50, (uint16_t)(m.stepsRemoved + 1));
    b[55] = cfg.slaveOnly ? 255 : cfg.stratum; memcpy(b + 56, cfg.identifier, 4);
    b[61] = 1; memcpy(b + 62, m.uuid, 6); be16(b + 70, m.portId);
    delayReqTxNs = ptpNs(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    send(evSock, p, sizeof p, ntohl(inet_addr(kGroup)));
    delayPending = true; st.delayReqTx++;
}

void ClockV1::sendDelayResp(const uint8_t* req, int64_t rxNs, uint32_t /*fromIp*/) {
    uint8_t p[60]; header(p, false, 3, rd16(req + 30));
    uint8_t* b = p + 40; memset(b, 0, 20);
    putTs(b, ptpNs(rxNs));                            // delayReceiptTimestamp
    b[9] = req[21]; memcpy(b + 10, req + 22, 6); be16(b + 16, rd16(req + 28)); be16(b + 18, rd16(req + 30));
    send(genSock, p, sizeof p, ntohl(inet_addr(kGroup)));
    st.delayRespTx++;
}

int ClockV1::compare(uint8_t prefA, uint8_t stratumA, int16_t varA, const uint8_t* uuidA, uint8_t prefB, uint8_t stratumB, int16_t varB, const uint8_t* uuidB) {
    // negative = A is the better clock.  IEEE 1588-2002 clause 7.6 data set comparison, simplified to the fields a Sync carries.
    if (prefA != prefB) return prefA ? -1 : 1;
    if (stratumA != stratumB) return stratumA < stratumB ? -1 : 1;
    if (varA != varB) return varA < varB ? -1 : 1;
    return memcmp(uuidA, uuidB, 6);
}

bool ClockV1::betterThanUs(const Foreign& f) const {
    uint8_t ours = cfg.slaveOnly ? 255 : cfg.stratum;
    return compare(f.preferred, f.stratum, f.variance, f.uuid, cfg.preferred, ours, cfg.variance, cfg.uuid) < 0;
}

void ClockV1::decide(int64_t monoNs) {
    // forget masters silent for 4 sync intervals (at least 4 s)
    for (size_t i = 0; i < foreign.size();) {
        int64_t timeout = (int64_t)4 * 1000000000LL * ((int64_t)1 << std::max<int>(0, foreign[i].syncInterval));
        if (monoNs - foreign[i].lastSeenNs > timeout) { if (log) log("PTP master " + uuidText(foreign[i].uuid) + " silent, dropped"); if ((int)i == selected) selected = -1; else if ((int)i < selected) selected--; foreign.erase(foreign.begin() + i); } else i++;
    }
    st.foreignMasters = (int)foreign.size();
    int best = -1;
    for (size_t i = 0; i < foreign.size(); i++) {
        if (foreign[i].count < 2) continue;                 // qualified after two Syncs
        if (best < 0 || compare(foreign[i].preferred, foreign[i].stratum, foreign[i].variance, foreign[i].uuid, foreign[best].preferred, foreign[best].stratum, foreign[best].variance, foreign[best].uuid) < 0) best = (int)i;
    }
    State want = State::Master;
    if (best >= 0 && betterThanUs(foreign[best])) want = State::Slave;
    else if (cfg.slaveOnly) want = State::Listening;
    else if (monoNs - lastDecisionNs < 3000000000LL && st.state == State::Listening) want = State::Listening;   // listen for existing masters first
    if (want == State::Slave && best != selected) {
        selected = best; pendingSync = false; delayPending = false; havePathDelay = false; lockedCount = 0; st.locked = false;
        memcpy(st.masterUuid, foreign[best].uuid, 6); st.masterStratum = foreign[best].stratum; st.masterPreferred = foreign[best].preferred;
        if (log) log("PTP: following master " + uuidText(foreign[best].uuid) + " (stratum " + std::to_string(foreign[best].stratum) + (foreign[best].preferred ? ", preferred" : "") + ")");
    }
    if (want != st.state) {
        if (want == State::Master) { selected = -1; memcpy(st.masterUuid, cfg.uuid, 6); st.masterStratum = cfg.stratum; st.masterPreferred = cfg.preferred; st.locked = true; st.offsetNs = 0; nextSyncNs = monoNs; if (log) log("PTP: no better clock on the network - acting as grandmaster"); }
        if (want == State::Listening) { st.locked = false; }
        st.state = want;
    }
}

void ClockV1::servo(int64_t offsetNs, int64_t monoNs) {
    st.offsetNs = offsetNs;
    if (std::llabs(offsetNs) > 100000000LL) {          // more than 100 ms: step the clock
        base -= offsetNs; st.steps++; lockedCount = 0; st.locked = false;
        if (log) log("PTP: clock stepped by " + std::to_string(-offsetNs / 1000000) + " ms");
        return;
    }
    // PI servo; the proportional part is applied as a phase correction, the integral part adjusts the frequency
    const double kp = 0.3, ki = 0.05;
    base -= (int64_t)(kp * (double)offsetNs);
    // re-base the frequency term so that changing freq does not jump the time
    int64_t nowPtp = ptpNs(monoNs); double newFreq = freq - ki * (double)offsetNs / 1e9; if (newFreq > 500e-6) newFreq = 500e-6; if (newFreq < -500e-6) newFreq = -500e-6;
    freq = newFreq; ref = monoNs; base = nowPtp - monoNs;
    st.freqPpb = freq * 1e9;
    if (std::llabs(offsetNs) < 1000000LL) { if (lockedCount < 1000) lockedCount++; } else lockedCount = 0;
    bool lockedNow = lockedCount >= 3;
    if (lockedNow != st.locked) { st.locked = lockedNow; if (log) log(lockedNow ? "PTP: locked to master (offset below 1 ms)" : "PTP: lock lost"); }
}

void ClockV1::onPacket(const uint8_t* p, size_t n, int64_t rxNs, uint32_t fromIp, bool event) {
    if (n < 40 || rd16(p) != 1) { st.badPackets++; return; }
    if (memcmp(p + 22, cfg.uuid, 6) == 0) return;        // our own multicast looped back
    uint8_t control = p[32]; uint16_t seq = rd16(p + 30);
    if (control == 0 && n >= 124) {                       // Sync: carries the grandmaster data set -> BMC input
        st.syncRx++;
        const uint8_t* b = p + 40;
        Foreign* f = nullptr; for (auto& x : foreign) if (memcmp(x.uuid, p + 22, 6) == 0) f = &x;
        if (!f) { foreign.push_back(Foreign{}); f = &foreign.back(); memcpy(f->uuid, p + 22, 6); f->count = 0; if (log) log("PTP: master candidate " + uuidText(f->uuid) + " seen"); }
        f->portId = rd16(p + 28); f->stratum = b[27]; memcpy(f->identifier, b + 28, 4); f->variance = (int16_t)rd16(b + 34); f->preferred = b[37] != 0;
        f->syncInterval = (int8_t)b[43]; f->stepsRemoved = rd16(b + 50); f->ip = fromIp; f->lastSeenNs = rxNs; f->count++;
        // the grandmaster the sender reports may differ from the sender (boundary clocks); for the decision we use the GM fields above
        if (selected >= 0 && &foreign[selected] == f) {
            pendingSync = true; pendingSyncSeq = seq; pendingSyncRxNs = rxNs; lastMasterSyncNs = rxNs; st.lastSyncMs = (uint64_t)(rxNs / 1000000);
        }
    } else if (control == 2 && n >= 52) {                 // Follow_Up
        st.followUpRx++;
        if (selected >= 0 && pendingSync && memcmp(p + 22, foreign[selected].uuid, 6) == 0 && rd16(p + 42) == pendingSyncSeq) {
            int64_t t1 = getTs(p + 44), t2 = ptpNs(pendingSyncRxNs); pendingSync = false;
            int64_t offset = (t2 - t1) - (havePathDelay ? (int64_t)pathDelayFiltered : 0);
            servo(offset, pendingSyncRxNs);
        }
    } else if (control == 1 && n >= 124) {                // Delay_Req
        st.delayReqRx++;
        if (st.state == State::Master) sendDelayResp(p, rxNs, fromIp);
    } else if (control == 3 && n >= 60) {                 // Delay_Resp
        st.delayRespRx++;
        const uint8_t* b = p + 40;
        if (delayPending && memcmp(b + 10, cfg.uuid, 6) == 0 && rd16(b + 18) == delaySeq && selected >= 0 && memcmp(p + 22, foreign[selected].uuid, 6) == 0) {
            delayPending = false;
            int64_t t4 = getTs(b), t3 = delayReqTxNs;
            // t4 - t3 = one-way delay - offset (slave time = master time + offset), so delay = (t4 - t3) + offset
            int64_t d = (t4 - t3) + st.offsetNs; if (d < 0) d = 0; if (d > 100000000LL) d = 100000000LL;
            double v = (double)d;
            pathDelayFiltered = havePathDelay ? pathDelayFiltered * 0.9 + v * 0.1 : v; havePathDelay = true;
            st.meanPathDelayNs = (int64_t)pathDelayFiltered;
        }
    } else st.badPackets++;
    (void)event;
}

void ClockV1::poll(int64_t monoNs) {
    if (evSock < 0) return;
    uint8_t buf[512];
    for (int pass = 0; pass < 2; pass++) {
        int s = pass == 0 ? evSock : genSock;
        for (int k = 0; k < 32; k++) {
            sockaddr_in from{}; socklen_t fl = sizeof from;
            ssize_t r = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
            if (r <= 0) break;
            int64_t rx = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); lastRxNs = rx;
            if (tap) tap(buf, (size_t)r, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), ntohl(inet_addr(kGroup)), pass == 0 ? kEventPort : kGeneralPort);
            onPacket(buf, (size_t)r, rx, ntohl(from.sin_addr.s_addr), pass == 0);
        }
    }
    decide(monoNs);
    if (st.state == State::Master && monoNs >= nextSyncNs) {
        sendSync(monoNs);
        int64_t interval = cfg.logSyncInterval >= 0 ? (int64_t)(1000000000LL << (int)cfg.logSyncInterval) : (int64_t)(1000000000LL >> (int)(-cfg.logSyncInterval));
        nextSyncNs = (nextSyncNs == 0 ? monoNs : nextSyncNs) + interval; if (nextSyncNs < monoNs) nextSyncNs = monoNs + interval;
    }
    if (st.state == State::Slave && monoNs >= nextDelayReqNs) {
        sendDelayReq(monoNs);
        static std::mt19937 rng{ 12345 }; std::uniform_int_distribution<int> jitter(0, 1500);
        nextDelayReqNs = monoNs + 1000000000LL + (int64_t)jitter(rng) * 1000000LL;   // 1-2.5 s, like a Dante slave
    }
    if (st.state == State::Slave && lastMasterSyncNs && monoNs - lastMasterSyncNs > 5000000000LL && st.locked) { st.locked = false; if (log) log("PTP: no Sync from the master for 5 s"); }
}

} // namespace ptp
