#include "bus.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

void Bus::addRam(uint32_t base, uint32_t size, const std::string& name, bool readonly) {
    Region r; r.base = base; r.size = size; r.name = name; r.readonly = readonly;
    r.ram = new uint8_t[size](); 
    regions.push_back(r);
}
void Bus::addDevice(uint32_t base, uint32_t size, MmioDevice* dev) {
    Region r; r.base = base; r.size = size; r.dev = dev; r.name = dev->name();
    regions.push_back(r);
}
uint8_t* Bus::ramPtr(uint32_t a) {
    a = phys(a);
    Region* r = find(a);
    return (r && r->ram) ? r->ram + (a - r->base) : nullptr;
}
Bus::Region* Bus::find(uint32_t a) {
    if (lastHit && a - lastHit->base < lastHit->size) return lastHit;
    for (auto& r : regions) if (a - r.base < r.size) { lastHit = &r; return &r; }
    return nullptr;
}
uint32_t Bus::readAny(uint32_t a, int size) {
    a = phys(a);
    if (watchHi && a < watchHi && a + size > watchLo && onWatch) { Region* rr = find(a); uint32_t v = 0; if (rr && rr->ram) { const uint8_t* p = rr->ram + (a - rr->base); for (int i = 0; i < size; i++) v = (v << 8) | p[i]; } onWatch(a, size, false, v); }
    Region* r = find(a);
    if (!r) {
        unmappedCount++;
        if (onUnmapped) onUnmapped(a, size, false, 0);
        return 0;
    }
    if (r->ram) {
        const uint8_t* p = r->ram + (a - r->base);
        switch (size) {
            case 1: return p[0];
            case 2: return (p[0] << 8) | p[1];
            default: return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        }
    }
    return r->dev->read(a, size);
}
void Bus::writeAny(uint32_t a, int size, uint32_t v) {
    a = phys(a);
    if (watchHi && a < watchHi && a + size > watchLo && onWatch) onWatch(a, size, true, v);
    Region* r = find(a);
    if (!r) {
        unmappedCount++;
        if (onUnmapped) onUnmapped(a, size, true, v);
        return;
    }
    if (r->ram) {
        if (r->readonly) { if (r->dev) r->dev->write(a, size, v); return; }
        uint8_t* p = r->ram + (a - r->base);
        switch (size) {
            case 1: p[0] = v; break;
            case 2: p[0] = v >> 8; p[1] = v; break;
            default: p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; break;
        }
        return;
    }
    r->dev->write(a, size, v);
}
uint8_t  Bus::read8(uint32_t a)  { return readAny(a, 1); }
uint16_t Bus::read16(uint32_t a) { return readAny(a, 2); }
uint32_t Bus::read32(uint32_t a) { return readAny(a, 4); }
void Bus::write8(uint32_t a, uint8_t v)   { writeAny(a, 1, v); }
void Bus::write16(uint32_t a, uint16_t v) { writeAny(a, 2, v); }
void Bus::write32(uint32_t a, uint32_t v) { writeAny(a, 4, v); }
