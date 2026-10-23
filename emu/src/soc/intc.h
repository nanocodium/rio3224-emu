// SH7203 interrupt controller (INTC): IPR priority registers, IRQ/PINT inputs, on-chip module sources.
#pragma once
#include "bus.h"
#include "sh2a/cpu.h"
#include <vector>
#include <cstdio>

class Intc : public MmioDevice {
public:
    explicit Intc(Sh2a& cpu);
    const char* name() const override { return "INTC"; }
    uint32_t read(uint32_t addr, int size) override;
    void write(uint32_t addr, int size, uint32_t val) override;

    // on-chip module / external interrupt lines (level sensitive; vector numbers per SH7203 table 6.4)
    void setLine(int vector, bool active);
    void pulse(int vector) { setLine(vector, true); }           // for edge-ish sources that are cleared by reading status
    bool lineActive(int vector) const { return vector >= 0 && vector < 512 && pending[vector]; }

    // registers (16-bit)
    uint16_t icr0 = 0, icr1 = 0, icr2 = 0, irqrr = 0, pinter = 0, pirr = 0, ibcr = 0, ibnr = 0;
    uint16_t ipr[18]{};             // IPR01..IPR17 indexed by number (0,3,4 unused)
    bool pending[512]{};
    int bestLevel = -1, bestVec = -1; bool dirty = true;
    void recompute();
    bool logAccess = false;

    int priorityOf(int vector) const;   // 0..16
    Sh2a& cpu;
    void updateCpu();
private:
    bool pick(int imask, int& level, int& vec);
};
