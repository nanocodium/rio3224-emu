// Virtual Dante network between processes: emulator instances (and tools such as a virtual console or a WAV
// player) exchange PCM over UDP multicast on a "primary" and optionally a "secondary" network (two different
// multicast groups/ports, mirroring the two Ethernet ports).  This is NOT Audinate's Dante protocol - no licensed
// technology is implemented and no interoperability with real Dante devices is claimed; it reproduces the
// behaviour the Rio exposes: named devices announcing their TX channels, receiver-side subscriptions (rx <- device@tx),
// a fixed receive latency, packet loss, redundant transmission on both networks with duplicate suppression and
// seamless fail-over, and the link/role status the firmware shows.
//
// Packet format (all little-endian, UDP payload):
//   magic "RIOV" | u8 type | u8 version(1) | u16 flags
//   type 1 ANNOUNCE: char name[32] | u32 fs | u16 txChannels | u16 rxChannels | u64 sampleTime | u8 clockLeader
//   type 2 AUDIO:    char name[32] | u32 fs | u16 channels | u16 frames | u64 sampleTime | u32 seq | int32 pcm[frames*channels] (24-bit left-justified)
//   type 3 SUBSCRIBE (control): char target[32] | u16 rxChannel(1-based) | char device[32] | u16 txChannel(1-based)   (empty device = clear)
//   type 4 STATUS request / type 5 STATUS reply (text) for tools
#pragma once
#include "dante_transport.h"
#include "../platform.h"
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace rio {

class VirtualDanteTransport : public IDanteTransport {
public:
    struct Net { std::string group = "239.69.83.1"; uint16_t port = 14336; std::string ifAddr; int sock = -1; bool enabled = false, failed = false; uint64_t lastRxMs = 0; uint64_t pkts = 0, sendFail = 0; };
    struct Peer {
        std::string name; uint32_t fs = 48000; int txChannels = 0, rxChannels = 0; bool clockLeader = false;
        uint64_t lastSeenMs = 0; int64_t offset = 0; bool haveOffset = false; uint32_t lastSeq = 0; bool haveSeq = false;
        std::vector<std::vector<float>> ring; uint64_t writePos = 0; uint64_t resyncs = 0, lost = 0, dup = 0, late = 0;
    };

    explicit VirtualDanteTransport(const std::string& selfName) : self(selfName) {}
    ~VirtualDanteTransport() override { for (Net* n : { &pri, &sec }) if (n->sock >= 0) sockClose(n->sock); }
    const char* name() const override { return "virtual-dante"; }

    // "GROUP:PORT" or ":PORT" or "" for the defaults; ifAddr = local interface address for multicast (optional)
    bool open(const std::string& primarySpec, const std::string& secondarySpec, std::string& err) {
        platformInit();
        parse(primarySpec, pri); if (!bind(pri, err)) return false; pri.enabled = true;
        if (!secondarySpec.empty()) { sec.port = pri.port + 1; sec.group = "239.69.83.2"; parse(secondarySpec, sec); if (!bind(sec, err)) return false; sec.enabled = true; }
        st.primaryLink = true; st.secondaryLink = sec.enabled; st.redundant = sec.enabled;
        return true;
    }
    void setFormat(uint32_t fs, int) override { st.fs = fs; }
    void setLatencyFrames(uint32_t n) override { st.latencyFrames = n; }
    const DanteStatus& status() const override { return st; }
    DanteStatus& mutableStatus() { return st; }
    std::function<void(const float* const* tx, int nch, size_t frames)> txTap;
    double dropPercent = 0.0;                       // simulated loss per network (independent on primary / secondary)
    void failNetwork(bool secondary, bool failed) { (secondary ? sec : pri).failed = failed; }
    void setClockLeader(bool leader) { st.clockMaster = leader; }

    void sendTx(const float* const* tx, int nch, size_t frames, uint64_t sampleTime) override {
        st.txBlocks++; if (txTap) txTap(tx, nch, frames);
        now = sampleTime; pumpAnnounce();
        if (!pri.enabled) return;
        // AUDIO packet: 24-bit left-justified int32 PCM
        pkt.resize(8 + 32 + 4 + 2 + 2 + 8 + 4 + frames * nch * 4);
        uint8_t* p = pkt.data(); memcpy(p, "RIOV", 4); p[4] = 2; p[5] = 1; p[6] = p[7] = 0; p += 8;
        putName(p, self); p += 32; put32(p, st.fs); p += 4; put16(p, (uint16_t)nch); p += 2; put16(p, (uint16_t)frames); p += 2; put64(p, sampleTime); p += 8; put32(p, txSeq++); p += 4;
        for (size_t i = 0; i < frames; i++) for (int c = 0; c < nch; c++) {
            float v = tx[c][i]; if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
            int32_t s = (int32_t)std::lrint(v * 8388607.0) << 8; memcpy(p, &s, 4); p += 4;
        }
        for (Net* n : { &pri, &sec }) {
            if (!n->enabled || n->failed) continue;
            if (dropPercent > 0 && drop(rng) < dropPercent) continue;
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(n->port); to.sin_addr.s_addr = inet_addr(n->group.c_str());
            if (sendto(n->sock, (const char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&to, sizeof to) < 0) n->sendFail++;
        }
    }
    void receiveRx(float* const* rx, int nch, size_t frames, uint64_t sampleTime) override {
        st.rxBlocks++; now = sampleTime; poll();
        bool anyLink = (pri.enabled && !pri.failed) || (sec.enabled && !sec.failed);
        st.primaryLink = pri.enabled && !pri.failed; st.secondaryLink = sec.enabled && !sec.failed;
        st.primaryActive = st.primaryLink || !st.secondaryLink;
        for (int c = 0; c < nch; c++) {
            const Sub& s = subs[c];
            auto it = s.device.empty() ? peers.end() : peers.find(s.device);
            if (!anyLink || it == peers.end() || s.ch >= it->second.txChannels) { for (size_t i = 0; i < frames; i++) rx[c][i] = 0.f; continue; }
            Peer& pr = it->second;
            for (size_t i = 0; i < frames; i++) {
                uint64_t t = sampleTime + i;
                if (t < st.latencyFrames) { rx[c][i] = 0.f; continue; }
                uint64_t want = t - st.latencyFrames;                                          // local time of the frame to play
                rx[c][i] = (want < pr.writePos && pr.writePos - want <= kRing) ? pr.ring[s.ch][want & (kRing - 1)] : 0.f;
            }
        }
    }
    bool subscribe(int rxChannel, const std::string& device, int txChannel) override {
        if (rxChannel < 0 || rxChannel >= kDanteRx) return false;
        if (device.empty()) { subs[rxChannel] = {}; return true; }
        subs[rxChannel] = { device, txChannel }; return true;                                 // the peer may appear later
    }
    std::string subscription(int rxChannel) const override { const Sub& s = subs[rxChannel]; return s.device.empty() ? "" : s.device + "@" + std::to_string(s.ch + 1); }
    // "rx@Device:tx,..." (1-based); "rx@tx" alone patches this unit's own TX
    void patch(const std::string& spec) {
        std::string cur; auto apply = [&](const std::string& p) {
            size_t at = p.find('@'); if (at == std::string::npos) return;
            int rx = atoi(p.substr(0, at).c_str()); std::string rest = p.substr(at + 1); size_t col = rest.find(':');
            std::string dev = col == std::string::npos ? self : rest.substr(0, col); int tx = atoi((col == std::string::npos ? rest : rest.substr(col + 1)).c_str());
            if (rx >= 1 && rx <= kDanteRx && tx >= 1) subscribe(rx - 1, dev, tx - 1);
        };
        for (char ch : spec) { if (ch == ',') { apply(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty()) apply(cur);
    }
    const std::map<std::string, Peer>& peerList() const { return peers; }
    // HA remote (console <-> unit) frames: on the real network these ride Dante ConMon and the module hands them to the
    // host on UART B; here they are packet type 6 (to a named unit) / 7 (from a unit) carrying the raw UART frame.
    std::function<void(const uint8_t* frame, size_t len)> onHaRemoteFrame;   // console -> this unit
    void sendHaRemote(const uint8_t* frame, size_t len, const std::string& from = "") {
        std::vector<uint8_t> p(8 + 32 + 32 + len); memcpy(p.data(), "RIOV", 4); p[4] = 7; p[5] = 1; p[6] = p[7] = 0;
        putName(p.data() + 8, from.empty() ? self : from); putName(p.data() + 40, ""); memcpy(p.data() + 72, frame, len);
        for (Net* n : { &pri, &sec }) { if (!n->enabled || n->failed) continue; sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(n->port); to.sin_addr.s_addr = inet_addr(n->group.c_str()); sendto(n->sock, (const char*)p.data(), (int)p.size(), 0, (sockaddr*)&to, sizeof to); }
    }
    std::string describe() const {
        char b[256]; snprintf(b, sizeof b, "virtual Dante '%s': primary %s:%u%s%s, secondary %s", self.c_str(), pri.group.c_str(), pri.port, pri.failed ? " (FAILED)" : "", pri.ifAddr.empty() ? "" : (" via " + pri.ifAddr).c_str(),
                              sec.enabled ? (sec.group + ":" + std::to_string(sec.port) + (sec.failed ? " (FAILED)" : "")).c_str() : "off");
        return b;
    }
    std::string statsText() const {
        std::string s; char b[256];
        snprintf(b, sizeof b, "tx blocks %llu, rx blocks %llu, primary packets %llu (send failures %llu), secondary packets %llu (send failures %llu), peers %zu", (unsigned long long)st.txBlocks, (unsigned long long)st.rxBlocks, (unsigned long long)pri.pkts, (unsigned long long)pri.sendFail, (unsigned long long)sec.pkts, (unsigned long long)sec.sendFail, peers.size()); s += b;
        for (auto& [n, p] : peers) { snprintf(b, sizeof b, "\n[dante]   peer '%s': %d tx ch @ %u Hz%s, lost %llu, duplicates %llu, late %llu, resyncs %llu", n.c_str(), p.txChannels, p.fs, p.clockLeader ? " (clock leader)" : "", (unsigned long long)p.lost, (unsigned long long)p.dup, (unsigned long long)p.late, (unsigned long long)p.resyncs); s += b; }
        return s;
    }
    uint64_t wallMs() const { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

private:
    static constexpr size_t kRing = 1 << 16;
    struct Sub { std::string device; int ch = 0; };
    static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); } static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); } static void put64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
    static uint16_t get16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; } static uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; } static uint64_t get64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
    static void putName(uint8_t* p, const std::string& s) { memset(p, 0, 32); memcpy(p, s.data(), std::min<size_t>(31, s.size())); }
    static std::string getName(const uint8_t* p) { size_t n = 0; while (n < 32 && p[n]) n++; return std::string((const char*)p, n); }
    static void parse(const std::string& spec, Net& n) {
        if (spec.empty() || spec == "on") return;
        size_t at = spec.find('@'); std::string gp = spec; if (at != std::string::npos) { n.ifAddr = spec.substr(at + 1); gp = spec.substr(0, at); }
        size_t col = gp.find(':'); if (col != std::string::npos) { if (col) n.group = gp.substr(0, col); n.port = (uint16_t)atoi(gp.substr(col + 1).c_str()); } else if (!gp.empty()) n.group = gp;
    }
    bool bind(Net& n, std::string& err) {
        n.sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (n.sock < 0) { err = "socket: " + std::string(sockError()); return false; }
        int one = 1; setsockopt(n.sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(n.port); sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(n.sock, (sockaddr*)&sa, sizeof sa) < 0) { err = "bind port " + std::to_string(n.port) + ": " + sockError(); return false; }
        ip_mreq mr{}; mr.imr_multiaddr.s_addr = inet_addr(n.group.c_str()); mr.imr_interface.s_addr = n.ifAddr.empty() ? htonl(INADDR_ANY) : inet_addr(n.ifAddr.c_str());
        if (setsockopt(n.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) < 0) { err = "join " + n.group + ": " + sockError(); return false; }
        if (!n.ifAddr.empty()) { in_addr ia{}; ia.s_addr = inet_addr(n.ifAddr.c_str()); setsockopt(n.sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof ia); }
        int loop = 1; setsockopt(n.sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);   // instances on one host hear each other
        unsigned char ttl = 1; setsockopt(n.sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
        int rcv = 4 << 20; setsockopt(n.sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof rcv);
        int snd = 4 << 20; setsockopt(n.sock, SOL_SOCKET, SO_SNDBUF, (const char*)&snd, sizeof snd);   // real-time pacing sends in bursts after a stall
        sockNonBlocking(n.sock);
        return true;
    }
    void pumpAnnounce() {
        uint64_t ms = wallMs(); if (ms - lastAnnounceMs < 1000) return; lastAnnounceMs = ms;
        uint8_t p[8 + 32 + 4 + 2 + 2 + 8 + 1]; memcpy(p, "RIOV", 4); p[4] = 1; p[5] = 1; p[6] = p[7] = 0;
        putName(p + 8, self); put32(p + 40, st.fs); put16(p + 44, kDanteTx); put16(p + 46, kDanteRx); put64(p + 48, now); p[56] = st.clockMaster ? 1 : 0;
        for (Net* n : { &pri, &sec }) { if (!n->enabled || n->failed) continue; sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(n->port); to.sin_addr.s_addr = inet_addr(n->group.c_str()); sendto(n->sock, (const char*)p, sizeof p, 0, (sockaddr*)&to, sizeof to); }
        // peers not heard for 5 s disappear
        for (auto it = peers.begin(); it != peers.end();) { if (ms - it->second.lastSeenMs > 5000) it = peers.erase(it); else ++it; }
    }
    void poll() {
        for (Net* n : { &pri, &sec }) {
            if (!n->enabled) continue;
            for (int k = 0; k < 256; k++) {
                sockaddr_in from{}; socklen_t fl = sizeof from;
                ssize_t r = recvfrom(n->sock, (char*)rbuf, sizeof rbuf, 0, (sockaddr*)&from, &fl);
                if (r <= 0) break;
                if (n->failed) continue;                                                   // a pulled cable: packets never arrive
                n->pkts++; n->lastRxMs = wallMs();
                handle(rbuf, (size_t)r, n == &sec);
            }
        }
    }
    void handle(const uint8_t* p, size_t len, bool /*secondary*/) {
        if (len < 8 || memcmp(p, "RIOV", 4) != 0) return;
        uint8_t type = p[4];
        if (type == 1 && len >= 57) {
            std::string nm = getName(p + 8); if (nm == self) return;
            Peer& pr = peers[nm]; pr.name = nm; pr.fs = get32(p + 40); pr.txChannels = get16(p + 44); pr.rxChannels = get16(p + 46); pr.clockLeader = p[56] != 0; pr.lastSeenMs = wallMs();
            if ((int)pr.ring.size() != pr.txChannels) pr.ring.assign(pr.txChannels, std::vector<float>(kRing, 0.f));
        } else if (type == 2 && len >= 60) {
            std::string nm = getName(p + 8); if (nm == self) return;
            uint32_t fs = get32(p + 40); int nch = get16(p + 44); size_t frames = get16(p + 46); uint64_t t = get64(p + 48); uint32_t seq = get32(p + 56);
            if (len < 60 + frames * nch * 4 || nch <= 0 || nch > 64 || frames == 0 || frames > 512) return;
            Peer& pr = peers[nm]; pr.name = nm; pr.fs = fs; pr.lastSeenMs = wallMs();
            if (pr.txChannels != nch) { pr.txChannels = nch; pr.ring.assign(nch, std::vector<float>(kRing, 0.f)); }
            if (pr.haveSeq) {                                                               // redundancy: the same block arrives on both networks
                int32_t d = (int32_t)(seq - pr.lastSeq);
                if (d <= 0) { pr.dup++; return; }
                if (d > 1) pr.lost += (uint64_t)(d - 1);
            }
            pr.lastSeq = seq; pr.haveSeq = true;
            // map the sender's sample time into our time: offset learnt from the first block, re-learnt when the stream
            // drifts out of the latency window (both emulations run on their own clocks - this stands in for the
            // word-clock distribution; a resync is audible as a short dropout and is counted)
            int64_t local = (int64_t)now - (int64_t)t;                                      // offset so that the block lands "now"
            if (!pr.haveOffset) { pr.offset = local; pr.haveOffset = true; }
            int64_t landing = (int64_t)t + pr.offset;                                       // local time of the block's first frame
            int64_t lead = landing - (int64_t)now;                                          // how far ahead of "now" it lands
            // re-learn the offset only when the stream has drifted well outside the window (the two emulations' real-time
            // pacing jitters by a few ms); blocks that merely arrive late for the receive latency are counted as late
            int64_t jitter = (int64_t)st.fs / 50;                                           // 20 ms
            if (lead < -((int64_t)st.latencyFrames + jitter) || lead > (int64_t)(kRing / 2)) { pr.offset = local; landing = (int64_t)now; pr.resyncs++; }
            if (landing + (int64_t)frames <= (int64_t)now - (int64_t)st.latencyFrames) pr.late++;   // too late to be played (still stored for statistics)
            const uint8_t* q = p + 60;
            for (size_t i = 0; i < frames; i++) for (int c = 0; c < nch; c++) { int32_t s; memcpy(&s, q, 4); q += 4; pr.ring[c][(uint64_t)(landing + (int64_t)i) & (kRing - 1)] = (float)((s >> 8) / 8388608.0); }
            pr.writePos = (uint64_t)(landing + (int64_t)frames);
        } else if (type == 6 && len > 72) {                                             // HA remote frame addressed to a unit: name[32] = target
            if (getName(p + 8) != self) return;
            if (onHaRemoteFrame) onHaRemoteFrame(p + 72, len - 72);
        } else if (type == 3 && len >= 8 + 32 + 2 + 32 + 2) {
            if (getName(p + 8) != self) return;
            int rx = get16(p + 40); std::string dev = getName(p + 42); int tx = get16(p + 74);
            if (rx >= 1 && rx <= kDanteRx) { subscribe(rx - 1, dev, tx - 1); fprintf(stderr, "[dante] control: RX %d <- %s\n", rx, dev.empty() ? "(none)" : (dev + "@" + std::to_string(tx)).c_str()); }
        }
    }

    std::string self; Net pri, sec; DanteStatus st; Sub subs[kDanteRx]; std::map<std::string, Peer> peers;
    uint64_t now = 0, lastAnnounceMs = 0; uint32_t txSeq = 0; std::vector<uint8_t> pkt; uint8_t rbuf[16384];
    std::mt19937 rng{ 7 }; std::uniform_real_distribution<double> drop{ 0.0, 100.0 };
};

} // namespace rio
