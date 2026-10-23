#include "flash.h"
#include <cstdio>
#include <cstring>
#include <fstream>

NorFlash::NorFlash(uint32_t totalBytes, uint32_t sectorBytes) : mem(totalBytes, 0xFF), sectorSize(sectorBytes) {
    buildCfi();
}

void NorFlash::buildCfi() {
    memset(cfi, 0, sizeof cfi);
    uint32_t chipBytes = size() / 2;                 // per x16 chip
    int sizeExp = 0; while ((1u << sizeExp) < chipBytes) sizeExp++;
    uint32_t chipSector = sectorSize / 2;
    uint32_t nBlocks = chipBytes / chipSector;
    cfi[0x10] = 'Q'; cfi[0x11] = 'R'; cfi[0x12] = 'Y';
    cfi[0x13] = 0x02; cfi[0x14] = 0x00;             // primary command set: AMD/Fujitsu standard
    cfi[0x15] = 0x40; cfi[0x16] = 0x00;             // address of primary extended table
    cfi[0x17] = 0x00; cfi[0x18] = 0x00;             // no alternate command set
    cfi[0x19] = 0x00; cfi[0x1A] = 0x00;
    cfi[0x1B] = 0x27; cfi[0x1C] = 0x36;             // Vcc 2.7..3.6 V
    cfi[0x1D] = 0x00; cfi[0x1E] = 0x00;             // no Vpp
    cfi[0x1F] = 0x06; cfi[0x20] = 0x06; cfi[0x21] = 0x09; cfi[0x22] = 0x13;   // typical timeouts (2^n us/ms)
    cfi[0x23] = 0x03; cfi[0x24] = 0x05; cfi[0x25] = 0x03; cfi[0x26] = 0x02;   // max timeouts
    cfi[0x27] = (uint8_t)sizeExp;                   // device size 2^n bytes
    cfi[0x28] = 0x02; cfi[0x29] = 0x00;             // interface: x8/x16 async
    cfi[0x2A] = 0x06; cfi[0x2B] = 0x00;             // max multi-byte write 2^6 = 64 bytes
    cfi[0x2C] = 0x01;                               // one erase block region
    cfi[0x2D] = (uint8_t)((nBlocks - 1) & 0xFF); cfi[0x2E] = (uint8_t)((nBlocks - 1) >> 8);
    cfi[0x2F] = (uint8_t)((chipSector / 256) & 0xFF); cfi[0x30] = (uint8_t)((chipSector / 256) >> 8);
    // primary vendor-specific extended query
    cfi[0x40] = 'P'; cfi[0x41] = 'R'; cfi[0x42] = 'I'; cfi[0x43] = '1'; cfi[0x44] = '3';
    cfi[0x45] = 0x14; cfi[0x46] = 0x02; cfi[0x47] = 0x01; cfi[0x48] = 0x00; cfi[0x49] = 0x08;
    cfi[0x4A] = 0x00; cfi[0x4B] = 0x00; cfi[0x4C] = 0x02; cfi[0x4D] = 0xB5; cfi[0x4E] = 0xC5;
    cfi[0x4F] = 0x05; cfi[0x50] = 0x01;
}

bool NorFlash::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read((char*)mem.data(), mem.size());
    fprintf(stderr, "[flash] loaded %s (%zu bytes)\n", path.c_str(), (size_t)f.gcount());
    dirty = false;
    return true;
}

bool NorFlash::save(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)mem.data(), mem.size());
    dirty = false;
    return true;
}

uint32_t NorFlash::sliceRead(uint32_t full32, uint32_t addr, int size) const {
    // full32 is the value of the aligned 32-bit word containing addr (big-endian bus)
    int shift = (3 - (addr & 3)) * 8;
    switch (size) {
    case 1: return (full32 >> shift) & 0xFF;
    case 2: return (full32 >> ((2 - (addr & 2)) * 8)) & 0xFFFF;
    default: return full32;
    }
}

uint32_t NorFlash::read(uint32_t addr, int size) {
    uint32_t off = addr & (size_t)(mem.size() - 1);
    if (mode == READ || mode == UNLOCK1 || mode == UNLOCK2 || mode == BYPASS || mode == ERASE1 || mode == ERASE2 || mode == ERASE3) {
        if (mode != READ && mode != BYPASS) mode = READ;    // a read in the middle of a command sequence aborts it
        const uint8_t* p = &mem[off & ~3u];
        uint32_t w = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        return sliceRead(w, off, size);
    }
    uint32_t wordAddr = (off >> 2) & 0xFF;      // word address within the chip (both chips see the same)
    uint32_t v = 0;
    if (mode == AUTOSELECT) {
        switch (wordAddr) {
        case 0x0: v = doubled(mfrId); break;
        case 0x1: v = doubled(devId); break;
        case 0x2: v = doubled(0x0000); break;          // sector protection: unprotected
        case 0x3: v = doubled(0x0000); break;
        case 0xE: v = doubled(extId1); break;
        case 0xF: v = doubled(extId2); break;
        default:  v = doubled(0xFFFF);
        }
    } else if (mode == QUERY) {
        v = doubled(wordAddr < sizeof cfi ? cfi[wordAddr] : 0);
    } else {
        // program / write-buffer in progress: report "done" by returning array data (DQ7 = true data, DQ6 steady)
        const uint8_t* p = &mem[off & ~3u];
        v = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
    if (log) fprintf(stderr, "[flash] rd %08X (mode %d) -> %08X\n", addr, mode, v);
    return sliceRead(v, off, size);
}

void NorFlash::programWord(uint32_t addr, int size, uint32_t val) {
    uint32_t off = addr & (size_t)(mem.size() - 1);
    for (int i = size - 1; i >= 0; i--) { mem[off + i] &= (uint8_t)val; val >>= 8; }
    programs++; dirty = true;
}

void NorFlash::eraseSector(uint32_t addr) {
    uint32_t off = (addr & (size_t)(mem.size() - 1)) & ~(sectorSize - 1);
    memset(&mem[off], 0xFF, sectorSize);
    erases++; dirty = true;
    if (log) fprintf(stderr, "[flash] erase sector @%08X\n", off);
}

void NorFlash::write(uint32_t addr, int size, uint32_t val) {
    uint32_t off = addr & (size_t)(mem.size() - 1);
    uint32_t wordAddr = (off >> 2) & 0xFFF;                 // chip word address (low 12 bits matter for commands)
    uint32_t cmd = val & 0xFF;                                // commands are replicated on both halves; low byte is enough
    if (log) fprintf(stderr, "[flash] wr %08X size %d val %08X (mode %d)\n", addr, size, val, mode);
    // data phases first
    if (mode == PROGRAM || mode == BYPASS_PROG) {
        if (nowMs && off >= 0x700000 && off < 0x800000) { uint64_t t = nowMs(); if (lastStoreMs == ~0ull || t - lastStoreMs > 500) fprintf(stderr, "[flash] parameter-store write begins at %llu ms\n", (unsigned long long)t); lastStoreMs = t; }
        programWord(addr, size, val); mode = bypass ? BYPASS : READ; return;
    }
    if (mode == WBUF_COUNT) { wbCount = (val & 0xFFFF) + 1; wbLeft = wbCount; wbWords.clear(); mode = WBUF_DATA; return; }
    if (mode == WBUF_DATA) {
        wbWords.push_back({addr, val});
        if (--wbLeft == 0) mode = WBUF_CONFIRM;
        return;
    }
    if (mode == WBUF_CONFIRM) {
        if (cmd == 0x29) { for (auto& w : wbWords) programWord(w.first, 4, w.second); }
        else if (log) fprintf(stderr, "[flash] write-buffer abort (cmd %02X)\n", cmd);
        mode = READ; return;
    }
    // reset works from any state
    if (cmd == 0xF0 || cmd == 0xFF) { mode = bypass ? BYPASS : READ; return; }
    switch (mode) {
    case READ: case AUTOSELECT: case QUERY:
        if (cmd == 0x98 && wordAddr == 0x55) { mode = QUERY; return; }
        if (cmd == 0xAA && wordAddr == 0x555) { mode = UNLOCK1; return; }
        if (mode != READ) { mode = READ; return; }
        break;
    case UNLOCK1:
        if (cmd == 0x55 && wordAddr == 0x2AA) { mode = UNLOCK2; return; }
        mode = READ; break;
    case UNLOCK2:
        if (wordAddr == 0x555) {
            switch (cmd) {
            case 0x90: mode = AUTOSELECT; return;
            case 0xA0: mode = PROGRAM; return;
            case 0x80: mode = ERASE1; return;
            case 0x20: bypass = true; mode = BYPASS; return;
            case 0x98: mode = QUERY; return;
            }
        }
        if (cmd == 0x25) { wbSector = off; mode = WBUF_COUNT; return; }     // write to buffer (sector address)
        mode = READ; break;
    case ERASE1:
        if (cmd == 0xAA && wordAddr == 0x555) { mode = ERASE2; return; }
        mode = READ; break;
    case ERASE2:
        if (cmd == 0x55 && wordAddr == 0x2AA) { mode = ERASE3; return; }
        mode = READ; break;
    case ERASE3:
        if (cmd == 0x30) { eraseSector(addr); mode = READ; return; }
        if (cmd == 0x10 && wordAddr == 0x555) { memset(mem.data(), 0xFF, mem.size()); erases++; dirty = true; mode = READ; return; }
        mode = READ; break;
    case BYPASS:
        if (cmd == 0xA0) { mode = BYPASS_PROG; return; }
        if (cmd == 0x90) { mode = BYPASS; bypass = false; mode = READ; return; }      // unlock bypass reset (0x90 then 0x00)
        if (cmd == 0x25) { wbSector = off; mode = WBUF_COUNT; return; }
        return;
    default: mode = READ; break;
    }
}
