// SH7203 compare match timer (2 channels) at 0xFFFEC000
#pragma once
#include "bus.h"
#include "intc.h"

class Cmt : public MmioDevice {
public:
    Cmt(Intc& intc, double pclkHz) : intc(intc), pclk(pclkHz) {}
    const char* name() const override { return "CMT"; }
    uint32_t read(uint32_t addr, int size) override;
    void write(uint32_t addr, int size, uint32_t val) override;
    void tick(uint64_t cpuCycles) override;      // called with elapsed CPU cycles; converts to Pφ
    double cpuHz = 200e6;
private:
    struct Ch { uint16_t csr = 0, cnt = 0, cor = 0xFFFF; double frac = 0; } ch[2];
    uint16_t cmstr = 0;
    Intc& intc;
    double pclk;
    void updateIrq(int i);
};
