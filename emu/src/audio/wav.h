// Minimal RIFF/WAVE reader and writer (PCM 16/24/32-bit integer and 32-bit float, any channel count).
#pragma once
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rio {

class WavReader {
public:
    bool open(const std::string& path) {
        f = fopen(path.c_str(), "rb");
        if (!f) return false;
        uint8_t h[12];
        if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { close(); return false; }
        while (true) {
            uint8_t ch[8];
            if (fread(ch, 1, 8, f) != 8) { close(); return false; }
            uint32_t sz = le32(ch + 4);
            if (!memcmp(ch, "fmt ", 4)) {
                std::vector<uint8_t> fmt(sz);
                if (fread(fmt.data(), 1, sz, f) != sz) { close(); return false; }
                format = le16(fmt.data()); channels = le16(fmt.data() + 2); sampleRate = le32(fmt.data() + 4);
                bits = le16(fmt.data() + 14);
                if (format == 0xFFFE && sz >= 40) format = le16(fmt.data() + 24);   // WAVE_FORMAT_EXTENSIBLE: sub format GUID's first word
                if (sz & 1) fseek(f, 1, SEEK_CUR);
            } else if (!memcmp(ch, "data", 4)) {
                dataBytes = sz; frameBytes = channels * (bits / 8); framesTotal = frameBytes ? dataBytes / frameBytes : 0;
                dataStart = ftell(f);
                return channels > 0 && (format == 1 || format == 3) && (bits == 16 || bits == 24 || bits == 32);
            } else fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
    // reads up to n frames into planar float buffers (ch[c][i]); returns frames read (0 at end, unless looping)
    size_t read(float* const* ch, int nch, size_t n) {
        if (!f) return 0;
        size_t done = 0;
        std::vector<uint8_t> buf;
        while (done < n) {
            size_t want = n - done;
            if (framesRead >= framesTotal) { if (!loop || framesTotal == 0) break; fseek(f, dataStart, SEEK_SET); framesRead = 0; }
            size_t avail = framesTotal - framesRead; if (want > avail) want = avail;
            buf.resize(want * frameBytes);
            size_t got = fread(buf.data(), frameBytes, want, f);
            if (got == 0) break;
            for (size_t i = 0; i < got; i++)
                for (int c = 0; c < channels; c++) {
                    float v = sample(buf.data() + i * frameBytes + c * (bits / 8));
                    if (c < nch) ch[c][done + i] = v;
                }
            framesRead += got; done += got;
        }
        for (size_t i = done; i < n; i++) for (int c = 0; c < nch; c++) ch[c][i] = 0.f;   // pad with silence
        return done;
    }
    bool eof() const { return !loop && framesRead >= framesTotal; }
    void close() { if (f) fclose(f); f = nullptr; }
    ~WavReader() { close(); }
    int channels = 0, bits = 0, format = 0; uint32_t sampleRate = 0; size_t framesTotal = 0; bool loop = false;
private:
    FILE* f = nullptr; uint32_t dataBytes = 0; size_t frameBytes = 0, framesRead = 0; long dataStart = 0;
    static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    static uint32_t le32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
    float sample(const uint8_t* p) const {
        if (format == 3) { float v; memcpy(&v, p, 4); return v; }
        if (bits == 16) return (float)(int16_t)le16(p) / 32768.f;
        if (bits == 24) { int32_t v = (int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8; return (float)v / 8388608.f; }
        return (float)(int32_t)le32(p) / 2147483648.f;
    }
};

class WavWriter {
public:
    // bits: 24 (integer) or 32 (float)
    bool open(const std::string& path, int nch, uint32_t fs, int bitsPerSample = 24) {
        f = fopen(path.c_str(), "wb"); if (!f) return false;
        channels = nch; sampleRate = fs; bits = bitsPerSample; frames = 0;
        writeHeader(); return true;
    }
    void write(const float* const* ch, int nch, size_t n) {
        if (!f) return;
        std::vector<uint8_t> buf(n * channels * (bits / 8));
        uint8_t* p = buf.data();
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < channels; c++) {
                float v = c < nch ? ch[c][i] : 0.f;
                if (bits == 32) { memcpy(p, &v, 4); p += 4; }
                else {
                    if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
                    int32_t s = (int32_t)lrintf(v * 8388607.f);
                    p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); p[2] = (uint8_t)(s >> 16); p += 3;
                }
            }
        fwrite(buf.data(), 1, buf.size(), f); frames += n;
    }
    void close() { if (!f) return; writeHeader(); fclose(f); f = nullptr; }
    ~WavWriter() { close(); }
    size_t frames = 0;
private:
    FILE* f = nullptr; int channels = 0, bits = 24; uint32_t sampleRate = 48000;
    void put16(uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, f); }
    void put32(uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
    void writeHeader() {
        uint32_t dataBytes = (uint32_t)(frames * channels * (bits / 8));
        fseek(f, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, f); put32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); put32(16); put16(bits == 32 ? 3 : 1); put16((uint16_t)channels); put32(sampleRate);
        put32(sampleRate * channels * (bits / 8)); put16((uint16_t)(channels * (bits / 8))); put16((uint16_t)bits);
        fwrite("data", 1, 4, f); put32(dataBytes);
        fseek(f, 0, SEEK_END);
    }
};

} // namespace rio
