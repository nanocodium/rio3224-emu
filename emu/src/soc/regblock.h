// Generic register block: byte-addressable storage with optional per-access logging and read/write hooks.
// Used for peripherals that are modelled as "remember what was written" plus a few live bits.
#pragma once
#include "bus.h"
#include <vector>
#include <string>
#include <functional>
#include <cstdio>

class RegBlock : public MmioDevice {
public:
    RegBlock(const std::string& name, uint32_t base, uint32_t size) : nm(name), base(base), mem(size, 0) {}
    const char* name() const override { return nm.c_str(); }
    uint32_t read(uint32_t addr, int size) override {
        uint32_t v = 0;
        if (readHook) { uint32_t hv; if (readHook(addr, size, hv)) { if (log) fprintf(stderr, "[%s] rd %08X -> %0*X (hook)\n", nm.c_str(), addr, size * 2, hv); return hv; } }
        for (int i = 0; i < size; i++) v = (v << 8) | at(addr + i);
        if (log) fprintf(stderr, "[%s] rd %08X -> %0*X\n", nm.c_str(), addr, size * 2, v);
        return v;
    }
    void write(uint32_t addr, int size, uint32_t val) override {
        if (log) fprintf(stderr, "[%s] wr %08X <- %0*X\n", nm.c_str(), addr, size * 2, val);
        if (writeHook && writeHook(addr, size, val)) return;
        for (int i = size - 1; i >= 0; i--) { at(addr + i) = val & 0xFF; val >>= 8; }
    }
    uint8_t& at(uint32_t addr) { static uint8_t dummy; uint32_t o = addr - base; return o < mem.size() ? mem[o] : dummy; }
    void set16(uint32_t addr, uint16_t v) { at(addr) = v >> 8; at(addr + 1) = v & 0xFF; }
    void set8(uint32_t addr, uint8_t v) { at(addr) = v; }
    void set32(uint32_t addr, uint32_t v) { set16(addr, v >> 16); set16(addr + 2, v & 0xFFFF); }
    uint16_t get16(uint32_t addr) { return (at(addr) << 8) | at(addr + 1); }
    std::function<bool(uint32_t addr, int size, uint32_t& val)> readHook;      // return true to override
    std::function<bool(uint32_t addr, int size, uint32_t val)> writeHook;      // return true to swallow
    bool log = false;
    std::string nm;
    uint32_t base;
    std::vector<uint8_t> mem;
};
