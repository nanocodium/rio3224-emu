// Physical address space of the SH7203 as seen by the CPU and DMAC.
// Regions are registered with a base/size and either a RAM backing store or an MMIO handler.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <memory>

struct MmioDevice {
    virtual ~MmioDevice() = default;
    virtual uint32_t read(uint32_t addr, int size) = 0;           // size: 1,2,4 ; returns zero-extended value
    virtual void write(uint32_t addr, int size, uint32_t val) = 0;
    virtual const char* name() const = 0;
    virtual void tick(uint64_t cycles) {}
};

class Bus {
public:
    struct Region {
        uint32_t base, size;
        uint8_t* ram = nullptr;        // if non-null: plain memory (big-endian bytes)
        bool readonly = false;
        MmioDevice* dev = nullptr;     // else: device
        std::string name;
    };

    void addRam(uint32_t base, uint32_t size, const std::string& name, bool readonly = false);
    void addDevice(uint32_t base, uint32_t size, MmioDevice* dev);
    uint8_t* ramPtr(uint32_t addr);   // nullptr if not RAM

    uint8_t  read8(uint32_t a);
    uint16_t read16(uint32_t a);
    uint32_t read32(uint32_t a);
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);
    void write32(uint32_t a, uint32_t v);

    // Translate CPU address into the physical map (SH7203: 0x00000000..0x1FFFFFFF external space,
    // mirrored cache-through at 0x20000000..0x3FFFFFFF; 0xFFF80000 RAM; 0xFFFC0000 peripherals).
    static inline uint32_t phys(uint32_t a) {
        if (a < 0x40000000u) return a & 0x1FFFFFFFu;
        return a;
    }

    // hooks for unmapped access reporting
    std::function<void(uint32_t addr, int size, bool write, uint32_t val)> onUnmapped;
    uint64_t unmappedCount = 0;
    // memory watch: any access overlapping [watchLo, watchHi) is reported
    uint32_t watchLo = 0, watchHi = 0;
    std::function<void(uint32_t addr, int size, bool write, uint32_t val)> onWatch;

    std::vector<Region> regions;
private:
    Region* find(uint32_t a);
    uint32_t readAny(uint32_t a, int size);
    void writeAny(uint32_t a, int size, uint32_t v);
    Region* lastHit = nullptr;
};
