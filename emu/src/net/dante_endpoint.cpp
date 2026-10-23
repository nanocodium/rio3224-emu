#include "dante_endpoint.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>



#include "bytes_be.h"
#ifdef _WIN32
  #include <timeapi.h>
  #include <avrt.h>
  typedef SOCKET SOCKET_T;
#else
  typedef int SOCKET_T;
#endif

namespace rio {

using namespace rio::be;
static void be16(std::vector<uint8_t>& v, uint16_t x) { put16v(v, x); }
static void be32(std::vector<uint8_t>& v, uint32_t x) { put32v(v, x); }
static const uint16_t kArcHeader = 10;
static const uint16_t kDbcp1 = 0x1102;
static const uint32_t kInfoGroup = 0xE00000E7, kHeartbeatGroup = 0xE00000E9;   // 224.0.0.231 / 224.0.0.233
static const uint16_t kInfoDstPort = 8702, kHeartbeatDstPort = 8708;
static const int kRxPortBase = 14336;

DanteEndpoint::DanteEndpoint() { for (auto& r : txRing) r.assign(kRing, 0.f); for (auto& r : rxRing) r.assign(kRing, 0.f); }
DanteEndpoint::~DanteEndpoint() { stop(); }

int64_t DanteEndpoint::monoNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int64_t DanteEndpoint::nowMs() const { return monoNs() / 1000000; }
std::string DanteEndpoint::ipText(uint32_t ip) const { char b[24]; snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }

void DanteEndpoint::logf(const char* fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    int64_t t = nowMs() - (startNs ? startNs / 1000000 : 0);
    char line[600]; snprintf(line, sizeof line, "%8.3f %s", t / 1000.0, b);
    fprintf(stderr, "[dante-net] %s\n", b);
    logLines.emplace_back(line); if (logLines.size() > 400) logLines.pop_front();
}

// ---- pcap tracing (LINKTYPE_IPV4 = 228: raw IPv4 packets, Wireshark decodes the UDP payloads with its Dante dissectors) ----
void DanteEndpoint::tapPacket(const uint8_t* p, size_t n, uint32_t srcIp, uint16_t srcPort, uint32_t dstIp, uint16_t dstPort) {
    if (!trace) return;
    std::lock_guard<std::mutex> g(traceMu);
    uint8_t h[28]; size_t total = 28 + n;
    h[0] = 0x45; h[1] = 0; put16(h + 2, (uint16_t)total); put16(h + 4, 0); put16(h + 6, 0x4000); h[8] = 64; h[9] = 17; put16(h + 10, 0); put32(h + 12, srcIp); put32(h + 16, dstIp);
    uint32_t sum = 0; for (int i = 0; i < 20; i += 2) sum += rd16(h + i); while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16); put16(h + 10, (uint16_t)~sum);
    put16(h + 20, srcPort); put16(h + 22, dstPort); put16(h + 24, (uint16_t)(8 + n)); put16(h + 26, 0);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    uint32_t rec[4] = { (uint32_t)(us / 1000000), (uint32_t)(us % 1000000), (uint32_t)total, (uint32_t)total };
    fwrite(rec, 1, sizeof rec, trace); fwrite(h, 1, sizeof h, trace); fwrite(p, 1, n, trace);
}

bool DanteEndpoint::openUdp(int& sock, uint16_t& port, const char* what, bool allowAlt, std::string& err) {
    uint16_t wanted = port;
    for (int attempt = 0; attempt < 16; attempt++) {
        sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) { err = std::string("socket: ") + sockError(); return false; }
#ifdef _WIN32
        int one = 1; setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);   // detect ports owned by other programs (e.g. Dante Controller's own services)
#endif
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(ifIp);
        if (::bind(sock, (sockaddr*)&sa, sizeof sa) == 0) {
            in_addr ia{}; ia.s_addr = htonl(ifIp); setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof ia);
            unsigned char ttl = 16; setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
            int rcv = 1 << 20; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof rcv);
            int snd = 1 << 20; setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof snd);
            sockNonBlocking(sock);
            if (port != wanted) logf("%s: standard port %u is taken by another program on this host, using %u (Dante Controller learns it through DNS-SD / CMC)", what, wanted, port);
            return true;
        }
        sockClose(sock); sock = -1;
        if (!allowAlt) break;
        port = (uint16_t)(wanted + 10 * (attempt + 1));
    }
    err = std::string(what) + ": cannot bind UDP port " + std::to_string(wanted) + " on " + ipText(ifIp) + " (" + sockError() + ")";
    return false;
}

void DanteEndpoint::sendTo(int sock, const uint8_t* p, size_t n, uint32_t ip, uint16_t port) {
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(port); to.sin_addr.s_addr = htonl(ip);
    sendto(sock, (const char*)p, (int)n, 0, (sockaddr*)&to, sizeof to);
    uint16_t sp = 0; sockaddr_in me{}; socklen_t ml = sizeof me; if (getsockname(sock, (sockaddr*)&me, &ml) == 0) sp = ntohs(me.sin_port);
    tapPacket(p, n, ifIp, sp, ip, port);
}

// ---- start / stop -------------------------------------------------------------------------------------------------
static uint32_t parseIp(const std::string& s) { in_addr a{}; if (s.empty()) return 0; a.s_addr = inet_addr(s.c_str()); return a.s_addr == INADDR_NONE ? 0 : ntohl(a.s_addr); }

bool DanteEndpoint::start(const DanteEndpointConfig& c, std::string& err) {
    platformInit();
    std::lock_guard<std::mutex> g(mu);
    cfg = c; startNs = monoNs();
    if (cfg.name.size() > 31) cfg.name.resize(31);
    ifIp = parseIp(cfg.ifAddr); if (!ifIp) { err = "the endpoint needs the IPv4 address of the interface Dante Controller is on (--dante-ctl A.B.C.D)"; return false; }
    ifIp2 = parseIp(cfg.ifAddr2);
    // interface details for the network-info page: netmask / gateway from the address (a class C guess when not told)
    netmask = 0xFFFFFF00; gateway = (ifIp & netmask) | 1;
    if (!cfg.tracePath.empty()) {
        trace = fopen(cfg.tracePath.c_str(), "wb");
        if (trace) { uint32_t hdr[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 228 }; fwrite(hdr, 1, sizeof hdr, trace); }
        else logf("cannot write the packet trace %s", cfg.tracePath.c_str());
    }
    txChans.resize(kDanteTx); rxChans.resize(kDanteRx);
    for (int i = 0; i < kDanteTx; i++) { char b[8]; snprintf(b, sizeof b, "%02d", i + 1); txChans[i].factoryName = b; txChans[i].friendlyName = i < (int)cfg.txLabels.size() && !cfg.txLabels[i].empty() ? cfg.txLabels[i] : b; }
    for (int i = 0; i < kDanteRx; i++) { char b[8]; snprintf(b, sizeof b, "%02d", i + 1); rxChans[i].factoryName = b; rxChans[i].friendlyName = i < (int)cfg.rxLabels.size() && !cfg.rxLabels[i].empty() ? cfg.rxLabels[i] : b; }
    subs.assign(kDanteRx, {});
    txFlows.assign(cfg.maxTxFlows, {}); rxFlows.assign(cfg.maxRxFlows, {});
    st.fs = cfg.fs; st.latencyFrames = (uint32_t)((uint64_t)cfg.rxLatencyNs * cfg.fs / 1000000000ULL); st.redundant = cfg.redundant; st.primaryLink = true; st.secondaryLink = ifIp2 != 0;
    { std::lock_guard<std::mutex> a(audioMu); audioFs = cfg.fs; audioAheadNs = cfg.txAheadNs; audioLatencyFrames = st.latencyFrames; }
    loadPersisted();
    st.fs = cfg.fs; st.latencyFrames = (uint32_t)((uint64_t)cfg.rxLatencyNs * cfg.fs / 1000000000ULL); { std::lock_guard<std::mutex> a(audioMu); audioFs = cfg.fs; audioLatencyFrames = st.latencyFrames; }
    // sockets.  Audinate software on this host (Dante Controller's ConMon, Dante Virtual Soundcard) binds the standard
    // ports on the wildcard address; a device on the same address must leave them alone or it steals their traffic
    // (Windows delivers to the most specific binding).  DNS-SD and the CMC advertisement carry our actual ports.
    if (cfg.allowAltPorts) {
        auto taken = [](uint16_t port) { int s = (int)socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return false;
#ifdef _WIN32
            int one = 1; setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
#endif
            sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(INADDR_ANY); bool t = ::bind(s, (sockaddr*)&sa, sizeof sa) < 0; sockClose(s); return t; };
        if (taken(8700) || taken(8800) || taken(4440) || taken(4455)) {
            logf("Audinate software is running on this host (standard Dante ports in use): using the alternate port set so it keeps working");
            if (cfg.arcPort == 4440) cfg.arcPort = 4450; if (cfg.cmcPort == 8800) cfg.cmcPort = 8810; if (cfg.infoPort == 8700) cfg.infoPort = 8710; if (cfg.flowsPort == 4455) cfg.flowsPort = 4465;
            rxPortBase = 15000;
        }
    }
    if (!openUdp(arcSock, cfg.arcPort, "ARC", cfg.allowAltPorts, err)) return false;
    if (!openUdp(cmcSock, cfg.cmcPort, "CMC", cfg.allowAltPorts, err)) return false;
    if (!openUdp(infoSock, cfg.infoPort, "info", cfg.allowAltPorts, err)) return false;
    if (!openUdp(flowsSock, cfg.flowsPort, "flow control", cfg.allowAltPorts, err)) return false;
    uint16_t zero = 0; if (!openUdp(ctlSock, zero, "flow client", false, err)) return false;
    {   // the info socket also listens to the two multicast groups (other devices' notifications, controller requests)
        for (uint32_t grp : { kInfoGroup, kHeartbeatGroup }) { ip_mreq mr{}; mr.imr_multiaddr.s_addr = htonl(grp); mr.imr_interface.s_addr = htonl(ifIp); setsockopt(infoSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr); }
    }
    if (!mdns.open(ifIp, err)) return false;
    mdns.onAnswer = [this](const dnssd::Answer& a, uint32_t from) { onMdnsAnswer(a, from); };
    mdns.tap = [this](const uint8_t* p, size_t n, uint32_t a, uint16_t b, uint32_t cc, uint16_t d) { tapPacket(p, n, a, b, cc, d); };
    ptp::Config pc; pc.ifAddr = ifIp; memcpy(pc.uuid, cfg.mac, 6); pc.preferred = cfg.preferredMaster; pc.slaveOnly = cfg.slaveOnly; pc.logSyncInterval = cfg.ptpLogSyncInterval; pc.stratum = cfg.ptpStratum;
    ptpClock.log = [this](const std::string& s) { logf("%s", s.c_str()); };
    ptpClock.tap = [this](const uint8_t* p, size_t n, uint32_t a, uint16_t b, uint32_t cc, uint16_t d) { tapPacket(p, n, a, b, cc, d); };
    if (!ptpClock.open(pc, err)) { logf("PTP: %s - running without a network clock (audio timestamps from the system clock)", err.c_str()); err.clear(); }
    rebuildRecords(); mdns.announce();
    running = true; st.clockMaster = true;
    logf("endpoint '%s' (%s) on %s: ARC %u, CMC %u, info %u, flows %u, %u Hz, %d TX / %d RX channels, latency %.2f ms%s", cfg.name.c_str(), cfg.factoryName.c_str(), ipText(ifIp).c_str(), cfg.arcPort, cfg.cmcPort, cfg.infoPort, cfg.flowsPort, cfg.fs, kDanteTx, kDanteRx, cfg.rxLatencyNs / 1e6, cfg.redundant ? ", redundant" : ", switched");
    quit = false; thread = std::thread([this] { run(); });
    return true;
}

void DanteEndpoint::stop() {
    if (!thread.joinable()) { return; }
    quit = true; thread.join();
    std::lock_guard<std::mutex> g(mu);
    for (int i = 0; i < (int)rxFlows.size(); i++) if (rxFlows[i].active) removeRxFlow(i, "shutdown", true);
    for (int i = 0; i < (int)txFlows.size(); i++) if (txFlows[i].active) removeTxFlow(i, "shutdown");
    mdns.goodbye(); mdns.close(); ptpClock.close();
    for (int* s : { &arcSock, &cmcSock, &infoSock, &flowsSock, &ctlSock }) if (*s >= 0) { sockClose(*s); *s = -1; }
    if (persistDirty) persist();
    if (trace) { fclose(trace); trace = nullptr; }
    running = false;
}

std::string DanteEndpoint::describe() const {
    char b[320]; snprintf(b, sizeof b, "Controller-interoperable Dante endpoint '%s' at %s (ARC %u, CMC %u, info %u, flows %u, PTPv1, %u Hz)", cfg.name.c_str(), ipText(ifIp).c_str(), cfg.arcPort, cfg.cmcPort, cfg.infoPort, cfg.flowsPort, cfg.fs); return b;
}

// ---- network thread -------------------------------------------------------------------------------------------------
void DanteEndpoint::run() {
#ifdef _WIN32
    timeBeginPeriod(1);
    DWORD mmTask = 0; HANDLE mmHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmTask);   // same class as the WASAPI threads
    if (!mmHandle) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    sendBoardInfo(); sendProductInfo();
    int64_t last = monoNs(); nextHeartbeatNs = last + 1000000000LL; nextBrowseNs = last + 500000000LL; nextHousekeepNs = last + 250000000LL;
    while (!quit) {
        // wait for traffic on any socket, at most 1 ms (the TX flows are paced from the ring positions)
        fd_set rf; FD_ZERO(&rf); int maxfd = 0;
        auto add = [&](int s) { if (s >= 0) { FD_SET((SOCKET_T)s, &rf); if (s > maxfd) maxfd = s; } };
        { std::lock_guard<std::mutex> g(mu); add(arcSock); add(cmcSock); add(infoSock); add(flowsSock); add(ctlSock); for (auto& f : rxFlows) if (f.active) add(f.sock); for (auto& f : txFlows) if (f.active) add(f.sock); }
        timeval tv{ 0, 1000 };
        select(maxfd + 1, &rf, nullptr, nullptr, &tv);
        int64_t mono = monoNs();
        std::lock_guard<std::mutex> g(mu);
        ptpClock.poll(mono);
        { std::lock_guard<std::mutex> a(audioMu); aclk.open = ptpClock.isOpen(); ptpClock.params(aclk.base, aclk.ref, aclk.freq); }
        mdns.poll((uint64_t)(mono / 1000000));
        pollArc(); pollCmc(); pollInfo(); pollFlowsControl();
        pollRx(mono);
        pumpTx(mono);
        if (mono >= nextHeartbeatNs) { nextHeartbeatNs += 1000000000LL; if (nextHeartbeatNs < mono) nextHeartbeatNs = mono + 1000000000LL; sendHeartbeat(); }
        if (mono >= nextBrowseNs) { nextBrowseNs = mono + 30000000000LL; mdns.query("_netaudio-arc._udp.local", dnssd::TYPE_PTR); }
        if (mono >= nextHousekeepNs) { nextHousekeepNs = mono + 250000000LL; housekeeping(mono); }
        if (recordsDirty) { recordsDirty = false; rebuildRecords(); mdns.announce(); }
    }
#ifdef _WIN32
    if (mmHandle) AvRevertMmThreadCharacteristics(mmHandle);
    timeEndPeriod(1);
#endif
}

void DanteEndpoint::rejoinMulticast(const char* why) {
    rejoins++;
    if (rejoins <= 3 || (rejoins % 50) == 0) logf("multicast reception %s: leaving and re-joining the groups (#%llu) - Windows drops memberships when a network interface appears or changes", why, (unsigned long long)rejoins);
    mdns.rejoin(); ptpClock.rejoin();
    for (uint32_t grp : { kInfoGroup, kHeartbeatGroup }) { ip_mreq mr{}; mr.imr_multiaddr.s_addr = htonl(grp); mr.imr_interface.s_addr = htonl(ifIp); setsockopt(infoSock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mr, sizeof mr); setsockopt(infoSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr); }
    mdns.announce();
}

void DanteEndpoint::housekeeping(int64_t mono) {
    int64_t ms = mono / 1000000;
    // multicast watchdog: a network with a controller on it is never silent on mDNS for long; when nothing arrives any
    // more (and the sockets are fine for unicast) the stack has lost our group memberships - re-join them
    {
        int64_t mdnsSilentMs = mdns.lastRxMs ? ms - (int64_t)mdns.lastRxMs : ms - startNs / 1000000;
        int64_t ptpSilentMs = ptpClock.lastRxNs ? (mono - ptpClock.lastRxNs) / 1000000 : ms - startNs / 1000000;
        if (mdnsSilentMs > 10000 && ptpSilentMs > 10000 && mono - lastRejoinNs > 10000000000LL) { lastRejoinNs = mono; rejoinMulticast("silent for 10 s"); }
    }
    // link state: the interface address must still exist (cable pulled / adapter down)
    st.primaryLink = !link1Failed; st.secondaryLink = ifIp2 != 0 && !link2Failed; st.primaryActive = st.primaryLink || !st.secondaryLink;
    const auto& ps = ptpClock.status(); st.clockMaster = ps.state != ptp::State::Slave;
    for (auto it = peers.begin(); it != peers.end();) { if (ms - it->second.lastSeenMs > 120000) { logf("device '%s' no longer announced", it->first.c_str()); it = peers.erase(it); } else ++it; }
    for (int i = 0; i < (int)txFlows.size(); i++) {
        auto& f = txFlows[i]; if (!f.active) continue;
        int64_t ref = f.keepalives ? f.lastKeepaliveMs : f.createdMs;
        if (ms - ref > (f.keepalives ? 8000 : 30000)) removeTxFlow(i, f.keepalives ? "no keepalive from the receiver for 8 s" : "receiver never sent a keepalive");
    }
    resolveSubscriptions(mono);
    if (persistDirty) { persistDirty = false; persist(); }
}

// ---- DNS-SD ------------------------------------------------------------------------------------------------------------
void DanteEndpoint::rebuildRecords() {
    std::vector<dnssd::Record> recs; const std::string host = hostName();
    auto ptr = [&](const std::string& svc, const std::string& inst) { dnssd::Record r; r.name = svc; r.type = dnssd::TYPE_PTR; r.flush = false; r.target = inst; recs.push_back(r); };
    auto srv = [&](const std::string& inst, uint16_t port) { dnssd::Record r; r.name = inst; r.type = dnssd::TYPE_SRV; r.port = port; r.target = host; recs.push_back(r); };
    auto txt = [&](const std::string& inst, std::vector<std::string> t) { dnssd::Record r; r.name = inst; r.type = dnssd::TYPE_TXT; r.txt = std::move(t); recs.push_back(r); };
    { dnssd::Record a; a.name = host; a.type = dnssd::TYPE_A; a.ttl = 120; a.a = ifIp; recs.push_back(a); }
    for (const char* s : { "_netaudio-arc._udp.local", "_netaudio-cmc._udp.local", "_netaudio-chan._udp.local" }) ptr("_services._dns-sd._udp.local", s);
    char idhex[20]; snprintf(idhex, sizeof idhex, "%02x%02x%02x%02x%02x%02x%02x%02x", cfg.deviceId[0], cfg.deviceId[1], cfg.deviceId[2], cfg.deviceId[3], cfg.deviceId[4], cfg.deviceId[5], cfg.deviceId[6], cfg.deviceId[7]);
    std::string arc = cfg.name + "._netaudio-arc._udp.local", cmc = cfg.name + "._netaudio-cmc._udp.local";
    ptr("_netaudio-arc._udp.local", arc); srv(arc, cfg.arcPort);
    txt(arc, { "arcp_vers=2.7.41", "arcp_min=0.2.4", "router_vers=4.0.2", "router_info=" + cfg.boardName, "mf=" + cfg.manufacturer, "model=" + cfg.modelId });
    ptr("_netaudio-cmc._udp.local", cmc); srv(cmc, cfg.cmcPort);
    txt(cmc, { std::string("id=") + idhex, "process=" + std::to_string(cfg.processId), "cmcp_vers=1.2.0", "cmcp_min=1.0.0", "server_vers=4.0.2", "channels=0x6000004d", "mf=" + cfg.manufacturer, "model=" + cfg.modelId, "", "" });
    char pcm[32]; snprintf(pcm, sizeof pcm, "pcm=%d e", cfg.bits / 8);
    for (int i = 0; i < kDanteTx; i++) {
        auto chan = [&](const std::string& chname, bool isDefault) {
            std::string inst = chname + "@" + cfg.name + "._netaudio-chan._udp.local";
            ptr("_netaudio-chan._udp.local", inst); srv(inst, cfg.flowsPort);
            std::vector<std::string> t = { "txtvers=2", "dbcp1=0x1102", "dbcp=0x1004", "id=" + std::to_string(i + 1), "rate=" + std::to_string(cfg.fs), pcm, "enc=" + std::to_string(cfg.bits), "en=" + std::to_string(cfg.bits), "latency_ns=" + std::to_string(cfg.rxLatencyNs), "fpp=" + std::to_string(cfg.fppMaxAdvertised) + "," + std::to_string(cfg.fppMin), "nchan=" + std::to_string(cfg.maxChannelsPerFlow) };
            if (isDefault) t.push_back("default");
            txt(inst, t);
        };
        chan(txChans[i].factoryName, true);
        if (txChans[i].friendlyName != txChans[i].factoryName) chan(txChans[i].friendlyName, false);
    }
    mdns.setRecords(std::move(recs));
}

void DanteEndpoint::onMdnsAnswer(const dnssd::Answer& a, uint32_t fromIp) {
    int64_t ms = nowMs();
    // other devices: instance names under _netaudio-arc
    if (a.type == dnssd::TYPE_PTR && dnssd::nameEq(a.name, "_netaudio-arc._udp.local")) {
        std::string inst = a.target; size_t p = inst.find("._netaudio-arc"); if (p != std::string::npos) inst.resize(p);
        if (!inst.empty() && !dnssd::nameEq(inst, cfg.name)) {
            if (a.ttl == 0) { if (peers.erase(inst)) logf("device '%s' left (goodbye)", inst.c_str()); }
            else { auto& pr = peers[inst]; if (pr.name.empty()) logf("device '%s' announced from %s", inst.c_str(), ipText(fromIp).c_str()); pr.name = inst; pr.lastSeenMs = ms; pr.ip = fromIp; }
        }
    }
    // transmit channels we are resolving: "<chan>@<host>._netaudio-chan._udp.local"
    size_t at = a.name.find("._netaudio-chan._udp.local");
    if (at != std::string::npos && (a.type == dnssd::TYPE_SRV || a.type == dnssd::TYPE_TXT)) {
        std::string key = a.name.substr(0, at);
        for (auto& [k, ci] : chanInfo) if (dnssd::nameEq(k, key)) {
            // the device's address is where its answer came from: a Dante device answers for its own records, and a
            // host name such as "<pc-name>.local" may also be answered by the PC's own mDNS stack with a different meaning
            if (a.type == dnssd::TYPE_SRV) { ci.flowsPort = a.port; ci.target = a.target; ci.haveSrv = true; ci.ip = fromIp; ci.haveA = true; }
            else {
                for (auto& t : a.txt) {
                    size_t e = t.find('='); if (e == std::string::npos) continue; std::string kk = t.substr(0, e), vv = t.substr(e + 1);
                    auto num = [&](const std::string& s) -> long { return s.rfind("0x", 0) == 0 ? strtol(s.c_str() + 2, nullptr, 16) : strtol(s.c_str(), nullptr, 10); };
                    if (kk == "id") ci.id = (uint16_t)num(vv); else if (kk == "dbcp1") ci.dbcp1 = (uint16_t)num(vv); else if (kk == "nchan") ci.nchan = (int)num(vv);
                    else if (kk == "enc" || kk == "en") ci.bits = (int)num(vv); else if (kk == "rate") ci.rate = (uint32_t)num(vv); else if (kk == "latency_ns") ci.latencyNs = (uint32_t)num(vv);
                    else if (kk == "fpp") { size_t c = vv.find(','); ci.fppMax = atoi(vv.c_str()); if (c != std::string::npos) ci.fppMin = atoi(vv.c_str() + c + 1); }
                }
                ci.haveTxt = true;
            }
            ci.lastSeenMs = ms;
        }
    }
    if (a.type == dnssd::TYPE_A) {
        for (auto& [k, ci] : chanInfo) if (ci.haveSrv && dnssd::nameEq(ci.target, a.name) && ci.ip == fromIp) ci.lastSeenMs = ms;   // confirms what the SRV source already told us
    }
}

// ---- ARC server ------------------------------------------------------------------------------------------------------
void DanteEndpoint::pollArc() {
    uint8_t buf[2048];
    for (int k = 0; k < 32; k++) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(arcSock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        tapPacket(buf, (size_t)r, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), ifIp, cfg.arcPort);
        handleArc(buf, (size_t)r, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
    }
}

void DanteEndpoint::arcRespond(uint16_t startCode, uint16_t seq, uint16_t op, uint16_t code, const std::vector<uint8_t>& content, uint32_t ip, uint16_t port) {
    std::vector<uint8_t> p(kArcHeader + content.size());
    put16(p.data(), startCode); put16(p.data() + 2, (uint16_t)p.size()); put16(p.data() + 4, seq); put16(p.data() + 6, op); put16(p.data() + 8, code);
    memcpy(p.data() + kArcHeader, content.data(), content.size());
    sendTo(arcSock, p.data(), p.size(), ip, port);
}

void DanteEndpoint::handleArc(const uint8_t* p, size_t n, uint32_t fromIp, uint16_t fromPort) {
    if (n < kArcHeader) { errors++; return; }
    uint16_t start = rd16(p), seq = rd16(p + 4), op = rd16(p + 6), code = rd16(p + 8);
    if (code != 0) return;                                    // a response, not for us
    controllerQueries++; lastControllerQueryMs = (uint64_t)nowMs(); char q[64]; snprintf(q, sizeof q, "ARC 0x%04x from %s", op, ipText(fromIp).c_str()); lastControllerQuery = q;
    if (verboseLog) logf("ARC request 0x%04x (start %04x seq %u) from %s:%u: %s", op, start, seq, ipText(fromIp).c_str(), fromPort, hex(p + kArcHeader, n - kArcHeader).c_str());
    const uint8_t* c = p + kArcHeader; size_t cn = n - kArcHeader; uint16_t rc = 1; std::vector<uint8_t> out;
    switch (op) {
    case 0x1000: out = arcChannelsAndFlows(); break;
    case 0x1002: out.insert(out.end(), cfg.name.begin(), cfg.name.end()); out.push_back(0); break;
    case 0x1003: out = arcDeviceNames(); break;
    case 0x2000: out = arcTxChannels(c, cn, rc); break;
    case 0x2010: out = arcTxFriendly(c, cn, rc); break;
    case 0x3000: out = arcRxChannels(c, cn, rc); break;
    case 0x2200: out = arcTxFlows(c, cn, rc); break;
    case 0x3200: out = arcRxFlows(c, cn, rc); break;
    case 0x2204: {                                            // TX flow labels: a page of {flow id, label offset} for the active flows (asked by Controller after 0x2200)
        std::vector<uint8_t> v(2 + 16 * 4 + kArcHeader, 0); size_t count = 0;
        for (size_t i = 0; i < txFlows.size() && count < 16; i++) { if (!txFlows[i].active) continue; uint16_t off = strz(v, std::to_string(i + 1) + "_" + std::to_string(cfg.processId)); size_t at = kArcHeader + 2 + count * 4; set16(v, at, (uint16_t)(i + 1)); set16(v, at + 2, off); count++; }
        v[kArcHeader] = 16; v[kArcHeader + 1] = (uint8_t)count; v.erase(v.begin(), v.begin() + kArcHeader); out = v; break;
    }
    case 0x2013: arcRenameTx(c, cn); out = { 0, 0 }; break;
    case 0x3001: arcRenameRx(c, cn); break;
    case 0x3010: arcSetSubscriptions(c, cn); break;
    case 0x3014: if (cn >= 6) { int ch = rd16(c + 4); if (ch >= 1 && ch <= kDanteRx) { logf("controller: unsubscribe RX %d", ch); subs[ch - 1] = {}; rebuildRxFlows(); persistDirty = true; } } break;
    case 0x1100: out.assign(110, 0); break;                   // device settings (contents not publicly understood; Controller accepts zeros)
    case 0x1101: {                                            // device settings write: the receive-latency form is known (22-byte preamble, latency ns twice)
        static const uint8_t pre[22] = { 0x05, 0x04, 0x82, 0x05, 0x00, 0x20, 0x02, 0x11, 0x00, 0x04, 0x83, 0x01, 0x00, 0x24, 0x03, 0x10, 0x00, 0x04, 0x83, 0x02, 0x83, 0x06 };
        if (cn >= 32 && memcmp(c + 2, pre, 22) == 0) {
            uint32_t ns = rd32(c + 24);
            if (ns >= 250000 && ns <= 20000000) { logf("controller set the receive latency: %.2f -> %.2f ms%s", cfg.rxLatencyNs / 1e6, ns / 1e6, ns < 2000000 ? " (below what a software receiver can hold reliably; expect late packets)" : ""); cfg.rxLatencyNs = ns; st.latencyFrames = (uint32_t)((uint64_t)ns * cfg.fs / 1000000000ULL); { std::lock_guard<std::mutex> a(audioMu); audioLatencyFrames = st.latencyFrames; } recordsDirty = true; persistDirty = true; }
            else { logf("controller asked for a latency of %u ns: out of range", ns); rc = 0x22; }
        } else { logf("device settings write from %s not understood: %s", ipText(fromIp).c_str(), hex(c, cn).c_str()); errors++; rc = 0x22; }
        break;
    }
    case 0x1102: out.assign(94, 0); break;                    // property directory
    case 0x3300: out = { 0x38, 0x00, 0x38, 0xfd, 0x38, 0xfe, 0x38, 0xff }; break;   // receiver port ranges (14336..); without it Controller reports a clock-domain mismatch
    case 0x2320: rc = 0x30; break;
    case 0x1001: {                                            // device rename from the controller: payload observed as [u16 0][name\0] - decode defensively
        std::string nm; for (size_t i = 0; i + 1 < cn; i++) { if (c[i] >= 0x20 && c[i] < 0x7F) { nm = readz(c, cn, i); break; } }
        if (!nm.empty() && nm.size() <= 31) { logf("controller renamed the device: '%s' -> '%s'", cfg.name.c_str(), nm.c_str()); mdns.goodbye(); cfg.name = nm; nameChangedByController = true; recordsDirty = true; persistDirty = true; out = { 0, 0 }; }
        else if (nm.empty()) { logf("controller reset the device name to the factory name"); mdns.goodbye(); cfg.name = cfg.factoryName; nameChangedByController = true; recordsDirty = true; persistDirty = true; out = { 0, 0 }; }
        else rc = 0x22;
        break;
    }
    default:
        logf("ARC request 0x%04x from %s not understood (start %04x): %s", op, ipText(fromIp).c_str(), start, hex(c, cn).c_str());
        errors++;
        return;                                               // like other unofficial implementations: no answer for what we cannot serve correctly
    }
    arcRespond(start, seq, op, rc, out, fromIp, fromPort);
}

std::vector<uint8_t> DanteEndpoint::arcChannelsAndFlows() {
    std::vector<uint8_t> v; v.push_back(0); v.push_back(0x10);   // flags2 bit4: TX channels may be renamed (no multicast TX flows offered)
    be16(v, kDanteTx); be16(v, kDanteRx); be16(v, 4); be16(v, cfg.maxChannelsPerFlow); be16(v, 8); be16(v, (uint16_t)cfg.maxTxFlows); be16(v, (uint16_t)cfg.maxRxFlows); be16(v, kDanteTx + kDanteRx); be16(v, 1); be16(v, 1);
    for (int i = 0; i < 6; i++) be16(v, 0);
    return v;
}

std::vector<uint8_t> DanteEndpoint::arcDeviceNames() {
    std::vector<uint8_t> v(44, 0);
    uint16_t friendly = (uint16_t)(v.size() + kArcHeader); v.insert(v.end(), cfg.name.begin(), cfg.name.end()); v.push_back(0);
    uint16_t factory = (uint16_t)(v.size() + kArcHeader); v.insert(v.end(), cfg.factoryName.begin(), cfg.factoryName.end()); v.push_back(0);
    uint16_t board = (uint16_t)(v.size() + kArcHeader); v.insert(v.end(), cfg.boardName.begin(), cfg.boardName.end()); v.push_back(0);
    uint16_t rev = (uint16_t)(v.size() + kArcHeader); const char* r = ":705"; v.insert(v.end(), r, r + 5);
    set16(v, 6, board); set16(v, 8, rev); set16(v, 12, friendly); set16(v, 14, factory); set16(v, 16, friendly); set16(v, 30, 0x2729); set16(v, 34, 0x1102);
    return v;
}

// paginated item lists: [u8 space][u8 count][items...][descriptors / strings]; offsets are absolute (header included)
static size_t startIndex(const uint8_t* req, size_t n) { if (n < 4) return 0; uint16_t s = rd16(req + 2); return s ? s - 1 : 0; }
static void commonDescriptor(std::vector<uint8_t>& v, uint32_t fs, int bits) { be32(v, fs); v.push_back(1); v.push_back(1); be16(v, (uint16_t)bits); be16(v, 0x400); be16(v, (uint16_t)bits); be16(v, (uint16_t)bits); be16(v, 0x0e); }

std::vector<uint8_t> DanteEndpoint::arcTxChannels(const uint8_t* req, size_t n, uint16_t& code) {
    size_t first = startIndex(req, n); const size_t itemSize = 8, space = 16;
    std::vector<uint8_t> v(2 + space * itemSize + kArcHeader, 0); size_t count = 0;
    uint16_t common = (uint16_t)v.size(); commonDescriptor(v, cfg.fs, cfg.bits);
    for (size_t i = first; i < (size_t)kDanteTx && count < space; i++, count++) {
        uint16_t nameOff = strz(v, txChans[i].factoryName);
        size_t at = kArcHeader + 2 + count * itemSize; set16(v, at, (uint16_t)(i + 1)); set16(v, at + 2, 7); set16(v, at + 4, common); set16(v, at + 6, nameOff);
    }
    v[kArcHeader] = space; v[kArcHeader + 1] = (uint8_t)count; code = first + count < (size_t)kDanteTx ? 0x8112 : 1;
    v.erase(v.begin(), v.begin() + kArcHeader); return v;
}

std::vector<uint8_t> DanteEndpoint::arcTxFriendly(const uint8_t* req, size_t n, uint16_t& code) {
    size_t first = startIndex(req, n); const size_t itemSize = 6, space = 16;
    std::vector<uint8_t> v(2 + space * itemSize + kArcHeader, 0); size_t count = 0; be32(v, 0);
    for (size_t i = first; i < (size_t)kDanteTx && count < space; i++, count++) {
        uint16_t nameOff = strz(v, txChans[i].friendlyName);
        size_t at = kArcHeader + 2 + count * itemSize; set16(v, at, (uint16_t)(i + 1)); set16(v, at + 2, (uint16_t)(i + 1)); set16(v, at + 4, nameOff);
    }
    v[kArcHeader] = space; v[kArcHeader + 1] = (uint8_t)count; code = first + count < (size_t)kDanteTx ? 0x8112 : 1;
    v.erase(v.begin(), v.begin() + kArcHeader); return v;
}

std::vector<uint8_t> DanteEndpoint::arcRxChannels(const uint8_t* req, size_t n, uint16_t& code) {
    size_t first = startIndex(req, n); const size_t itemSize = 20, space = 16;
    std::vector<uint8_t> v(2 + space * itemSize + kArcHeader, 0); size_t count = 0;
    uint16_t common = (uint16_t)v.size(); commonDescriptor(v, cfg.fs, cfg.bits);
    for (size_t i = first; i < (size_t)kDanteRx && count < space; i++, count++) {
        const auto& s = subs[i]; uint16_t txName = 0, txHost = 0;
        if (!s.txChannel.empty()) { txName = strz(v, s.txChannel); txHost = strz(v, s.txHost); }
        uint16_t friendly = strz(v, rxChans[i].friendlyName);
        size_t at = kArcHeader + 2 + count * itemSize;
        set16(v, at, (uint16_t)(i + 1)); set16(v, at + 2, 6); set16(v, at + 4, common); set16(v, at + 6, txName); set16(v, at + 8, txHost); set16(v, at + 10, friendly);
        uint32_t status = s.txChannel.empty() ? 0 : s.status; v[at + 12] = status >> 24; v[at + 13] = (status >> 16) & 255; v[at + 14] = (status >> 8) & 255; v[at + 15] = status & 255;
    }
    v[kArcHeader] = space; v[kArcHeader + 1] = (uint8_t)count; code = first + count < (size_t)kDanteRx ? 0x8112 : 1;
    v.erase(v.begin(), v.begin() + kArcHeader); return v;
}

std::vector<uint8_t> DanteEndpoint::arcTxFlows(const uint8_t* req, size_t n, uint16_t& code) {
    size_t first = startIndex(req, n); const size_t space = 16;
    std::vector<uint8_t> v(2 + space * 2 + kArcHeader, 0); size_t count = 0, index = 0;
    for (size_t i = 0; i < txFlows.size() && count < space; i++) {
        const auto& f = txFlows[i]; if (!f.active) continue; if (index++ < first) continue;
        std::string flowName = std::to_string(i + 1) + "_" + std::to_string(cfg.processId);
        uint16_t local = strz(v, flowName), rhost = f.rxHost.empty() ? 0 : strz(v, f.rxHost), rflow = f.rxFlowName.empty() ? 0 : strz(v, f.rxFlowName);
        align(v, 4); uint16_t sockOff = (uint16_t)v.size(); be16(v, 0x8002); be16(v, f.dstPort); be32(v, f.dstIp);
        uint16_t names = (uint16_t)v.size(); be16(v, 0xa00); be16(v, 1); be16(v, rhost); be16(v, rflow); be16(v, 0x10); be16(v, local); for (int k = 0; k < 8; k++) v.push_back(0);
        uint16_t main = (uint16_t)v.size(); be16(v, (uint16_t)(i + 1)); be16(v, 0x11); be32(v, cfg.fs); be16(v, 0); be16(v, (uint16_t)(f.bps * 8)); be16(v, 1); be16(v, (uint16_t)f.chan.size()); be16(v, sockOff);
        for (int ch : f.chan) be16(v, (uint16_t)(ch >= 0 ? ch + 1 : 0));
        be16(v, names);
        set16(v, kArcHeader + 2 + count * 2, main); count++;
    }
    v[kArcHeader] = space; v[kArcHeader + 1] = (uint8_t)count; code = index > first + count ? 0x8112 : 1;
    v.erase(v.begin(), v.begin() + kArcHeader); return v;
}

std::vector<uint8_t> DanteEndpoint::arcRxFlows(const uint8_t* req, size_t n, uint16_t& code) {
    size_t first = startIndex(req, n); const size_t space = 16;
    std::vector<uint8_t> v(2 + space * 2 + kArcHeader, 0); size_t count = 0, index = 0;
    for (size_t i = 0; i < rxFlows.size() && count < space; i++) {
        const auto& f = rxFlows[i]; if (!f.active) continue; if (index++ < first) continue;
        align(v, 4); uint16_t sockOff = (uint16_t)v.size(); be16(v, 0x8002); be16(v, f.port); be32(v, ifIp);
        uint16_t d2 = (uint16_t)v.size(); be16(v, 9); be16(v, 1); be16(v, 0x800); be16(v, 0); be32(v, cfg.rxLatencyNs); be32(v, 0);
        size_t slots = f.local.size(); int words = 2;   // 24 RX channels -> 2 x 16-bit mask words
        std::vector<uint16_t> maskOff;
        for (size_t s = 0; s < slots; s++) { maskOff.push_back((uint16_t)v.size()); uint32_t m = 0; for (int ch : f.local[s]) if (ch >= 0 && ch < 32) m |= 1u << ch; be16(v, (uint16_t)(m & 0xFFFF)); be16(v, (uint16_t)(m >> 16)); }
        align(v, 4); uint16_t main = (uint16_t)v.size();
        be16(v, (uint16_t)(i + 1)); be16(v, 1); be32(v, cfg.fs); be16(v, 0); be16(v, (uint16_t)(f.bps * 8)); be16(v, 1); be16(v, (uint16_t)slots); be16(v, (uint16_t)words); be16(v, sockOff);
        for (auto m : maskOff) be16(v, m);
        be16(v, d2);
        set16(v, kArcHeader + 2 + count * 2, main); count++;
    }
    v[kArcHeader] = space; v[kArcHeader + 1] = (uint8_t)count; code = index > first + count ? 0x8112 : 1;
    v.erase(v.begin(), v.begin() + kArcHeader); return v;
}

void DanteEndpoint::arcRenameTx(const uint8_t* p, size_t n) {
    if (n < 2) return; size_t items = p[1];
    for (size_t i = 0; i < items && 2 + (i + 1) * 6 <= n; i++) {
        const uint8_t* it = p + 2 + i * 6; uint16_t ch = rd16(it + 2), off = rd16(it + 4);
        if (ch < 1 || ch > kDanteTx || off < kArcHeader) continue;
        std::string nm = readz(p, n, off - kArcHeader); if (nm.empty()) nm = txChans[ch - 1].factoryName;
        logf("controller renamed TX %u: '%s' -> '%s'", ch, txChans[ch - 1].friendlyName.c_str(), nm.c_str());
        txChans[ch - 1].friendlyName = nm; recordsDirty = true; persistDirty = true;
    }
}

void DanteEndpoint::arcRenameRx(const uint8_t* p, size_t n) {
    if (n < 2) return; size_t items = p[1];
    for (size_t i = 0; i < items && 2 + (i + 1) * 4 <= n; i++) {
        const uint8_t* it = p + 2 + i * 4; uint16_t ch = rd16(it), off = rd16(it + 2);
        if (ch < 1 || ch > kDanteRx || off < kArcHeader) continue;
        std::string nm = readz(p, n, off - kArcHeader); if (nm.empty()) nm = rxChans[ch - 1].factoryName;
        logf("controller renamed RX %u: '%s' -> '%s'", ch, rxChans[ch - 1].friendlyName.c_str(), nm.c_str());
        rxChans[ch - 1].friendlyName = nm; persistDirty = true;
    }
}

void DanteEndpoint::arcSetSubscriptions(const uint8_t* p, size_t n) {
    if (n < 2) return; size_t items = p[1]; bool changed = false;
    for (size_t i = 0; i < items && 2 + (i + 1) * 6 <= n; i++) {
        const uint8_t* it = p + 2 + i * 6; uint16_t ch = rd16(it), nameOff = rd16(it + 2), hostOff = rd16(it + 4);
        if (ch < 1 || ch > kDanteRx) continue;
        if (nameOff >= kArcHeader && hostOff >= kArcHeader) {
            std::string nm = readz(p, n, nameOff - kArcHeader), host = readz(p, n, hostOff - kArcHeader);
            if (nm.empty() || host.empty()) continue;
            logf("controller: subscribe RX %u <- %s@%s", ch, nm.c_str(), host.c_str());
            subs[ch - 1].txChannel = nm; subs[ch - 1].txHost = host; subs[ch - 1].status = 1; changed = true;
        } else { logf("controller: unsubscribe RX %u", ch); subs[ch - 1] = {}; changed = true; }
    }
    if (changed) { rebuildRxFlows(); persistDirty = true; }
}

// ---- CMC -----------------------------------------------------------------------------------------------------------------
void DanteEndpoint::pollCmc() {
    uint8_t buf[1024];
    for (int k = 0; k < 16; k++) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(cmcSock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        uint32_t ip = ntohl(from.sin_addr.s_addr); uint16_t port = ntohs(from.sin_port);
        tapPacket(buf, (size_t)r, ip, port, ifIp, cfg.cmcPort);
        if (r < kArcHeader) continue;
        uint16_t start = rd16(buf), seq = rd16(buf + 4), op = rd16(buf + 6), code = rd16(buf + 8);
        if (code != 0) continue;
        controllerQueries++; lastControllerQueryMs = (uint64_t)nowMs(); char q[64]; snprintf(q, sizeof q, "CMC 0x%04x from %s", op, ipText(ip).c_str()); lastControllerQuery = q;
        if (op == 0x1001) {
            std::vector<uint8_t> v; be16(v, cfg.processId); v.insert(v.end(), cfg.deviceId, cfg.deviceId + 8); be16(v, 1); be16(v, 0); be32(v, ifIp); be16(v, cfg.infoPort); be16(v, 0);
            std::vector<uint8_t> p(kArcHeader + v.size()); put16(p.data(), start); put16(p.data() + 2, (uint16_t)p.size()); put16(p.data() + 4, seq); put16(p.data() + 6, op); put16(p.data() + 8, 1); memcpy(p.data() + kArcHeader, v.data(), v.size());
            sendTo(cmcSock, p.data(), p.size(), ip, port);
            if (verboseLog) logf("CMC advertisement sent to %s:%u", ipText(ip).c_str(), port);
        } else { logf("CMC request 0x%04x from %s not understood: %s", op, ipText(ip).c_str(), hex(buf + kArcHeader, (size_t)r - kArcHeader).c_str()); errors++; }
    }
}

// ---- ConMon information multicast ------------------------------------------------------------------------------------------
void DanteEndpoint::infoSend(uint32_t ip, uint16_t port, uint16_t startCode, const uint8_t opcode[8], const std::vector<uint8_t>& content) {
    std::vector<uint8_t> p(32 + content.size(), 0);
    put16(p.data(), startCode); put16(p.data() + 2, (uint16_t)p.size()); put16(p.data() + 4, infoSeq++); put16(p.data() + 6, cfg.processId);
    memcpy(p.data() + 8, cfg.deviceId, 8); memset(p.data() + 16, ' ', 8); memcpy(p.data() + 16, cfg.vendor.data(), std::min<size_t>(8, cfg.vendor.size())); memcpy(p.data() + 24, opcode, 8);
    memcpy(p.data() + 32, content.data(), content.size());
    sendTo(infoSock, p.data(), p.size(), ip, port);
}

void DanteEndpoint::sendBoardInfo() {
    std::vector<uint8_t> c(200, 0);
    memcpy(c.data(), cfg.fwVersion, 4); c[0x23] = cfg.fwPatch; memcpy(c.data() + 4, cfg.hwVersion, 4); c[0x27] = 1; c[0x28] = 1;   // Controller shows major.minor.(u16 build).patch
    c[0x14] = 0; c[0x15] = 0; c[0x16] = 0x10; c[0x17] = 0;        // feature flags: has a manufacturer name; no lock / AES67 / reboot / factory-reset offered
    c[0xbb] = 0x1f;
    memcpy(c.data() + 12, cfg.boardName.data(), std::min<size_t>(8, cfg.boardName.size())); memcpy(c.data() + 0x38, cfg.boardName.data(), std::min<size_t>(16, cfg.boardName.size()));
    static const uint8_t op[8] = { 0x07, 0x2a, 0x00, 0x60, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, op, c);
}
void DanteEndpoint::sendProductInfo() {
    std::vector<uint8_t> c(336, 0);
    memcpy(c.data(), cfg.manufacturer.data(), std::min<size_t>(8, cfg.manufacturer.size())); memcpy(c.data() + 8, cfg.boardName.data(), std::min<size_t>(8, cfg.boardName.size()));
    memcpy(c.data() + 0x2c, cfg.manufacturer.data(), std::min<size_t>(16, cfg.manufacturer.size())); memcpy(c.data() + 0xac, cfg.modelName.data(), std::min<size_t>(16, cfg.modelName.size()));
    memcpy(c.data() + 0x18, cfg.productVersion, 4); memcpy(c.data() + 0x1c, cfg.productVersion, 4);   // software / firmware version words (Controller's Product Version)
    static const uint8_t op[8] = { 0x07, 0x2a, 0x00, 0xc0, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, op, c);
}
void DanteEndpoint::sendHeartbeat() {
    std::vector<uint8_t> c; uint16_t ctr = infoSeq;
    const auto& ps = ptpClock.status(); int32_t ppb = (int32_t)std::lround(ps.freqPpb);
    be16(c, 16); be16(c, 0x8001); be16(c, 4); be16(c, 4); be16(c, ctr); be16(c, 0); be32(c, (uint32_t)ppb);
    // meters: 32 TX + 24 RX peak bytes, 0 = full scale, 1 unit = 0.25 dB below (what controllers plot)
    { std::vector<uint8_t> tx(kDanteTx, 255), rx(kDanteRx, 255);
      std::lock_guard<std::mutex> a(audioMu);
      for (int i = 0; i < kDanteTx; i++) { float pk = 0; for (size_t k = 0; k < 256; k++) pk = std::max(pk, std::fabs(txRing[i][(txWritePos - 1 - k) & (kRing - 1)])); tx[i] = pk > 0 ? (uint8_t)std::clamp(std::lround(-40.0 * std::log10(pk)), 0L, 255L) : 255; }
      for (int i = 0; i < kDanteRx; i++) { float pk = 0; for (size_t k = 0; k < 256; k++) pk = std::max(pk, std::fabs(rxRing[i][(lastEngineSample - audioLatencyFrames - k) & (kRing - 1)])); rx[i] = pk > 0 ? (uint8_t)std::clamp(std::lround(-40.0 * std::log10(pk)), 0L, 255L) : 255; }
      size_t total = tx.size() + rx.size(), len = 24 + total; while (len % 4) len++;
      size_t end = c.size() + len; be16(c, (uint16_t)(24 + total)); be16(c, 0x8002); be16(c, 4); be16(c, (uint16_t)(12 + total)); be16(c, ctr); be16(c, 0); be16(c, (uint16_t)tx.size()); be16(c, 0); be16(c, (uint16_t)rx.size()); be16(c, 0); be16(c, 24); be16(c, 0);
      c.insert(c.end(), tx.begin(), tx.end()); c.insert(c.end(), rx.begin(), rx.end()); while (c.size() < end) c.push_back(0); }
    {   // receive flow latencies (max over the last second)
        uint16_t flows = (uint16_t)rxFlows.size(); be16(c, (uint16_t)(24 + flows * 4)); be16(c, 0x8003); be16(c, 4); be16(c, (uint16_t)(12 + flows * 4)); be16(c, ctr); be16(c, 0); be16(c, flows); be16(c, 0); be16(c, 24); be16(c, 0); be32(c, cfg.fs);
        for (auto& f : rxFlows) { uint32_t lat = f.active ? (uint32_t)std::clamp<int64_t>((int64_t)f.maxLatencyNs * cfg.fs / 1000000000LL, 0, 0x7fffffff) : 0; be32(c, lat); if (f.active) f.maxLatencyNs = 0; }
    }
    static const uint8_t op[8] = { 0, 8, 0, 1, 0x10, 0, 0, 0 }; infoSend(kHeartbeatGroup, kHeartbeatDstPort, 0xfffe, op, c);
}
void DanteEndpoint::sendClockStats() {
    const auto& ps = ptpClock.status(); std::vector<uint8_t> c;
    bool slave = ps.state == ptp::State::Slave; bool locked = ps.state == ptp::State::Master || ps.locked;
    c = { 0x00, 0x03, 0x00, (uint8_t)(locked ? 0x03 : 0x01), 0x00, 0x00, 0x00, 0x9f };
    be32(c, (uint32_t)(int32_t)std::lround(ps.freqPpb)); c.insert(c.end(), cfg.mac, cfg.mac + 6); be16(c, 0);
    uint8_t master[8] = { 0 }; const uint8_t* mu6 = slave ? ps.masterUuid : cfg.mac; memcpy(master, mu6, 3); master[3] = 0xff; master[4] = 0xfe; memcpy(master + 5, mu6 + 3, 3);
    c.insert(c.end(), master, master + 8); c.insert(c.end(), master, master + 8); c.insert(c.end(), 76, 0);
    static const uint8_t op[8] = { 0x07, 0x2a, 0x00, 0x20, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, op, c);
}
void DanteEndpoint::sendNetworkInfo() {
    std::vector<uint8_t> c = { 0, 1, 0, 0, 0, 0 }; be16(c, linkMbps); be16(c, 1); c.insert(c.end(), cfg.mac, cfg.mac + 6); be32(c, ifIp); be32(c, netmask); be32(c, gateway); be32(c, gateway);
    static const uint8_t tail[32] = { 0x00, 0x18, 0x00, 0x30 }; c.insert(c.end(), tail, tail + 32);
    static const uint8_t op[8] = { 0x07, 0x2a, 0x00, 0x11, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, op, c);
}

void DanteEndpoint::pollInfo() {
    uint8_t buf[2048];
    for (int k = 0; k < 32; k++) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(infoSock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        uint32_t ip = ntohl(from.sin_addr.s_addr); uint16_t port = ntohs(from.sin_port);
        tapPacket(buf, (size_t)r, ip, port, ifIp, cfg.infoPort);
        if (r < 32) continue;
        if (memcmp(buf + 8, cfg.deviceId, 8) == 0) continue;               // our own multicast looped back
        const uint8_t* op = buf + 24;
        if (op[0] != 0x07) continue;                                         // other devices' notifications (board info etc.)
        controllerQueries++; lastControllerQueryMs = (uint64_t)nowMs(); char q[64]; snprintf(q, sizeof q, "info %02x%02x%02x%02x from %s", op[0], op[1], op[2], op[3], ipText(ip).c_str()); lastControllerQuery = q;
        if (rd16(buf) == 0xFFFF && (op[1] == 0x3a || op[1] == 0x27 || op[1] == 0x3e || op[1] == 0x34)) { handleSettings(op, buf + 32, (size_t)r - 32, ip); continue; }
        if (op[2] == 0 && op[3] == 0x61) sendBoardInfo();
        else if (op[2] == 0 && op[3] == 0xc1) sendProductInfo();
        else if (op[2] == 0 && op[3] == 0x21) sendClockStats();
        else if (op[2] == 0 && op[3] == 0x13) sendNetworkInfo();
        else if (op[2] == 0 && op[3] == 0x77) { static const uint8_t o[8] = { 0x07, 0x2a, 0x00, 0x78, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, o, { 0, 0, 0, 3, 0, 0, 0, 0 }); }
        else { logf("info request %s from %s not understood: %s", hex(op, 8).c_str(), ipText(ip).c_str(), hex(buf + 32, (size_t)r - 32).c_str()); errors++; }
    }
}

// ---- controller settings messages (start code 0xFFFF, "Audinate" family, request value 100) ------------------------------
void DanteEndpoint::applySampleRate(uint32_t fs, const char* who) {
    if (fs != 44100 && fs != 48000 && fs != 88200 && fs != 96000) { logf("%s asked for %u Hz: not a rate this unit supports (44.1 / 48 / 88.2 / 96 kHz)", who, fs); errors++; return; }
    if (fs == cfg.fs) return;
    logf("%s set the sample rate: %u -> %u Hz (the unit's DIO follows; receivers on other rates stop)", who, cfg.fs, fs);
    cfg.fs = fs; st.fs = fs; st.latencyFrames = (uint32_t)((uint64_t)cfg.rxLatencyNs * fs / 1000000000ULL);
    { std::lock_guard<std::mutex> a(audioMu); audioFs = fs; audioLatencyFrames = st.latencyFrames; anchored = false; anchorEpoch++; }
    for (int i = 0; i < (int)rxFlows.size(); i++) if (rxFlows[i].active) removeRxFlow(i, "sample rate changed", true);
    for (int i = 0; i < (int)txFlows.size(); i++) if (txFlows[i].active) removeTxFlow(i, "sample rate changed");
    for (auto& ci : chanInfo) ci.second.haveTxt = false;        // re-read the transmitters' rates
    recordsDirty = true; persistDirty = true;
}

void DanteEndpoint::sendSampleRateInfo() {
    std::vector<uint8_t> c; be16(c, 0x18); be16(c, 4); be32(c, cfg.fs); be32(c, cfg.fs); be16(c, 2); be16(c, 0);
    for (uint32_t r : { 44100u, 48000u, 88200u, 96000u }) be32(c, r);
    static const uint8_t op[8] = { 0x07, 0x2a, 0x00, 0x80, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, op, c);
}
void DanteEndpoint::sendEncodingInfo() {
    std::vector<uint8_t> c; be16(c, 0x18); be16(c, 3); be32(c, (uint32_t)cfg.bits); be32(c, (uint32_t)cfg.bits); be16(c, 2); be16(c, 0);
    for (uint32_t b : { 24u, 16u, 32u }) be32(c, b);
    static const uint8_t op[8] = { 0x07, 0x2a, 0x00, 0x82, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, op, c);
}

void DanteEndpoint::handleSettings(const uint8_t* op, const uint8_t* c, size_t n, uint32_t fromIp) {
    uint16_t kind = rd16(op + 2);
    controllerQueries++; lastControllerQueryMs = (uint64_t)nowMs(); { char q[64]; snprintf(q, sizeof q, "settings 0x%04x from %s", kind, ipText(fromIp).c_str()); lastControllerQuery = q; }
    uint32_t mode = n >= 4 ? rd32(c) : 0, value = n >= 8 ? rd32(c + 4) : 0;
    switch (kind) {
    case 0x0081:                                              // sample rate: probe (mode 0) or set (mode 1)
        if (mode == 1) applySampleRate(value, "controller");
        sendSampleRateInfo(); break;
    case 0x0083:                                              // PCM encoding
        if (mode == 1) { if (value == 16 || value == 24 || value == 32) { if ((int)value != cfg.bits) { logf("controller set the encoding: PCM %d -> PCM %u", cfg.bits, value); cfg.bits = (int)value; recordsDirty = true; persistDirty = true; } } else { logf("controller asked for PCM %u: unsupported", value); errors++; } }
        sendEncodingInfo(); break;
    case 0x0063:                                              // identify: a real unit blinks its front panel
        logf("controller: identify this device"); identifyRequested = true;
        { static const uint8_t o[8] = { 0x07, 0x2a, 0x00, 0x62, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, o, { 0, 0, 0, 0 }); }
        break;
    case 0x0085:                                              // sample-rate pull-up: this unit has none
        { static const uint8_t o[8] = { 0x07, 0x2a, 0x00, 0x84, 0, 0, 0, 0 }; std::vector<uint8_t> v; be16(v, 0x18); be16(v, 1); be32(v, 0); be32(v, 0); be16(v, 2); be16(v, 0); be32(v, 0); infoSend(kInfoGroup, kInfoDstPort, 0xffff, o, v); }
        break;
    case 0x0090:                                              // reboot (mode 0) / factory reset (mode 1): not something a controller may do to this emulation
        logf("controller asked for a %s - refused (use the unit's own power / INITIALIZE menu)", (n >= 8 && rd16(c + 6) == 1) ? "factory reset" : "reboot"); errors++; break;
    case 0x0061: sendBoardInfo(); break;                      // the same information requests also arrive in the 0xFFFF settings family
    case 0x00c1: sendProductInfo(); break;
    case 0x0021: sendClockStats(); break;
    case 0x0013: sendNetworkInfo(); break;
    case 0x0077: { static const uint8_t o[8] = { 0x07, 0x2a, 0x00, 0x78, 0, 0, 0, 0 }; infoSend(kInfoGroup, kInfoDstPort, 0xffff, o, { 0, 0, 0, 3, 0, 0, 0, 0 }); break; }
    default:
        logf("settings message 0x%04x (family 0x%02x) from %s not understood: %s", kind, op[1], ipText(fromIp).c_str(), hex(c, n).c_str()); errors++; break;
    }
}

} // namespace rio
