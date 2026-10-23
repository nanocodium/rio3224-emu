// Parallel NOR flash on CS0: two Spansion S29GL-P style x16 chips side by side on the 32-bit bus
// (the firmware's fmd_amd32 driver sends doubled 16-bit commands, e.g. 0x00AA00AA to byte offset 0x1554,
// CFI query 0x00980098 to 0x154, and reads the CFI table as the low byte of each 32-bit word).
// Supports: read array, CFI query, autoselect (IDs), word program, unlock bypass, write-buffer
// programming (0x25/0x29), sector erase, chip erase, reset. Program/erase complete instantly.
#pragma once
#include "bus.h"
#include <cstdint>
#include <string>
#include <functional>
#include <vector>

class NorFlash : public MmioDevice {
public:
    NorFlash(uint32_t totalBytes = 32u << 20, uint32_t sectorBytes = 256u << 10);
    const char* name() const override { return "NOR flash"; }
    uint32_t read(uint32_t addr, int size) override;
    void write(uint32_t addr, int size, uint32_t val) override;

    bool load(const std::string& path);      // raw image (size must match)
    bool save(const std::string& path);
    uint8_t* data() { return mem.data(); }
    uint32_t size() const { return (uint32_t)mem.size(); }
    bool dirty = false;
    bool log = false;
    uint64_t erases = 0, programs = 0;
    std::function<uint64_t()> nowMs;         // optional time source: logs when the firmware starts writing its parameter store (0x700000..)
    uint64_t lastStoreMs = ~0ull;

    // per-chip identification (x16 chip); replies are doubled onto both bus halves
    uint16_t mfrId = 0x0001, devId = 0x227E, extId1 = 0x2210, extId2 = 0x2201;   // S29GL064N (uniform 64 KB sectors)
private:
    enum Mode { READ, UNLOCK1, UNLOCK2, AUTOSELECT, QUERY, PROGRAM, ERASE1, ERASE2, ERASE3, BYPASS, BYPASS_PROG, WBUF_COUNT, WBUF_DATA, WBUF_CONFIRM };
    Mode mode = READ;
    bool bypass = false;
    std::vector<uint8_t> mem;
    uint32_t sectorSize;
    // write buffer state
    uint32_t wbSector = 0, wbCount = 0, wbLeft = 0;
    std::vector<std::pair<uint32_t, uint32_t>> wbWords;   // (addr, 32-bit value)
    uint8_t cfi[0x60];
    void buildCfi();
    uint32_t doubled(uint16_t v) const { return ((uint32_t)v << 16) | v; }
    uint32_t sliceRead(uint32_t full32, uint32_t addr, int size) const;   // pick the requested bytes out of a 32-bit word
    void programWord(uint32_t addr, int size, uint32_t val);
    void eraseSector(uint32_t addr);
};
