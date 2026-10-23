// Dante endpoint, part 2: audio flows (transmit side served to other devices, receive side for our subscriptions),
// the IDanteTransport face towards the audio engine, state snapshots, diagnostics and persistence.
#include "dante_endpoint.h"
#include "bytes_be.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace rio {

using namespace rio::be;
static void be16(std::vector<uint8_t>& v, uint16_t x) { put16v(v, x); }
static void be32(std::vector<uint8_t>& v, uint32_t x) { put32v(v, x); }
static const uint16_t kArcHeader = 10;
static const uint16_t kDbcp1 = 0x1102;
static const int kFppMax = 256;
static const uint8_t kKeepalive[9] = { 'K', 'E', 'E', 'P', 'A', 'L', 'I', 'V', 'E' };   // what Audinate's own receiver (Dante Virtual Soundcard) sends every 250 ms; a transmitter stops a flow without them
static std::mt19937 g_rng{ 0x5EED };

// PTP sample count (samples since the PTP epoch at fs) for a PTP time in ns
static int64_t ptpSamples(int64_t ptpNs, uint32_t fs) { int64_t s = ptpNs / 1000000000LL, ns = ptpNs % 1000000000LL; if (ns < 0) { ns += 1000000000LL; s--; } return s * (int64_t)fs + (ns * (int64_t)fs) / 1000000000LL; }

// ---- flow control server: other devices ask for our channels ------------------------------------------------------------
void DanteEndpoint::pollFlowsControl() {
    uint8_t buf[2048];
    for (int k = 0; k < 32; k++) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(flowsSock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        tapPacket(buf, (size_t)r, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), ifIp, cfg.flowsPort);
        handleFlowRequest(buf, (size_t)r, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
    }
}

void DanteEndpoint::handleFlowRequest(const uint8_t* p, size_t n, uint32_t fromIp, uint16_t fromPort) {
    if (n < kArcHeader) return;
    uint16_t start = rd16(p), seq = rd16(p + 4), op = rd16(p + 6), code = rd16(p + 8);
    if (code != 0) return;
    const uint8_t* c = p + kArcHeader; size_t cn = n - kArcHeader;
    auto reply = [&](uint16_t rc, const std::vector<uint8_t>& content) {
        std::vector<uint8_t> o(kArcHeader + content.size()); put16(o.data(), start); put16(o.data() + 2, (uint16_t)o.size()); put16(o.data() + 4, seq); put16(o.data() + 6, op); put16(o.data() + 8, rc);
        memcpy(o.data() + kArcHeader, content.data(), content.size()); sendTo(flowsSock, o.data(), o.size(), fromIp, fromPort);
    };
    if (op == 0x0100) {                                           // request a unicast flow of our channels
        if (cn < 24) { errors++; return; }
        uint16_t hostOff = rd16(c); uint32_t fs = rd32(c + 2), bits = rd32(c + 6); uint16_t nch = rd16(c + 12), remoteOff = rd16(c + 14);
        if (nch == 0 || nch > 64 || cn < 16 + nch * 2 + 10) { errors++; reply(0x0302, {}); return; }
        std::vector<int> chan; for (int i = 0; i < nch; i++) { uint16_t id = rd16(c + 16 + i * 2); chan.push_back(id ? (int)id - 1 : -1); }
        size_t o = 16 + nch * 2 + 6; uint16_t fpp = rd16(c + o), flowNameOff = rd16(c + o + 2);
        std::string host = readz(p, n, hostOff), flowName = readz(p, n, flowNameOff);
        if (remoteOff + 8 > n || rd16(p + remoteOff) != 0x0802) { logf("flow request from %s with an unexpected receiver descriptor: %s", ipText(fromIp).c_str(), hex(c, cn).c_str()); errors++; reply(0x0302, {}); return; }
        uint16_t rxPort = rd16(p + remoteOff + 2); uint32_t rxIp = rd32(p + remoteOff + 4);
        std::string chText; for (int ch : chan) { chText += chText.empty() ? "" : ","; chText += ch >= 0 ? std::to_string(ch + 1) : "-"; }
        logf("'%s' requests flow '%s': TX %s -> %s:%u, %u Hz %u bit, %u frames/packet", host.c_str(), flowName.c_str(), chText.c_str(), ipText(rxIp).c_str(), rxPort, fs, bits, fpp);
        for (int ch : chan) if (ch >= kDanteTx) { logf("  rejected: channel out of range"); reply(0x0302, {}); return; }
        if (fs != cfg.fs) { logf("  rejected: sample rate mismatch (we run at %u Hz)", cfg.fs); reply(0x0301, {}); return; }
        if (fpp == 0 || fpp > kFppMax || (bits != 16 && bits != 24 && bits != 32)) { logf("  rejected: unsupported format"); reply(0x0302, {}); return; }
        std::string err; int idx = addTxFlow(rxIp, rxPort, chan, fpp, (int)bits / 8, host, flowName, err);
        if (idx < 0) { logf("  rejected: %s", err.c_str()); reply(0x0315, {}); return; }
        reply(1, std::vector<uint8_t>(txFlows[idx].handle, txFlows[idx].handle + 6));
    } else if (op == 0x0101) {                                    // stop
        if (cn < 6) { reply(0x0103, {}); return; }
        for (int i = 0; i < (int)txFlows.size(); i++) if (txFlows[i].active && memcmp(txFlows[i].handle, c, 6) == 0) { removeTxFlow(i, "stopped by the receiver"); reply(1, {}); return; }
        reply(0x0103, {});
    } else if (op == 0x0102) {                                    // update channel list
        if (cn < 8) { reply(0x0103, {}); return; }
        uint16_t nch = rd16(c + 6); if (cn < 8 + nch * 2u) { reply(0x0103, {}); return; }
        for (auto& f : txFlows) if (f.active && memcmp(f.handle, c, 6) == 0) {
            std::vector<int> chan; for (int i = 0; i < nch; i++) { uint16_t id = rd16(c + 8 + i * 2); chan.push_back(id && id <= kDanteTx ? (int)id - 1 : -1); }
            std::string chText; for (int ch : chan) { chText += chText.empty() ? "" : ","; chText += ch >= 0 ? std::to_string(ch + 1) : "-"; }
            logf("'%s' updates flow to %s:%u: TX %s", f.rxHost.c_str(), ipText(f.dstIp).c_str(), f.dstPort, chText.c_str());
            f.chan = chan; reply(1, {}); return;
        }
        reply(0x0103, {});
    } else { logf("flow control request 0x%04x from %s not understood: %s", op, ipText(fromIp).c_str(), hex(c, cn).c_str()); errors++; }
}

int DanteEndpoint::addTxFlow(uint32_t dstIp, uint16_t dstPort, const std::vector<int>& chan, int fpp, int bps, const std::string& rxHost, const std::string& rxFlowName, std::string& err) {
    for (int i = 0; i < (int)txFlows.size(); i++) if (txFlows[i].active && txFlows[i].dstIp == dstIp && txFlows[i].dstPort == dstPort) {
        txFlows[i].chan = chan; txFlows[i].fpp = fpp; txFlows[i].bps = bps; txFlows[i].rxHost = rxHost; txFlows[i].rxFlowName = rxFlowName; txFlows[i].bootstrapped = false; return i;
    }
    int idx = -1; for (int i = 0; i < (int)txFlows.size(); i++) if (!txFlows[i].active) { idx = i; break; }
    if (idx < 0) { err = "all " + std::to_string(txFlows.size()) + " transmit flows are in use"; return -1; }
    TxFlow f; f.sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (f.sock < 0) { err = sockError(); return -1; }
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = 0; sa.sin_addr.s_addr = htonl(ifIp);
    if (::bind(f.sock, (sockaddr*)&sa, sizeof sa) < 0) { err = std::string("bind: ") + sockError(); sockClose(f.sock); return -1; }
    int tos = 0xB8; setsockopt(f.sock, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof tos);
    int snd = 1 << 20; setsockopt(f.sock, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof snd);
    sockNonBlocking(f.sock);
    f.active = true; f.dstIp = dstIp; f.dstPort = dstPort; f.chan = chan; f.fpp = fpp; f.bps = bps; f.rxHost = rxHost; f.rxFlowName = rxFlowName; f.createdMs = nowMs();
    std::uniform_int_distribution<int> d(0, 65535); put32(f.handle, (uint32_t)idx); put16(f.handle + 4, (uint16_t)d(g_rng));
    txFlows[idx] = std::move(f);
    return idx;
}

void DanteEndpoint::removeTxFlow(int i, const char* why) {
    auto& f = txFlows[i]; if (!f.active) return;
    logf("TX flow %d to '%s' (%s:%u) removed: %s (%llu packets)", i + 1, f.rxHost.c_str(), ipText(f.dstIp).c_str(), f.dstPort, why, (unsigned long long)f.pkts);
    if (f.sock >= 0) sockClose(f.sock); f = TxFlow{};
}

void DanteEndpoint::pumpTx(int64_t mono) {
    int64_t ms = mono / 1000000; int sent = 0;
    static thread_local std::vector<uint8_t> burst; static thread_local std::vector<size_t> lens;
    for (auto& f : txFlows) {
        if (!f.active) continue;
        // keepalives from the receiver arrive on the flow's own socket
        uint8_t kb[64]; for (int k = 0; k < 8; k++) { ssize_t r = recv(f.sock, (char*)kb, sizeof kb, 0); if (r <= 0) break; if (!f.keepalives) logf("TX flow to '%s': first keepalive from the receiver, %d bytes: %s", f.rxHost.c_str(), (int)r, hex(kb, (size_t)r).c_str()); f.keepalives++; f.lastKeepaliveMs = ms; }
        size_t slots = f.chan.size(), stride = slots * f.bps, len = 9 + stride * f.fpp; burst.clear(); lens.clear();
        {   // build this flow's due packets from the ring under the audio lock, send them after releasing it
            std::lock_guard<std::mutex> a(audioMu);
            if (!anchored) continue;
            if (!f.bootstrapped || f.epoch != anchorEpoch || txWritePos - f.nextSample > kRing / 2 || f.nextSample > txWritePos) {
                // start at the newest fpp-aligned position (timestamps aligned to the packet size, as hardware does)
                int64_t S = sampleBase + (int64_t)txWritePos; S -= S % f.fpp; f.nextSample = (uint64_t)(S - sampleBase); if (f.nextSample > txWritePos) f.nextSample -= f.fpp;
                f.bootstrapped = true; f.epoch = anchorEpoch;
            }
            int n = 0;
            while (f.nextSample + f.fpp <= txWritePos && n < 64) {
                size_t at = burst.size(); burst.resize(at + len); uint8_t* p = burst.data() + at; p[0] = 2;
                int64_t S = sampleBase + (int64_t)f.nextSample; put32(p + 1, (uint32_t)(S / audioFs)); put32(p + 5, (uint32_t)(S % audioFs));
                for (int k = 0; k < f.fpp; k++) {
                    uint8_t* q = p + 9 + k * stride; size_t idx = (f.nextSample + k) & (kRing - 1);
                    for (size_t s = 0; s < slots; s++, q += f.bps) {
                        int ch = f.chan[s]; float v = ch >= 0 ? txRing[ch][idx] : 0.f; if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
                        int32_t x = (int32_t)std::lrint((double)v * 2147483392.0);          // 24-bit precision, left-justified
                        if (f.bps == 3) { q[0] = (uint8_t)(x >> 24); q[1] = (uint8_t)(x >> 16); q[2] = (uint8_t)(x >> 8); }
                        else if (f.bps == 2) { q[0] = (uint8_t)(x >> 24); q[1] = (uint8_t)(x >> 16); }
                        else put32(q, (uint32_t)x);
                    }
                }
                lens.push_back(len); f.nextSample += f.fpp; n++;
            }
        }
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(f.dstPort); to.sin_addr.s_addr = htonl(f.dstIp);
        size_t off = 0;
        for (size_t l : lens) {
            const uint8_t* p = burst.data() + off; off += l;
            if (sendto(f.sock, (const char*)p, (int)l, 0, (sockaddr*)&to, sizeof to) > 0) { f.pkts++; txPackets++; }
            if (trace && (f.pkts % 50) == 1) { sockaddr_in me{}; socklen_t ml = sizeof me; getsockname(f.sock, (sockaddr*)&me, &ml); tapPacket(p, l, ifIp, ntohs(me.sin_port), f.dstIp, f.dstPort); }   // sample the audio in the trace
            sent++;
        }
    }
    (void)sent;
}

// ---- receive side: subscriptions -> resolve -> request flows -> receive audio ------------------------------------------------
static std::string chanKey(const std::string& ch, const std::string& host) { return ch + "@" + host; }

void DanteEndpoint::rebuildRxFlows() {
    // desired: per TX host, the distinct TX channels we want; one flow per `nchan` channels, slots kept stable
    std::map<std::string, std::vector<std::string>> want;   // host -> channel names (ordered by first RX channel)
    for (int rx = 0; rx < kDanteRx; rx++) { auto& s = subs[rx]; if (s.txChannel.empty()) continue; auto& v = want[s.txHost]; if (std::find(v.begin(), v.end(), s.txChannel) == v.end()) v.push_back(s.txChannel); }
    // drop flows whose host has no subscriptions left, and slots no longer wanted
    for (int i = 0; i < (int)rxFlows.size(); i++) {
        auto& f = rxFlows[i]; if (!f.active) continue;
        auto it = want.find(f.txHost);
        if (it == want.end()) { removeRxFlow(i, "no subscriptions left for this device", true); continue; }
        bool changed = false;
        for (size_t s = 0; s < f.txIds.size(); s++) {
            std::string nm; for (auto& [k, ci] : chanInfo) if (ci.id == f.txIds[s] && dnssd::nameEq(ci.host, f.txHost)) nm = k.substr(0, k.find('@'));
            bool stillWanted = f.txIds[s] && std::find(it->second.begin(), it->second.end(), nm) != it->second.end();
            if (!stillWanted && f.txIds[s]) { f.txIds[s] = 0; f.local[s].clear(); changed = true; }
        }
        if (changed) { if (f.haveHandle) { f.reqOp = 0x02; f.reqPending = false; f.nextRequestMs = 0; } }
    }
    for (auto& [host, chans] : want) for (auto& chName : chans) {
        auto ci = chanInfo.find(chanKey(chName, host)); if (ci == chanInfo.end()) { chanInfo[chanKey(chName, host)].host = host; continue; }
        if (!(ci->second.haveSrv && ci->second.haveTxt && ci->second.haveA)) continue;
        // local RX channels fed by this TX channel
        std::vector<int> local; for (int rx = 0; rx < kDanteRx; rx++) if (subs[rx].txChannel == chName && dnssd::nameEq(subs[rx].txHost, host)) local.push_back(rx);
        // already in a flow?
        bool placed = false;
        for (auto& f : rxFlows) if (f.active && dnssd::nameEq(f.txHost, host)) for (size_t s = 0; s < f.txIds.size(); s++) if (f.txIds[s] == ci->second.id) { f.local[s] = local; placed = true; }
        if (placed) continue;
        int nchan = std::max(1, std::min<int>(ci->second.nchan, cfg.maxChannelsPerFlow));
        // a free slot in an existing flow of that host?
        RxFlow* target = nullptr;
        for (auto& f : rxFlows) if (f.active && dnssd::nameEq(f.txHost, host) && f.bps == ci->second.bits / 8) { for (size_t s = 0; s < f.txIds.size(); s++) if (f.txIds[s] == 0) { f.txIds[s] = ci->second.id; f.local[s] = local; target = &f; break; } if (!target && (int)f.txIds.size() < nchan) { f.txIds.push_back(ci->second.id); f.local.push_back(local); target = &f; } if (target) break; }
        if (target) { if (target->haveHandle) { target->reqOp = 0x02; target->reqPending = false; target->nextRequestMs = 0; } continue; }
        // new flow
        int idx = -1; for (int i = 0; i < (int)rxFlows.size(); i++) if (!rxFlows[i].active) { idx = i; break; }
        if (idx < 0) { logf("cannot subscribe %s@%s: all %zu receive flows are in use", chName.c_str(), host.c_str(), rxFlows.size()); continue; }
        RxFlow f; f.sock = (int)socket(AF_INET, SOCK_DGRAM, 0); if (f.sock < 0) continue;
        bool bound = false;
        for (int port = rxPortBase; port < rxPortBase + 254; port++) { sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(ifIp); if (::bind(f.sock, (sockaddr*)&sa, sizeof sa) == 0) { f.port = (uint16_t)port; bound = true; break; } }
        if (!bound) { sockClose(f.sock); logf("no free receive port for %s@%s", chName.c_str(), host.c_str()); continue; }
        int rcv = 1 << 20; setsockopt(f.sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof rcv); sockNonBlocking(f.sock);
        f.active = true; f.id = idx + 1; f.txHost = host; f.txIp = ci->second.ip; f.txFlowsPort = ci->second.flowsPort; f.dbcp1 = ci->second.dbcp1; f.bps = ci->second.bits / 8;
        // frames per packet: what the transmitter allows (its TXT "fpp=max,min"), about a millisecond of audio, within one MTU
        size_t mtuLimit = 1400 / std::max<size_t>(1, nchan * f.bps);
        int want = (int)std::min<size_t>(std::max<size_t>(1, cfg.fs / 1000), mtuLimit);
        f.fpp = std::max(ci->second.fppMin, std::min(ci->second.fppMax, want)); if (f.fpp < 1) f.fpp = 1;
        if ((size_t)f.fpp > mtuLimit) logf("note: '%s' insists on %d frames per packet, more than fit one MTU with %d channels", host.c_str(), f.fpp, nchan);
        f.txLatencyNs = ci->second.latencyNs;
        f.txIds.push_back(ci->second.id); f.local.push_back(local); f.flowName = std::to_string(idx + 1) + "_" + std::to_string(cfg.processId); f.reqOp = 0x00; f.nextRequestMs = 0;
        rxFlows[idx] = std::move(f);
        logf("RX flow %d for '%s' (%s:%u) created on port %u, %d bytes/sample, %d frames/packet", idx + 1, host.c_str(), ipText(ci->second.ip).c_str(), ci->second.flowsPort, rxFlows[idx].port, rxFlows[idx].bps, rxFlows[idx].fpp);
    }
    // flows that ended up empty
    for (int i = 0; i < (int)rxFlows.size(); i++) { auto& f = rxFlows[i]; if (!f.active) continue; bool any = false; for (auto id : f.txIds) if (id) any = true; if (!any) removeRxFlow(i, "empty", true); }
    updateEffectiveLatency(); updateRxActive();
}

void DanteEndpoint::updateRxActive() {
    bool act[kDanteRx] = {};
    for (auto& f : rxFlows) if (f.active && f.haveHandle && f.lastPacketMs) for (auto& l : f.local) for (int rx : l) if (rx >= 0 && rx < kDanteRx) act[rx] = true;   // audio has started: silence from now on is a dropout
    std::lock_guard<std::mutex> a(audioMu);
    for (int i = 0; i < kDanteRx; i++) { if (act[i] != rxActive[i]) { rxq[i].zeroRun = 0; rxq[i].inDropout = false; } rxActive[i] = act[i]; }
}

// Dante semantics: a receiver plays at the larger of its own latency setting and the transmitter's advertised minimum
void DanteEndpoint::updateEffectiveLatency() {
    uint32_t ns = cfg.rxLatencyNs; const char* who = nullptr;
    for (auto& f : rxFlows) if (f.active && f.txLatencyNs > ns) { ns = f.txLatencyNs; who = f.txHost.c_str(); }
    uint32_t frames = (uint32_t)((uint64_t)ns * cfg.fs / 1000000000ULL);
    if (frames != st.latencyFrames) {
        if (who) logf("receive latency %.2f -> %.2f ms: transmitter '%s' requires at least that", st.latencyFrames * 1000.0 / cfg.fs, ns / 1e6, who);
        else logf("receive latency back to the unit's own setting, %.2f ms", ns / 1e6);
        st.latencyFrames = frames; std::lock_guard<std::mutex> a(audioMu); audioLatencyFrames = frames;
    }
}

void DanteEndpoint::resolveSubscriptions(int64_t mono) {
    int64_t ms = mono / 1000000; bool resolvedSomething = false;
    for (int rx = 0; rx < kDanteRx; rx++) {
        auto& s = subs[rx]; if (s.txChannel.empty()) continue;
        std::string key = chanKey(s.txChannel, s.txHost); auto& ci = chanInfo[key]; ci.host = s.txHost;
        bool resolved = ci.haveSrv && ci.haveTxt && ci.haveA;
        if (!resolved) {
            if (ms - ci.queriedMs > 2000) { ci.queriedMs = ms; mdns.query(key + "._netaudio-chan._udp.local", dnssd::TYPE_ANY); }
            if (s.status != 1 && s.status != 8) s.status = 1;
            continue;
        }
        if (ci.rate && ci.rate != cfg.fs) { if (s.status != 0x15) logf("RX %d <- %s: sample rate mismatch (%u Hz there, %u Hz here)", rx + 1, key.c_str(), ci.rate, cfg.fs); s.status = 0x15; continue; }
        bool inFlow = false; for (auto& f : rxFlows) if (f.active) for (size_t k = 0; k < f.txIds.size(); k++) if (f.txIds[k] == ci.id && dnssd::nameEq(f.txHost, s.txHost)) { inFlow = true; s.status = f.haveHandle ? 0x01010009 : 8; }
        if (!inFlow) resolvedSomething = true;
    }
    if (resolvedSomething) rebuildRxFlows();
    for (auto& f : rxFlows) {
        if (!f.active) continue;
        if (f.reqPending && ms - f.requestMs > 3000) { f.reqPending = false; f.retries++; f.nextRequestMs = ms + std::min(30000, 2000 * f.retries); logf("RX flow %d: no answer from '%s' (%s:%u) to the flow request (try %d)", f.id, f.txHost.c_str(), ipText(f.txIp).c_str(), f.txFlowsPort, f.retries); errors++; }
        if (!f.reqPending && ms >= f.nextRequestMs && (!f.haveHandle || f.reqOp == 0x02)) requestRxFlow(f, mono);
        if (f.haveHandle && f.lastPacketMs && ms - f.lastPacketMs > 4000) { logf("RX flow %d from '%s': no audio for 4 s, requesting again", f.id, f.txHost.c_str()); f.haveHandle = false; f.lastPacketMs = 0; f.reqOp = 0x00; f.nextRequestMs = ms; for (auto& l : f.local) for (int rx : l) subs[rx].status = 8; }
    }
}

void DanteEndpoint::requestRxFlow(RxFlow& f, int64_t mono) {
    std::vector<uint8_t> body;
    if (f.reqOp == 0x02 && f.haveHandle) {                        // update the channel list of an existing flow
        body.insert(body.end(), f.handle, f.handle + 6); be16(body, (uint16_t)f.txIds.size()); for (auto id : f.txIds) be16(body, id);
    } else {                                                       // request a new flow (0x0100)
        f.reqOp = 0x00;
        size_t stringsOff = 0x26 + f.txIds.size() * 2 + kArcHeader;
        std::vector<uint8_t> strings; strings.insert(strings.end(), cfg.name.begin(), cfg.name.end()); strings.push_back(0);
        uint16_t flowNameOff = (uint16_t)(strings.size() + stringsOff); strings.insert(strings.end(), f.flowName.begin(), f.flowName.end()); strings.push_back(0);
        while ((strings.size() + stringsOff) % 8) strings.push_back(0);
        uint16_t remoteOff = (uint16_t)(strings.size() + stringsOff); be16(strings, 0x0802); be16(strings, f.port); be32(strings, ifIp);
        be16(body, (uint16_t)stringsOff); be32(body, cfg.fs); be32(body, (uint32_t)(f.bps * 8)); be16(body, 1); be16(body, (uint16_t)f.txIds.size()); be16(body, remoteOff);
        for (auto id : f.txIds) be16(body, id);
        be16(body, (uint16_t)(0x1c + 2 * f.txIds.size())); be16(body, 0x0a00); be16(body, 0x0002); be16(body, (uint16_t)f.fpp); be16(body, flowNameOff); for (int i = 0; i < 12; i++) body.push_back(0);
        body.insert(body.end(), strings.begin(), strings.end());
    }
    f.reqSeq = ctlSeq++; if (!ctlSeq) ctlSeq = 1;
    std::vector<uint8_t> p(kArcHeader + body.size()); put16(p.data(), f.dbcp1 ? f.dbcp1 : kDbcp1); put16(p.data() + 2, (uint16_t)p.size()); put16(p.data() + 4, f.reqSeq); put16(p.data() + 6, f.reqOp == 0x02 ? 0x0102 : 0x0100); put16(p.data() + 8, 0);
    memcpy(p.data() + kArcHeader, body.data(), body.size());
    sendTo(ctlSock, p.data(), p.size(), f.txIp, f.txFlowsPort);
    f.reqPending = true; f.requestMs = mono / 1000000;
    std::string ids; for (auto id : f.txIds) { ids += ids.empty() ? "" : ","; ids += id ? std::to_string(id) : "-"; }
    logf("RX flow %d: %s '%s' at %s:%u for its TX %s -> our port %u", f.id, f.reqOp == 0x02 ? "updating flow at" : "requesting flow from", f.txHost.c_str(), ipText(f.txIp).c_str(), f.txFlowsPort, ids.c_str(), f.port);
    for (auto& l : f.local) for (int rx : l) if (subs[rx].status != 0x01010009) subs[rx].status = 8;
}

void DanteEndpoint::rxControlResponse(const uint8_t* p, size_t n, uint32_t fromIp) {
    if (n < kArcHeader) return;
    uint16_t seq = rd16(p + 4), op = rd16(p + 6), code = rd16(p + 8);
    for (auto& f : rxFlows) {
        if (!f.active || !f.reqPending || f.reqSeq != seq) continue;
        f.reqPending = false;
        if (code != 1) {
            logf("RX flow %d: '%s' refused the request (0x%04x: %s)", f.id, f.txHost.c_str(), code, code == 0x0301 ? "sample rate mismatch" : code == 0x0315 ? "too many transmit flows there" : code == 0x0103 ? "flow not found" : "error"); errors++;
            uint32_t stat = code == 0x0315 ? 0x14 : 0x15; for (auto& l : f.local) for (int rx : l) subs[rx].status = stat;
            if (code == 0x0103) { f.haveHandle = false; f.reqOp = 0x00; } f.retries++; f.nextRequestMs = nowMs() + std::min(30000, 5000 * f.retries);
            return;
        }
        if (op == 0x0100) {
            if (n < kArcHeader + 6) return;
            memcpy(f.handle, p + kArcHeader, 6); f.haveHandle = true; f.retries = 0; f.reqOp = 0; updateRxActive();
            logf("RX flow %d: '%s' sends TX to our port %u (handle %s)", f.id, f.txHost.c_str(), f.port, hex(f.handle, 6).c_str());
        } else if (op == 0x0102) { f.reqOp = 0; logf("RX flow %d: channel list updated at '%s'", f.id, f.txHost.c_str()); }
        for (auto& l : f.local) for (int rx : l) subs[rx].status = 0x01010009;
        (void)fromIp; return;
    }
}

void DanteEndpoint::removeRxFlow(int i, const char* why, bool tellTx) {
    auto& f = rxFlows[i]; if (!f.active) return;
    logf("RX flow %d from '%s' removed: %s (%llu packets, %llu late)", f.id, f.txHost.c_str(), why, (unsigned long long)f.pkts, (unsigned long long)f.late);
    if (tellTx && f.haveHandle && ctlSock >= 0) {
        std::vector<uint8_t> p(kArcHeader + 6); put16(p.data(), f.dbcp1 ? f.dbcp1 : kDbcp1); put16(p.data() + 2, (uint16_t)p.size()); put16(p.data() + 4, ctlSeq++); put16(p.data() + 6, 0x0101); put16(p.data() + 8, 0); memcpy(p.data() + kArcHeader, f.handle, 6);
        sendTo(ctlSock, p.data(), p.size(), f.txIp, f.txFlowsPort);
    }
    for (auto& l : f.local) for (int rx : l) if (!subs[rx].txChannel.empty()) subs[rx].status = 1;
    if (f.sock >= 0) sockClose(f.sock); f = RxFlow{};
    updateRxActive();
}

void DanteEndpoint::onAudioPacket(RxFlow& f, const uint8_t* p, size_t n, int64_t mono) {
    if (n < 9) return;
    size_t slots = f.txIds.size(), stride = slots * f.bps; if (!stride) return;
    size_t frames = (n - 9) / stride;
    std::lock_guard<std::mutex> a(audioMu);
    if (!anchored) return;
    int64_t S = (int64_t)rd32(p + 1) * audioFs + rd32(p + 5);     // PTP sample count of the first frame
    int64_t i = S - sampleBase;                                   // engine sample index
    int64_t nowS = ptpSamples(audioPtpNs(mono), audioFs);
    int64_t lat = nowS - S; if (lat > 0) { int64_t ns = lat * 1000000000LL / audioFs; if (ns > f.maxLatencyNs) f.maxLatencyNs = ns; }
    if (std::llabs(i - (int64_t)lastEngineSample) > (int64_t)kRing / 2) {        // not in our clock domain: the transmitter follows another grandmaster (or none)
        f.otherDomain++; f.domainGapS = (nowS - S) / (int64_t)audioFs;
        if (f.otherDomain == 1 || (f.otherDomain % 20000) == 0) fprintf(stderr, "[dante-net] RX flow %d: audio timestamps from '%s' are %lld s away from our PTP time - different clock domain, not played (%llu packets)\n", f.id, f.txHost.c_str(), (long long)f.domainGapS, (unsigned long long)f.otherDomain);
        return;
    }
    int64_t playHead = (int64_t)lastEngineSample - (int64_t)audioLatencyFrames;   // the engine already played up to here
    int64_t margin = i - playHead;                                 // samples of headroom before this packet's first frame is due
    if (margin < f.minMarginSamples) f.minMarginSamples = margin;
    if (i + (int64_t)frames <= playHead) { f.late++; rxLate++; int64_t lateBy = playHead - i; if (lateBy > f.maxLateSamples) f.maxLateSamples = lateBy; }
    for (size_t s = 0; s < slots; s++) {
        if (f.local[s].empty()) continue;
        const uint8_t* q = p + 9 + s * f.bps;
        for (size_t k = 0; k < frames; k++, q += stride) {
            int32_t x = f.bps == 3 ? (int32_t)(((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8)) : f.bps == 2 ? (int32_t)(((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16)) : (int32_t)rd32(q);
            float v = (float)(x / 2147483648.0); size_t idx = (size_t)(i + (int64_t)k) & (kRing - 1);
            for (int rx : f.local[s]) rxRing[rx][idx] = v;
        }
    }
    f.pkts++; rxPackets++;
}

void DanteEndpoint::pollRx(int64_t mono) {
    uint8_t buf[2048]; int64_t ms = mono / 1000000;
    for (int k = 0; k < 16; k++) {                                // flow control answers
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(ctlSock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        tapPacket(buf, (size_t)r, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), ifIp, 0);
        rxControlResponse(buf, (size_t)r, ntohl(from.sin_addr.s_addr));
    }
    for (auto& f : rxFlows) {
        if (!f.active) continue;
        for (int k = 0; k < 64; k++) {
            sockaddr_in from{}; socklen_t fl = sizeof from;
            ssize_t r = recvfrom(f.sock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
            if (r <= 0) break;
            bool firstPacket = !f.lastPacketMs;
            if (firstPacket) logf("RX flow %d: audio from %s:%u arriving", f.id, ipText(ntohl(from.sin_addr.s_addr)).c_str(), ntohs(from.sin_port));
            f.lastSrcIp = ntohl(from.sin_addr.s_addr); f.lastSrcPort = ntohs(from.sin_port); f.lastPacketMs = ms;
            if (firstPacket) updateRxActive();
            if (trace && (f.pkts % 50) == 0) tapPacket(buf, (size_t)r, f.lastSrcIp, f.lastSrcPort, ifIp, f.port);
            if (st.primaryLink || st.secondaryLink) onAudioPacket(f, buf, (size_t)r, mono);
        }
        if (f.lastSrcIp && ms >= f.nextKeepaliveMs) {            // tell the transmitter we are still listening
            f.nextKeepaliveMs = ms + 250;
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(f.lastSrcPort); to.sin_addr.s_addr = htonl(f.lastSrcIp);
            sendto(f.sock, (const char*)kKeepalive, sizeof kKeepalive, 0, (sockaddr*)&to, sizeof to);
        }
    }
}

int64_t DanteEndpoint::mediaTimeNs() { std::lock_guard<std::mutex> a(audioMu); return audioPtpNs(monoNs()); }
int64_t DanteEndpoint::ptpSampleCount(int64_t ptpNs, uint32_t fs) { return ptpSamples(ptpNs, fs); }

// ---- IDanteTransport ----------------------------------------------------------------------------------------------------------
void DanteEndpoint::setFormat(uint32_t fs, int) { std::lock_guard<std::mutex> g(mu); if (fs && fs != cfg.fs) { cfg.fs = fs; st.fs = fs; st.latencyFrames = (uint32_t)((uint64_t)cfg.rxLatencyNs * fs / 1000000000ULL); { std::lock_guard<std::mutex> a(audioMu); audioFs = fs; audioLatencyFrames = st.latencyFrames; anchored = false; anchorEpoch++; } recordsDirty = true; logf("sample rate now %u Hz", fs); } }
void DanteEndpoint::setLatencyFrames(uint32_t n) { std::lock_guard<std::mutex> g(mu); st.latencyFrames = n; cfg.rxLatencyNs = (uint32_t)((uint64_t)n * 1000000000ULL / cfg.fs); { std::lock_guard<std::mutex> a(audioMu); audioLatencyFrames = n; } recordsDirty = true; }

void DanteEndpoint::sendTx(const float* const* tx, int nch, size_t frames, uint64_t sampleTime) {
    if (txTap) txTap(tx, nch, frames);
    std::lock_guard<std::mutex> a(audioMu);
    st.txBlocks++;
    int64_t mono = monoNs(); int64_t nowS = ptpSamples(audioPtpNs(mono), audioFs);
    int64_t aheadS = audioAheadNs * (int64_t)audioFs / 1000000000LL;
    int64_t expect = sampleBase + (int64_t)sampleTime; int64_t tol = (int64_t)audioFs / 25;   // 40 ms: pacing jitter of a loaded PC must not re-anchor (a re-anchor is an audible step)
    if (!anchored || std::llabs(nowS + aheadS - expect) > tol) {
        if (anchored) { uint64_t r = ++resyncs; if (r <= 20 || (r % 100) == 0) fprintf(stderr, "[dante-net] media clock re-anchored (#%llu): engine time drifted %.1f ms from the PTP clock (emulation stalled or clock stepped)\n", (unsigned long long)r, (double)(nowS + aheadS - expect) * 1000.0 / audioFs); }
        sampleBase = nowS + aheadS - (int64_t)sampleTime; anchored = true; anchorEpoch++;
    }
    for (int c = 0; c < nch && c < kDanteTx; c++) for (size_t i = 0; i < frames; i++) txRing[c][(sampleTime + i) & (kRing - 1)] = tx[c][i];
    txWritePos = sampleTime + frames; txStarted = true; lastEngineMonoNs = mono;
}

void DanteEndpoint::receiveRx(float* const* rx, int nch, size_t frames, uint64_t sampleTime) {
    std::lock_guard<std::mutex> a(audioMu);
    st.rxBlocks++; lastEngineSample = sampleTime + frames;
    bool link = st.primaryLink || st.secondaryLink; uint32_t L = audioLatencyFrames;
    for (int c = 0; c < nch && c < kDanteRx; c++) {
        bool watch = rxActive[c]; RxQuality& q = rxq[c];
        for (size_t i = 0; i < frames; i++) {
            uint64_t t = sampleTime + i; if (t < L || !link) { rx[c][i] = 0.f; continue; }
            size_t idx = (t - L) & (kRing - 1); float v = rxRing[c][idx]; rx[c][i] = v; rxRing[c][idx] = 0.f;   // consume: silence where nothing arrived
            if (watch) {
                if (v == 0.f) { if (++q.zeroRun == audioFs / 1000 && !q.inDropout) { q.inDropout = true; q.dropouts++; } if (q.inDropout) q.dropoutSamples++; }
                else { q.zeroRun = 0; q.inDropout = false; }
            }
        }
    }
}

bool DanteEndpoint::subscribe(int rxChannel, const std::string& device, int txChannel) {
    char b[8]; snprintf(b, sizeof b, "%02d", txChannel + 1);
    return subscribeNamed(rxChannel, device.empty() ? "" : std::string(b), device);
}
bool DanteEndpoint::subscribeNamed(int rxChannel, const std::string& txChannel, const std::string& txHost) {
    if (rxChannel < 0 || rxChannel >= kDanteRx) return false;
    std::lock_guard<std::mutex> g(mu);
    if (txChannel.empty() || txHost.empty()) subs[rxChannel] = {}; else { subs[rxChannel].txChannel = txChannel; subs[rxChannel].txHost = txHost; subs[rxChannel].status = 1; }
    logf("RX %d <- %s", rxChannel + 1, txChannel.empty() ? "(none)" : (txChannel + "@" + txHost).c_str());
    rebuildRxFlows(); persistDirty = true; return true;
}
std::string DanteEndpoint::subscription(int rxChannel) const { std::lock_guard<std::mutex> g(mu); if (rxChannel < 0 || rxChannel >= kDanteRx || subs[rxChannel].txChannel.empty()) return ""; return subs[rxChannel].txChannel + "@" + subs[rxChannel].txHost; }

// ---- emulator-side API ------------------------------------------------------------------------------------------------------
DanteNetStatus DanteEndpoint::snapshot() {
    std::lock_guard<std::mutex> g(mu); DanteNetStatus s;
    s.running = running; s.linkPrimary = st.primaryLink; s.linkSecondary = st.secondaryLink; s.ip = ifIp; s.ip2 = ifIp2;
    s.arcPort = cfg.arcPort; s.cmcPort = cfg.cmcPort; s.flowsPort = cfg.flowsPort; s.infoPort = cfg.infoPort; s.name = cfg.name; s.factoryName = cfg.factoryName; s.fs = cfg.fs;
    const auto& ps = ptpClock.status(); s.clockState = ps.state; s.clockLocked = ps.state == ptp::State::Master || ps.locked; s.clockMaster = ps.state != ptp::State::Slave; s.preferred = cfg.preferredMaster; s.masterId = ptp::ClockV1::uuidText(ps.masterUuid); s.ptpOffsetNs = ps.offsetNs; s.ptpFreqPpb = ps.freqPpb;
    for (auto& [n, p] : peers) s.peers.push_back(n);
    s.subs = subs; for (int i = 0; i < kDanteRx; i++) { s.subs[i].dropouts = rxq[i].dropouts; s.subs[i].dropoutMs = rxq[i].dropoutSamples * 1000.0 / cfg.fs; } for (auto& c : txChans) s.txLabels.push_back(c.friendlyName); for (auto& c : rxChans) s.rxLabels.push_back(c.friendlyName);
    for (auto& f : txFlows) if (f.active) s.txFlows++; for (auto& f : rxFlows) if (f.active) s.rxFlows++;
    s.controllerQueries = controllerQueries; s.lastControllerQueryMs = lastControllerQueryMs; s.lastControllerQuery = lastControllerQuery;
    s.txPackets = txPackets; s.rxPackets = rxPackets; s.rxLate = rxLate; s.resyncs = resyncs.load(); s.errors = errors; s.nameChangedByController = nameChangedByController; s.identify = identifyRequested; s.bits = cfg.bits;
    return s;
}
void DanteEndpoint::clearNameChangedFlag() { std::lock_guard<std::mutex> g(mu); nameChangedByController = false; identifyRequested = false; }
void DanteEndpoint::setDeviceName(const std::string& n) { std::lock_guard<std::mutex> g(mu); std::string nm = n.size() > 31 ? n.substr(0, 31) : n; if (nm.empty() || nm == cfg.name) return; logf("device name set by the unit: '%s' -> '%s'", cfg.name.c_str(), nm.c_str()); mdns.goodbye(); cfg.name = nm; recordsDirty = true; persistDirty = true; }
void DanteEndpoint::setChannelLabel(bool tx, int index, const std::string& label) { std::lock_guard<std::mutex> g(mu); auto& v = tx ? txChans : rxChans; if (index < 0 || index >= (int)v.size()) return; std::string l = label.empty() ? v[index].factoryName : label; if (l == v[index].friendlyName) return; v[index].friendlyName = l; if (tx) recordsDirty = true; persistDirty = true; }
void DanteEndpoint::setSampleRate(uint32_t fs) { std::lock_guard<std::mutex> g(mu); applySampleRate(fs, "the unit"); }
void DanteEndpoint::setPreferredMaster(bool p) { std::lock_guard<std::mutex> g(mu); if (p != cfg.preferredMaster) { cfg.preferredMaster = p; ptpClock.setPreferred(p); logf("preferred master %s", p ? "on" : "off"); } }
void DanteEndpoint::setRedundant(bool r) { std::lock_guard<std::mutex> g(mu); if (r != cfg.redundant) { cfg.redundant = r; st.redundant = r; logf("switch mode: %s", r ? "redundant" : "switched (daisy chain)"); } }
void DanteEndpoint::failLink(bool secondary, bool failed) { std::lock_guard<std::mutex> g(mu); (secondary ? link2Failed : link1Failed) = failed; logf("%s link %s", secondary ? "secondary" : "primary", failed ? "down (test)" : "up"); }
std::string DanteEndpoint::logText(size_t maxLines) { std::lock_guard<std::mutex> g(mu); std::string s; size_t start = logLines.size() > maxLines ? logLines.size() - maxLines : 0; for (size_t i = start; i < logLines.size(); i++) { s += logLines[i]; s += '\n'; } return s; }

static std::string jsonStr(const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((unsigned char)c < 0x20) o += ' '; else o += c; } return o + "\""; }

std::string DanteEndpoint::statusJson() {
    std::lock_guard<std::mutex> g(mu); std::ostringstream o; const auto& ps = ptpClock.status();
    const char* cs = ps.state == ptp::State::Master ? "master" : ps.state == ptp::State::Slave ? "slave" : ps.state == ptp::State::Listening ? "listening" : "off";
    o << "{\"running\":" << (running ? "true" : "false") << ",\"name\":" << jsonStr(cfg.name) << ",\"factoryName\":" << jsonStr(cfg.factoryName) << ",\"manufacturer\":" << jsonStr(cfg.manufacturer) << ",\"model\":" << jsonStr(cfg.modelName) << ",\"modelId\":" << jsonStr(cfg.modelId);
    char idhex[20]; snprintf(idhex, sizeof idhex, "%02x%02x%02x%02x%02x%02x%02x%02x", cfg.deviceId[0], cfg.deviceId[1], cfg.deviceId[2], cfg.deviceId[3], cfg.deviceId[4], cfg.deviceId[5], cfg.deviceId[6], cfg.deviceId[7]);
    o << ",\"deviceId\":\"" << idhex << "\",\"mac\":\"" << ptp::ClockV1::uuidText(cfg.mac) << "\",\"ip\":" << jsonStr(ipText(ifIp)) << ",\"ip2\":" << jsonStr(ifIp2 ? ipText(ifIp2) : "") << ",\"ports\":{\"arc\":" << cfg.arcPort << ",\"cmc\":" << cfg.cmcPort << ",\"info\":" << cfg.infoPort << ",\"flows\":" << cfg.flowsPort << "}";
    o << ",\"standardPorts\":" << ((cfg.arcPort == 4440 && cfg.cmcPort == 8800 && cfg.infoPort == 8700 && cfg.flowsPort == 4455) ? "true" : "false");
    o << ",\"fs\":" << cfg.fs << ",\"bits\":" << cfg.bits << ",\"latencyMs\":" << cfg.rxLatencyNs / 1e6 << ",\"redundant\":" << (cfg.redundant ? "true" : "false") << ",\"linkPrimary\":" << (st.primaryLink ? "true" : "false") << ",\"linkSecondary\":" << (st.secondaryLink ? "true" : "false");
    o << ",\"clock\":{\"state\":\"" << cs << "\",\"locked\":" << ((ps.state == ptp::State::Master || ps.locked) ? "true" : "false") << ",\"master\":" << jsonStr(ptp::ClockV1::uuidText(ps.masterUuid)) << ",\"preferred\":" << (cfg.preferredMaster ? "true" : "false") << ",\"offsetUs\":" << ps.offsetNs / 1000.0 << ",\"pathDelayUs\":" << ps.meanPathDelayNs / 1000.0 << ",\"freqPpb\":" << ps.freqPpb << ",\"foreignMasters\":" << ps.foreignMasters << ",\"syncRx\":" << ps.syncRx << ",\"syncTx\":" << ps.syncTx << ",\"delayReqTx\":" << ps.delayReqTx << ",\"delayRespRx\":" << ps.delayRespRx << ",\"delayReqRx\":" << ps.delayReqRx << ",\"delayRespTx\":" << ps.delayRespTx << ",\"steps\":" << ps.steps << ",\"open\":" << (ptpClock.isOpen() ? "true" : "false") << "}";
    o << ",\"discovery\":{\"records\":" << mdns.currentRecords().size() << ",\"queriesSeen\":" << mdns.rxQueries << ",\"answered\":" << mdns.answered << ",\"responsesSeen\":" << mdns.rxResponses << ",\"lastQuery\":" << jsonStr(mdns.lastQuery) << ",\"lastQueryAgeMs\":" << (mdns.lastQueryMs ? nowMs() - (int64_t)mdns.lastQueryMs : -1) << "}";
    o << ",\"controller\":{\"queries\":" << controllerQueries << ",\"last\":" << jsonStr(lastControllerQuery) << ",\"lastAgeMs\":" << (lastControllerQueryMs ? nowMs() - (int64_t)lastControllerQueryMs : -1) << "}";
    o << ",\"peers\":["; bool first = true; for (auto& [n, p] : peers) { o << (first ? "" : ",") << "{\"name\":" << jsonStr(n) << ",\"ip\":" << jsonStr(ipText(p.ip)) << ",\"ageMs\":" << nowMs() - p.lastSeenMs << "}"; first = false; } o << "]";
    o << ",\"tx\":["; for (int i = 0; i < kDanteTx; i++) o << (i ? "," : "") << jsonStr(txChans[i].friendlyName); o << "]";
    o << ",\"rx\":["; for (int i = 0; i < kDanteRx; i++) { const auto& s = subs[i]; const char* stn = s.txChannel.empty() ? "none" : s.status == 0x01010009 ? "receiving" : s.status == 8 ? "in progress" : s.status == 1 ? "unresolved" : s.status == 0x15 ? "tx error" : s.status == 0x14 ? "tx busy" : "pending";
        o << (i ? "," : "") << "{\"label\":" << jsonStr(rxChans[i].friendlyName) << ",\"sub\":" << jsonStr(s.txChannel.empty() ? "" : s.txChannel + "@" + s.txHost) << ",\"status\":\"" << stn << "\",\"dropouts\":" << rxq[i].dropouts << ",\"dropoutMs\":" << rxq[i].dropoutSamples * 1000.0 / cfg.fs << "}"; } o << "]";
    o << ",\"txFlows\":["; first = true; for (size_t i = 0; i < txFlows.size(); i++) { const auto& f = txFlows[i]; if (!f.active) continue; std::string ch; for (int c : f.chan) { ch += ch.empty() ? "" : ","; ch += c >= 0 ? std::to_string(c + 1) : "-"; }
        o << (first ? "" : ",") << "{\"id\":" << i + 1 << ",\"to\":" << jsonStr(f.rxHost) << ",\"dst\":" << jsonStr(ipText(f.dstIp) + ":" + std::to_string(f.dstPort)) << ",\"channels\":" << jsonStr(ch) << ",\"fpp\":" << f.fpp << ",\"bits\":" << f.bps * 8 << ",\"packets\":" << f.pkts << ",\"keepalives\":" << f.keepalives << "}"; first = false; } o << "]";
    o << ",\"rxFlows\":["; first = true; for (const auto& f : rxFlows) { if (!f.active) continue; std::string ids; for (auto id : f.txIds) { ids += ids.empty() ? "" : ","; ids += id ? std::to_string(id) : "-"; }
        o << (first ? "" : ",") << "{\"id\":" << f.id << ",\"from\":" << jsonStr(f.txHost) << ",\"src\":" << jsonStr(ipText(f.txIp) + ":" + std::to_string(f.txFlowsPort)) << ",\"port\":" << f.port << ",\"txChannels\":" << jsonStr(ids) << ",\"established\":" << (f.haveHandle ? "true" : "false") << ",\"packets\":" << f.pkts << ",\"late\":" << f.late << ",\"otherDomain\":" << f.otherDomain << ",\"domainGapS\":" << f.domainGapS << ",\"maxLateMs\":" << f.maxLateSamples * 1000.0 / cfg.fs << ",\"minMarginMs\":" << (f.minMarginSamples == (1 << 30) ? 0.0 : f.minMarginSamples * 1000.0 / cfg.fs) << ",\"audioAgeMs\":" << (f.lastPacketMs ? nowMs() - f.lastPacketMs : -1) << ",\"fpp\":" << f.fpp << "}"; first = false; } o << "]";
    o << ",\"counters\":{\"txPackets\":" << txPackets << ",\"rxPackets\":" << rxPackets << ",\"rxLate\":" << rxLate << ",\"resyncs\":" << resyncs.load() << ",\"errors\":" << errors << ",\"txBlocks\":" << st.txBlocks << ",\"rxBlocks\":" << st.rxBlocks << ",\"mdnsTx\":" << mdns.txPackets << ",\"mdnsRx\":" << mdns.rxPackets << ",\"multicastRejoins\":" << rejoins << ",\"mdnsSilentMs\":" << (mdns.lastRxMs ? nowMs() - (int64_t)mdns.lastRxMs : -1) << ",\"ptpSilentMs\":" << (ptpClock.lastRxNs ? (monoNs() - ptpClock.lastRxNs) / 1000000 : -1) << "}";
    bool anch; { std::lock_guard<std::mutex> a(audioMu); anch = anchored; }
    o << ",\"trace\":" << jsonStr(cfg.tracePath) << ",\"anchored\":" << (anch ? "true" : "false") << "}";
    return o.str();
}

void DanteEndpoint::persist() {
    if (cfg.persistPath.empty()) return;
    std::ofstream f(cfg.persistPath, std::ios::trunc); if (!f) { logf("cannot write %s", cfg.persistPath.c_str()); return; }
    f << "# Dante endpoint state (names, format and subscriptions), rewritten by the emulator\n" << "name=" << cfg.name << "\n" << "fs=" << cfg.fs << "\n" << "bits=" << cfg.bits << "\n" << "latency_ns=" << cfg.rxLatencyNs << "\n";
    for (int i = 0; i < kDanteTx; i++) if (txChans[i].friendlyName != txChans[i].factoryName) f << "tx." << i + 1 << "=" << txChans[i].friendlyName << "\n";
    for (int i = 0; i < kDanteRx; i++) if (rxChans[i].friendlyName != rxChans[i].factoryName) f << "rx." << i + 1 << "=" << rxChans[i].friendlyName << "\n";
    for (int i = 0; i < kDanteRx; i++) if (!subs[i].txChannel.empty()) f << "sub." << i + 1 << "=" << subs[i].txChannel << "@" << subs[i].txHost << "\n";
}

void DanteEndpoint::loadPersisted() {
    if (cfg.persistPath.empty()) return;
    std::ifstream f(cfg.persistPath); if (!f) return; std::string line; int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue; size_t e = line.find('='); if (e == std::string::npos) continue; std::string k = line.substr(0, e), v = line.substr(e + 1);
        if (k == "name" && !v.empty()) { cfg.name = v.size() > 31 ? v.substr(0, 31) : v; n++; }
        else if (k == "fs") { uint32_t fs = (uint32_t)atoi(v.c_str()); if (fs == 44100 || fs == 48000 || fs == 88200 || fs == 96000) { cfg.fs = fs; n++; } }
        else if (k == "bits") { int b = atoi(v.c_str()); if (b == 16 || b == 24 || b == 32) { cfg.bits = b; n++; } }
        else if (k == "latency_ns") { uint32_t ns = (uint32_t)atol(v.c_str()); if (ns >= 250000 && ns <= 20000000) { cfg.rxLatencyNs = ns; n++; } }
        else if (k.rfind("tx.", 0) == 0) { int i = atoi(k.c_str() + 3); if (i >= 1 && i <= kDanteTx) { txChans[i - 1].friendlyName = v; n++; } }
        else if (k.rfind("rx.", 0) == 0) { int i = atoi(k.c_str() + 3); if (i >= 1 && i <= kDanteRx) { rxChans[i - 1].friendlyName = v; n++; } }
        else if (k.rfind("sub.", 0) == 0) { int i = atoi(k.c_str() + 4); size_t at = v.find('@'); if (i >= 1 && i <= kDanteRx && at != std::string::npos) { subs[i - 1].txChannel = v.substr(0, at); subs[i - 1].txHost = v.substr(at + 1); subs[i - 1].status = 1; n++; } }
    }
    if (n) logf("restored %d setting(s) from %s", n, cfg.persistPath.c_str());
}

} // namespace rio
