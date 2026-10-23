// SH7203 multi-function timer pulse unit 2 (MTU2): channels 0-4 as free-running/compare-match counters.
// Only the timer/interrupt side is modelled (no waveform outputs).
#pragma once
#include "bus.h"
#include "intc.h"

class Mtu2 : public MmioDevice {
public:
    Mtu2(Intc& intc, double pclkHz) : intc(intc), pclk(pclkHz) {}
    const char* name() const override { return "MTU2"; }
    uint32_t read(uint32_t addr, int size) override;
    void write(uint32_t addr, int size, uint32_t val) override;
    void tick(uint64_t cpuCycles) override;
    double cpuHz = 200e6;
    bool log = false;
private:
    struct Ch {
        uint8_t tcr = 0, tmdr = 0, tiorh = 0, tiorl = 0, tier = 0, tsr = 0xC0, tier2 = 0, tsr2 = 0xC0, tbtm = 0;
        uint16_t tcnt = 0, tgr[6] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
        double frac = 0;
    } ch[5];
    // channel 5: three independent 16-bit counters U, V, W (registers at 0x080..0x0B6)
    struct Ch5 { uint16_t tcnt = 0, tgr = 0xFFFF; uint8_t tcr = 0, tior = 0; double frac = 0; } c5[3];   // 0=U 1=V 2=W
    uint8_t tsr5 = 0, tier5 = 0, tstr5 = 0, tcntcmpclr = 0;
    int vecCh5[3] = { 196, 197, 198 };     // TGIU_5, TGIV_5, TGIW_5 (SH7216 numbering; verified against firmware table at runtime)
    uint8_t tstr = 0, tsyr = 0, toer = 0xC0, tocr1 = 0, tocr2 = 0, tgcr = 0x80, titcr = 0, tbter = 0, tder = 1, twcr = 0, trwer = 1, ticcr = 0;
    uint16_t tcdr = 0xFFFF, tddr = 0xFFFF, tcbr = 0xFFFF, tadcr = 0, tadcora = 0xFFFF, tadcorb = 0xFFFF, tadcobra = 0xFFFF, tadcobrb = 0xFFFF;
    Intc& intc;
    double pclk;
    int prescale(int c) const;
    void countStep(int c);
    void updateIrq(int c);
    static int vecBase(int c) { static const int v[5] = { 156, 164, 172, 180, 188 }; return v[c]; }   // TGIA_n (SH7216 table)
    static int vecOvf(int c)  { static const int v[5] = { 160, 168, 176, 184, 192 }; return v[c]; }   // TCIV_n
};
