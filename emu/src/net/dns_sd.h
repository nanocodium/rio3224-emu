// Multicast DNS / DNS-SD (RFC 6762 / 6763) responder and resolver on one UDP socket (224.0.0.251:5353).
//
// The Controller-interoperable Dante endpoint (dante_endpoint.*) publishes its services with it (the device
// instance in `_netaudio-arc._udp` / `_netaudio-cmc._udp`, one `_netaudio-chan._udp` instance per TX channel) and
// resolves the transmit channels of other devices when the controller subscribes a receive channel.  Plain C++,
// no third-party code; only the record layouts of the two RFCs.  The socket shares port 5353 with the host's other
// mDNS users (Bonjour, Dante Controller's own browser), so every answer is sent as multicast, which is what the
// standard prescribes for shared records anyway.
#pragma once
#include "../platform.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dnssd {

enum : uint16_t { TYPE_A = 1, TYPE_PTR = 12, TYPE_TXT = 16, TYPE_SRV = 33, TYPE_ANY = 255, CLASS_IN = 1 };

struct Record {
    std::string name;             // owner, dotted ("x._netaudio-arc._udp.local"); labels may contain dots and '@' - they are kept verbatim
    uint16_t type = 0;
    uint32_t ttl = 4500;
    bool flush = true;            // cache-flush bit (unique record)
    // rdata by type
    std::string target;           // PTR / SRV target name
    uint16_t port = 0;            // SRV
    uint32_t a = 0;               // A (host order)
    std::vector<std::string> txt; // TXT entries
};

// one decoded question / answer out of a received message
struct Question { std::string name; uint16_t type = 0; bool unicastReply = false; };
struct Answer { std::string name; uint16_t type = 0; uint32_t ttl = 0; std::string target; uint16_t port = 0; uint32_t a = 0; std::vector<std::string> txt; };
struct Message { uint16_t id = 0; bool response = false; std::vector<Question> questions; std::vector<Answer> answers; };

// Message encoder with name compression; splits into several datagrams when a packet would exceed maxBytes.
class Writer {
public:
    explicit Writer(size_t maxBytes = 1400) : limit(maxBytes) {}
    void begin(uint16_t id, bool response);
    bool question(const std::string& name, uint16_t type, bool unicast = false);
    bool answer(const Record& r);                      // returns false when the record did not fit (caller: flush and retry)
    void additional(const Record& r);                  // optional: counted in the additional section
    std::vector<uint8_t> finish();                     // the datagram (header counts filled in)
    size_t size() const { return buf.size(); }
    bool empty() const { return qd == 0 && an == 0 && ar == 0; }
private:
    size_t limit; std::vector<uint8_t> buf; std::map<std::string, uint16_t> offsets; uint16_t qd = 0, an = 0, ar = 0; bool inAdditional = false;
    bool putName(const std::string& name);
    void putRecord(const Record& r, size_t& mark);
    void put16(uint16_t v) { buf.push_back(v >> 8); buf.push_back(v & 0xFF); }
    void put32(uint32_t v) { put16(v >> 16); put16(v & 0xFFFF); }
};

bool parse(const uint8_t* p, size_t n, Message& out);   // decodes questions + answers (+ additionals as answers)

// split "a.b.c" into labels, honouring '\.' escapes (service instance names may contain dots)
std::vector<std::string> labels(const std::string& name);
bool nameEq(const std::string& a, const std::string& b);   // case-insensitive comparison of dotted names

class Responder {
public:
    bool open(uint32_t ifAddrHostOrder, std::string& err);   // joins 224.0.0.251 on that interface
    void close();
    bool isOpen() const { return sock >= 0; }
    // the record set we answer for (replace wholesale; call announce() afterwards)
    void setRecords(std::vector<Record> recs) { records = std::move(recs); }
    const std::vector<Record>& currentRecords() const { return records; }
    void announce();                                    // unsolicited multicast of every record (RFC 6762 8.3), also used after a change
    void goodbye();                                     // every record with TTL 0
    void rejoin();                                      // leave and re-join the group (Windows drops existing memberships when an interface appears / changes)
    uint64_t lastRxMs = 0;                              // when anything was last received (silence = membership probably lost)
    void poll(uint64_t nowMs);                          // receive + answer queries, deliver responses to the resolver callbacks
    // resolver: ask for name/type; replies arrive in onAnswer (from any responder on the link), repeated until stop
    void query(const std::string& name, uint16_t type);
    std::function<void(const Answer&, uint32_t fromIp)> onAnswer;
    // statistics for the diagnostics page
    uint64_t rxPackets = 0, rxQueries = 0, rxResponses = 0, txPackets = 0, answered = 0, lastQueryMs = 0;
    std::string lastQuery;                              // "name type" of the last question we answered
    std::function<void(const uint8_t*, size_t, uint32_t srcIp, uint16_t srcPort, uint32_t dstIp, uint16_t dstPort)> tap;   // packet tracing hook (both directions)
private:
    int sock = -1; uint32_t ifAddr = 0; std::vector<Record> records; uint64_t lastAnnounceMs = 0; int announcesLeft = 0;
    void send(const std::vector<uint8_t>& pkt, uint32_t toIp, uint16_t toPort);
    void answerQuery(const Message& m, uint32_t fromIp, uint16_t fromPort, uint64_t nowMs);
    void addMatching(Writer& w, const Question& q, std::vector<const Record*>& extra);
};

} // namespace dnssd
