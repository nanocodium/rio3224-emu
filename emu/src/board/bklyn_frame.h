// Yamaha "BCA" serial framing between the Rio host MCU and the Dante module's `dac` process (DNT_UART_A).
// Recovered from dac (dac_cmd.c sendCommand/createWriteData, dac_serial_task.c):
//   frame = 0xFF  escaped(header[8] + payload)  escaped(checksum)  0xFE
//   escape: bytes 0xFD/0xFE/0xFF are preceded by 0xFD; checksum = sum of all emitted bytes between the preamble
//   and the checksum itself (including escape bytes) modulo 256.
//   header: category(1) cmd(1) ver(1)=1 seq(1) result(1) origin(1) length(2, big endian) where length is the
//   payload length plus the number of payload bytes that need escaping.
//   categories: 1 SET, 2 GET, 3 NOTIFY, |0x80 for responses.
#pragma once
#include <cstdint>
#include <vector>
#include <functional>

namespace bklyn {

enum Category : uint8_t { SET = 1, GET = 2, NOTIFY = 3, RES_BIT = 0x80 };
enum Code : uint8_t { PREAMBLE = 0xFF, POSTAMBLE = 0xFE, ESCAPE = 0xFD };

struct Header {
    uint8_t category = 0, cmd = 0, ver = 1, seq = 0, result = 0, origin = 0;
    uint16_t length = 0;             // host order
};

struct Frame { Header h; std::vector<uint8_t> payload; };

inline bool needsEscape(uint8_t b) { return b >= 0xFD; }

inline std::vector<uint8_t> encode(const Header& h, const uint8_t* payload, size_t n) {
    std::vector<uint8_t> raw;
    size_t esc = 0; for (size_t i = 0; i < n; i++) if (needsEscape(payload[i])) esc++;
    uint16_t len = (uint16_t)(n + esc);
    raw = { h.category, h.cmd, h.ver, h.seq, h.result, h.origin, (uint8_t)(len >> 8), (uint8_t)len };
    raw.insert(raw.end(), payload, payload + n);
    std::vector<uint8_t> out; out.reserve(raw.size() * 2 + 4);
    out.push_back(PREAMBLE);
    uint32_t sum = 0;
    for (uint8_t b : raw) { if (needsEscape(b)) { out.push_back(ESCAPE); sum += ESCAPE; } out.push_back(b); sum += b; }
    uint8_t ck = sum & 0xFF;
    if (needsEscape(ck)) out.push_back(ESCAPE);
    out.push_back(ck);
    out.push_back(POSTAMBLE);
    return out;
}

// byte-wise decoder; onFrame gets the unescaped header + payload once a complete, checksum-valid frame arrived
class Decoder {
public:
    std::function<void(const Frame&)> onFrame;
    std::function<void(const char* why)> onError;
    uint64_t frames = 0, errors = 0;
    void feed(uint8_t b) {
        if (inFrame && esc) { esc = false; buf.push_back(b); sum += b; return; }   // escaped byte (FD FD/FE/FF) is data, like dac's decoder
        if (b == PREAMBLE) { inFrame = true; esc = false; buf.clear(); sum = 0; return; }
        if (!inFrame) return;
        if (b == POSTAMBLE) { finish(); inFrame = false; return; }
        if (b == ESCAPE) { esc = true; sum += b; return; }
        buf.push_back(b); sum += b;
    }
private:
    bool inFrame = false, esc = false; std::vector<uint8_t> buf; uint32_t sum = 0;
    void finish() {
        if (buf.size() < 9) { fail("short frame"); return; }
        uint8_t ck = buf.back(); buf.pop_back();
        uint8_t expect = (sum - ck) & 0xFF;          // sum included the checksum byte (and its escape, if any)
        // the escape before an escaped checksum was counted in sum but is not part of the covered bytes; dac
        // computes the checksum over everything before it, so the escape of the checksum itself is excluded
        if (needsEscape(ck)) expect = (sum - ck - ESCAPE) & 0xFF;
        if (ck != expect) { fail("checksum"); return; }
        Frame f; f.h.category = buf[0]; f.h.cmd = buf[1]; f.h.ver = buf[2]; f.h.seq = buf[3]; f.h.result = buf[4]; f.h.origin = buf[5];
        f.h.length = (buf[6] << 8) | buf[7];
        f.payload.assign(buf.begin() + 8, buf.end());
        frames++;
        if (onFrame) onFrame(f);
    }
    void fail(const char* why) { errors++; if (onError) onError(why); }
};

} // namespace bklyn
