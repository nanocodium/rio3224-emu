// DSP / converter interface block on CS4 (0x10000000, firmware uses the cache-through mirror 0x30000000).
// Recovered from RIO2_PROG.BIN (FUN_0c13b6a2 / FUN_0c13b522 register transport, FUN_0c13a674 RAM test):
//   +0x00..+0x7C  32 data words
//   +0x80  command: bits 15:0 register address, bits 20:16 word count-1, bit 30 error, bit 31 busy
//   +0x84  device select (bits 7:0), bit 8 = read (data words are filled from the device)
//   +0x88  configuration (0x238 written at init)
//   +0x94  control (0x80 = enable)
//   +0xA8  serial transmit word: 32-bit frames clocked out to the converter / head-amp chips
// Devices seen: 0 (?), 4, 6, 7 (DSP coefficient memory, regs 0x4A.. per channel), 8, 0xD (DSP control regs 0x10..0x12)
#pragma once
#include "bus.h"
#include <cstdio>
#include <cstdint>
#include <map>
#include <vector>
#include <functional>

class DspIf : public MmioDevice {
public:
    const char* name() const override { return "CS4"; }

    uint32_t data[32] = {};
    uint32_t cmd = 0, dev = 0, cfg = 0, ctrl = 0;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> regs;       // device -> register -> value
    std::vector<uint32_t> serialWords;                           // everything written to +0xA8
    std::function<void(uint32_t word)> onSerial;
    std::function<uint16_t(int index)> meterRead;                 // +0x8E meter port: write index, read level (engine)
    uint8_t meterIndex = 0;
    bool log = false, logSerial = false;
    uint64_t transfers = 0;
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x1000);

    uint32_t read(uint32_t addr, int size) override {
        uint32_t o = addr & 0x03FFFFFF, v;
        if (o < 0x80) v = data[o >> 2];
        else switch (o) {
        case 0x80: v = cmd & ~0x80000000u; break;          // never busy
        case 0x84: v = dev; break;
        case 0x88: v = cfg; break;
        case 0x94: v = ctrl; break;
        case 0x8E: v = meterRead ? meterRead(meterIndex) : 0; break;
        default: v = 0; for (int i = 0; i < size; i++) v = (v << 8) | at(o + i); break;
        }
        if (o != 0x8E) { if (size == 2) v = (o & 2) ? (v & 0xFFFF) : (v >> 16);
        else if (size == 1) v = (v >> (8 * (3 - (o & 3)))) & 0xFF; }
        if (log) fprintf(stderr, "[CS4] rd %08X -> %0*X\n", addr, size * 2, v);
        return v;
    }
    void write(uint32_t addr, int size, uint32_t val) override {
        uint32_t o = addr & 0x03FFFFFF;
        if (log) fprintf(stderr, "[CS4] wr %08X <- %0*X\n", addr, size * 2, val);
        if (o < 0x80) { data[o >> 2] = val; return; }
        switch (o) {
        case 0x80: cmd = val; break;
        case 0x84: dev = val; transfer(); break;
        case 0x88: cfg = val; break;
        case 0x94: ctrl = val; break;
        case 0x8E: meterIndex = (uint8_t)(val & 0xFF); break;      // bit 15 = "muted" flag from the firmware, index in the low byte
        case 0xA8: {                                  // burst write: word goes to regs[dev][cmd], address auto-increments (FUN_0c13b8da)
            uint32_t reg = cmd & 0xFFFF, d = dev & 0xFF;
            regs[d][reg] = val; cmd = (cmd & ~0xFFFFu) | ((reg + 1) & 0xFFFF);
            serialWords.push_back(val);
            if (logSerial) fprintf(stderr, "[CS4] burst dev %X reg %04X <- %08X\n", d, reg, val);
            if (onSerial) onSerial(val);
            break; }
        default: for (int i = size - 1; i >= 0; i--) { at(o + i) = val & 0xFF; val >>= 8; } break;
        }
    }
private:
    uint8_t& at(uint32_t o) { static uint8_t dummy; return o < mem.size() ? mem[o] : dummy; }
    void transfer() {
        uint32_t reg = cmd & 0xFFFF, n = ((cmd >> 16) & 0x1F) + 1, d = dev & 0xFF;
        bool rd = dev & 0x100;
        auto& r = regs[d];
        if (rd) for (uint32_t i = 0; i < n; i++) { auto it = r.find(reg + i); data[i] = it == r.end() ? 0 : it->second; }
        else    for (uint32_t i = 0; i < n; i++) r[reg + i] = data[i];   // dev 0xD regs 0x10..0x12: DSP control words the host read-modify-writes (0x11: 0x00011414 / 0x00001414)
        transfers++;
        if (logSerial) {
            fprintf(stderr, "[CS4] %s dev %X reg %04X x%u:", rd ? "read " : "write", d, reg, n);
            for (uint32_t i = 0; i < n && i < 6; i++) fprintf(stderr, " %08X", data[i]);
            fprintf(stderr, "%s\n", n > 6 ? " ..." : "");
        }
        cmd &= ~0xC0000000u;     // done, no error
    }
};
