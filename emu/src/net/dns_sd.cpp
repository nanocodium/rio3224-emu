#include "dns_sd.h"
#include <algorithm>
#include <cctype>

namespace dnssd {

static const char* kGroup = "224.0.0.251";
static const uint16_t kPort = 5353;

std::vector<std::string> labels(const std::string& name) {
    std::vector<std::string> out; std::string cur;
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        if (c == '\\' && i + 1 < name.size()) { cur += name[++i]; continue; }
        if (c == '.') { if (!cur.empty()) out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string lower(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)tolower((unsigned char)c); return r; }
bool nameEq(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

// ---- Writer ------------------------------------------------------------------------------------------------------

void Writer::begin(uint16_t id, bool response) {
    buf.clear(); offsets.clear(); qd = an = ar = 0; inAdditional = false;
    put16(id); put16(response ? 0x8400 : 0x0000); put16(0); put16(0); put16(0); put16(0);
}

bool Writer::putName(const std::string& name) {
    std::vector<std::string> ls = labels(name);
    for (size_t i = 0; i < ls.size(); i++) {
        std::string suffix; for (size_t k = i; k < ls.size(); k++) { suffix += lower(ls[k]); suffix += '.'; }
        auto it = offsets.find(suffix);
        if (it != offsets.end()) { put16(0xC000 | it->second); return true; }
        if (buf.size() < 0x3FFF) offsets[suffix] = (uint16_t)buf.size();
        if (ls[i].size() > 63) return false;
        buf.push_back((uint8_t)ls[i].size()); buf.insert(buf.end(), ls[i].begin(), ls[i].end());
    }
    buf.push_back(0);
    return true;
}

bool Writer::question(const std::string& name, uint16_t type, bool unicast) {
    size_t mark = buf.size(); auto saved = offsets;
    if (!putName(name) || buf.size() + 4 > limit) { buf.resize(mark); offsets = saved; return false; }
    put16(type); put16(CLASS_IN | (unicast ? 0x8000 : 0)); qd++; return true;
}

void Writer::putRecord(const Record& r, size_t& mark) {
    mark = buf.size();
    putName(r.name); put16(r.type); put16(CLASS_IN | (r.flush ? 0x8000 : 0)); put32(r.ttl);
    size_t lenPos = buf.size(); put16(0);
    switch (r.type) {
    case TYPE_PTR: putName(r.target); break;
    case TYPE_SRV: put16(0); put16(0); put16(r.port); putName(r.target); break;
    case TYPE_A:   put32(r.a); break;
    case TYPE_TXT:
        if (r.txt.empty()) buf.push_back(0);
        for (auto& t : r.txt) { size_t n = std::min<size_t>(255, t.size()); buf.push_back((uint8_t)n); buf.insert(buf.end(), t.begin(), t.begin() + n); }
        break;
    default: break;
    }
    uint16_t rdlen = (uint16_t)(buf.size() - lenPos - 2); buf[lenPos] = rdlen >> 8; buf[lenPos + 1] = rdlen & 0xFF;
}

bool Writer::answer(const Record& r) {
    size_t mark; auto saved = offsets; putRecord(r, mark);
    if (buf.size() > limit && (an + ar) > 0) { buf.resize(mark); offsets = saved; return false; }
    if (inAdditional) ar++; else an++;
    return true;
}
void Writer::additional(const Record& r) { inAdditional = true; size_t mark; auto saved = offsets; putRecord(r, mark); if (buf.size() > limit) { buf.resize(mark); offsets = saved; return; } ar++; }

std::vector<uint8_t> Writer::finish() {
    buf[4] = qd >> 8; buf[5] = qd & 0xFF; buf[6] = an >> 8; buf[7] = an & 0xFF; buf[10] = ar >> 8; buf[11] = ar & 0xFF;
    return buf;
}

// ---- parser -------------------------------------------------------------------------------------------------------

static bool readName(const uint8_t* p, size_t n, size_t& pos, std::string& out, int depth = 0) {
    if (depth > 8) return false;
    size_t cur = pos; bool jumped = false; out.clear(); int guard = 0;
    while (true) {
        if (cur >= n || ++guard > 128) return false;
        uint8_t len = p[cur];
        if (len == 0) { cur++; break; }
        if ((len & 0xC0) == 0xC0) {
            if (cur + 1 >= n) return false;
            size_t off = ((len & 0x3F) << 8) | p[cur + 1];
            if (!jumped) pos = cur + 2;
            jumped = true;
            if (off >= cur) return false;
            cur = off; depth++; if (depth > 8) return false;
            continue;
        }
        if (cur + 1 + len > n) return false;
        if (!out.empty()) out += '.';
        for (size_t i = 0; i < len; i++) { char c = (char)p[cur + 1 + i]; if (c == '.' || c == '\\') out += '\\'; out += c; }
        cur += 1 + len;
    }
    if (!jumped) pos = cur;
    return true;
}

bool parse(const uint8_t* p, size_t n, Message& out) {
    if (n < 12) return false;
    out.id = (p[0] << 8) | p[1]; out.response = (p[2] & 0x80) != 0;
    size_t qd = (p[4] << 8) | p[5], an = (p[6] << 8) | p[7], ns = (p[8] << 8) | p[9], ar = (p[10] << 8) | p[11];
    size_t pos = 12;
    for (size_t i = 0; i < qd; i++) {
        Question q; if (!readName(p, n, pos, q.name) || pos + 4 > n) return false;
        q.type = (p[pos] << 8) | p[pos + 1]; q.unicastReply = (p[pos + 2] & 0x80) != 0; pos += 4; out.questions.push_back(q);
    }
    for (size_t i = 0; i < an + ns + ar; i++) {
        Answer a; if (!readName(p, n, pos, a.name) || pos + 10 > n) return false;
        a.type = (p[pos] << 8) | p[pos + 1]; a.ttl = ((uint32_t)p[pos + 4] << 24) | (p[pos + 5] << 16) | (p[pos + 6] << 8) | p[pos + 7];
        size_t rdlen = (p[pos + 8] << 8) | p[pos + 9]; pos += 10; if (pos + rdlen > n) return false;
        size_t rp = pos;
        switch (a.type) {
        case TYPE_PTR: readName(p, n, rp, a.target); break;
        case TYPE_SRV: if (rdlen >= 6) { a.port = (p[rp + 4] << 8) | p[rp + 5]; rp += 6; readName(p, n, rp, a.target); } break;
        case TYPE_A: if (rdlen == 4) a.a = ((uint32_t)p[rp] << 24) | (p[rp + 1] << 16) | (p[rp + 2] << 8) | p[rp + 3]; break;
        case TYPE_TXT: { size_t e = pos + rdlen; while (rp < e) { size_t l = p[rp]; if (rp + 1 + l > e) break; a.txt.emplace_back((const char*)p + rp + 1, l); rp += 1 + l; } break; }
        default: break;
        }
        pos += rdlen; out.answers.push_back(std::move(a));
    }
    return true;
}

// ---- Responder ----------------------------------------------------------------------------------------------------

bool Responder::open(uint32_t ifAddrHostOrder, std::string& err) {
    platformInit(); ifAddr = ifAddrHostOrder;
    sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { err = std::string("socket: ") + sockError(); return false; }
    int one = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof one);
#endif
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(kPort); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock, (sockaddr*)&sa, sizeof sa) < 0) { err = std::string("bind 5353: ") + sockError(); sockClose(sock); sock = -1; return false; }
    ip_mreq mr{}; mr.imr_multiaddr.s_addr = inet_addr(kGroup); mr.imr_interface.s_addr = htonl(ifAddr);
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) < 0) { err = std::string("join 224.0.0.251: ") + sockError(); sockClose(sock); sock = -1; return false; }
    in_addr ia{}; ia.s_addr = htonl(ifAddr); setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof ia);
    unsigned char ttl = 255; setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
    int loop = 1; setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof loop);
    int rcv = 1 << 20; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof rcv);
    sockNonBlocking(sock); lastRxMs = 0;
    return true;
}

void Responder::close() { if (sock >= 0) { sockClose(sock); sock = -1; } }

void Responder::rejoin() {
    if (sock < 0) return;
    ip_mreq mr{}; mr.imr_multiaddr.s_addr = inet_addr(kGroup); mr.imr_interface.s_addr = htonl(ifAddr);
    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mr, sizeof mr);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr);
    in_addr ia{}; ia.s_addr = htonl(ifAddr); setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof ia);
}

void Responder::send(const std::vector<uint8_t>& pkt, uint32_t toIp, uint16_t toPort) {
    if (sock < 0) return;
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(toPort); to.sin_addr.s_addr = htonl(toIp);
    if (sendto(sock, (const char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&to, sizeof to) > 0) txPackets++;
    if (tap) tap(pkt.data(), pkt.size(), ifAddr, kPort, toIp, toPort);
}

void Responder::announce() {
    // RFC 6762 8.3: at least two announcements one second apart; we send one now and schedule the repeats in poll()
    Writer w; w.begin(0, true);
    for (auto& r : records) { if (!w.answer(r)) { send(w.finish(), ntohl(inet_addr(kGroup)), kPort); w.begin(0, true); w.answer(r); } }
    if (!w.empty()) send(w.finish(), ntohl(inet_addr(kGroup)), kPort);
    announcesLeft = 2; lastAnnounceMs = 0;
}

void Responder::goodbye() {
    Writer w; w.begin(0, true);
    for (auto r : records) { r.ttl = 0; if (!w.answer(r)) { send(w.finish(), ntohl(inet_addr(kGroup)), kPort); w.begin(0, true); w.answer(r); } }
    if (!w.empty()) send(w.finish(), ntohl(inet_addr(kGroup)), kPort);
    announcesLeft = 0;
}

void Responder::query(const std::string& name, uint16_t type) {
    Writer w; w.begin(0, false); w.question(name, type, false); send(w.finish(), ntohl(inet_addr(kGroup)), kPort);
}

void Responder::addMatching(Writer& w, const Question& q, std::vector<const Record*>& extra) {
    for (auto& r : records) {
        if (!nameEq(r.name, q.name)) continue;
        if (q.type != TYPE_ANY && q.type != r.type) continue;
        if (!w.answer(r)) continue;
        answered++;
        // RFC 6763 12: a PTR answer brings the SRV, TXT and A records of the instance along; an SRV brings the A record
        if (r.type == TYPE_PTR) for (auto& x : records) if (nameEq(x.name, r.target) && (x.type == TYPE_SRV || x.type == TYPE_TXT)) extra.push_back(&x);
        if (r.type == TYPE_PTR || r.type == TYPE_SRV) {
            const Record* srv = &r; if (r.type == TYPE_PTR) { srv = nullptr; for (auto& x : records) if (nameEq(x.name, r.target) && x.type == TYPE_SRV) srv = &x; }
            if (srv) for (auto& x : records) if (nameEq(x.name, srv->target) && x.type == TYPE_A) extra.push_back(&x);
        }
    }
}

void Responder::answerQuery(const Message& m, uint32_t fromIp, uint16_t fromPort, uint64_t nowMs) {
    bool legacy = fromPort != kPort;                      // a one-shot resolver: answer unicast with its id
    Writer w; w.begin(legacy ? m.id : 0, true); std::vector<const Record*> extra; bool any = false;
    for (auto& q : m.questions) {
        size_t before = answered;
        if (legacy) w.question(q.name, q.type);
        addMatching(w, q, extra);
        if (answered != before) { any = true; lastQuery = q.name + " type " + std::to_string(q.type); lastQueryMs = nowMs; }
    }
    if (!any) return;
    std::sort(extra.begin(), extra.end()); extra.erase(std::unique(extra.begin(), extra.end()), extra.end());
    for (auto* r : extra) w.additional(*r);
    send(w.finish(), legacy ? fromIp : ntohl(inet_addr(kGroup)), legacy ? fromPort : kPort);
}

void Responder::poll(uint64_t nowMs) {
    if (sock < 0) return;
    if (announcesLeft > 0 && nowMs - lastAnnounceMs >= 1000) {
        lastAnnounceMs = nowMs; announcesLeft--;
        Writer w; w.begin(0, true);
        for (auto& r : records) { if (!w.answer(r)) { send(w.finish(), ntohl(inet_addr(kGroup)), kPort); w.begin(0, true); w.answer(r); } }
        if (!w.empty()) send(w.finish(), ntohl(inet_addr(kGroup)), kPort);
    }
    uint8_t buf[9000];
    for (int k = 0; k < 64; k++) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t r = recvfrom(sock, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (r <= 0) break;
        rxPackets++; lastRxMs = nowMs;
        uint32_t fromIp = ntohl(from.sin_addr.s_addr); uint16_t fromPort = ntohs(from.sin_port);
        if (tap) tap(buf, (size_t)r, fromIp, fromPort, ntohl(inet_addr(kGroup)), kPort);
        Message m; if (!parse(buf, (size_t)r, m)) continue;
        if (m.response) {
            rxResponses++;
            if (onAnswer) for (auto& a : m.answers) onAnswer(a, fromIp);
        } else {
            rxQueries++;
            answerQuery(m, fromIp, fromPort, nowMs);
        }
    }
}

} // namespace dnssd
