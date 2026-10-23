// SH-2A (SH2A-FPU) CPU core — interpreter.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "bus.h"

struct Sh2aBank { uint32_t R[15]; uint32_t GBR, MACH, MACL, PR; uint32_t IVN; bool valid = false; };

class Sh2a {
public:
    explicit Sh2a(Bus& bus) : bus(bus) { reset(); }
    void reset();
    void step();                       // execute one instruction (plus its delay slot if any)
    void run(uint64_t maxInsns);

    // --- architectural state ---
    uint32_t R[16]{};
    uint32_t PC = 0, PR = 0, SR = 0, GBR = 0, VBR = 0, TBR = 0, MACH = 0, MACL = 0;
    uint32_t FPSCR = 0x00040001, FPUL = 0;
    uint32_t FR[16]{};                 // FRn as raw bits; DRn = (FR[2n] << 32) | FR[2n+1]
    Sh2aBank bank[16];                 // register banks 0..14 (IBNR.BN)
    uint64_t cycles = 0, insns = 0;
    bool sleeping = false;

    // SR bits
    enum { SR_T = 1, SR_S = 2, SR_I_SHIFT = 4, SR_I_MASK = 0xF0, SR_Q = 0x100, SR_M = 0x200, SR_CS = 0x2000, SR_BO = 0x4000 };
    int  imask() const { return (SR >> 4) & 0xF; }
    bool T() const { return SR & 1; }
    void setT(bool t) { SR = (SR & ~1u) | (t ? 1 : 0); }

    // --- interrupt interface (filled by the INTC model) ---
    // returns true and sets level/vector if an interrupt with level > SR.I is pending
    std::function<bool(int imask, int& level, int& vector)> pendingInterrupt;
    std::function<void(int vector)> interruptAccepted;   // INTC clears/acknowledges edge sources
    uint32_t ibnr = 0;                  // INTC register bank control (mirrored here): BE[15:14], BOVE[13], BN[3:0]
    uint32_t ibcr = 0;                  // INTC bank control: bit i = use bank for priority level i
    bool checkInterrupts();

    // exceptions
    enum Vec { VEC_POWER_RESET = 0, VEC_MANUAL_RESET = 2, VEC_ILLEGAL = 4, VEC_SLOT_ILLEGAL = 6,
               VEC_CPU_ADDR_ERR = 9, VEC_DMA_ADDR_ERR = 10, VEC_NMI = 11, VEC_UBC = 12, VEC_FPU = 13, VEC_BANK_OVER = 15, VEC_BANK_UNDER = 16,
               VEC_DIV_ZERO = 17, VEC_DIV_OVF = 18 };
    void raiseException(int vec, uint32_t savedPc);     // generic: push SR, PC; PC = *(VBR+vec*4)
    [[noreturn]] void fatal(const char* fmt, ...);

    // debug
    bool trace = false;
    bool debugExc = false;
    int traceOnIrq = 0;        // if >0: trace this many instructions after every interrupt/exception
    uint32_t tracePc = 0; int tracePcLen = 0;   // trace tracePcLen instructions whenever PC reaches tracePc
    int traceCountdown = 0;
    // ring buffer of recently executed (pc, opcode) for post-mortem dumps
    static constexpr int kRing = 4096;
    uint32_t ringPc[kRing]{}; uint16_t ringOp[kRing]{}; int ringPos = 0; bool ringEnable = false;
    void dumpRing(int n) const;
    struct Hook { uint32_t pc; int reg; uint32_t val; uint32_t pr = 0; };   // pr != 0: only when PR matches
    std::vector<Hook> hooks;
    struct Probe { uint32_t pc; int memReg = -1; uint32_t memLen = 0; };   // memReg >= 0: also dump memLen bytes at R[memReg]
    std::vector<Probe> probes;      // PCs at which to dump registers (non-stopping), limited by probeLeft
    int probeLeft = 60;
    uint64_t probeAfter = 0;        // probes only fire once this many instructions have executed (--probe-after)
    // idle-loop fast forward: the RTOS idle task spins at idlePc ("mov.w @r14,r2; add #1,r2; bra; mov.w r2,@r14",
    // 4 instructions per iteration incrementing a 16-bit idle counter). When PC sits there with no interrupt
    // pending we execute idleIters iterations at once (--idle-pc, 0 disables).
    uint32_t idlePc = 0; int idleIters = 200; uint64_t idleSkipped = 0; bool idleHit = false;   // idleHit: main loop re-ticks the board after a skip
    bool irqCheck = true;           // set by the INTC when a line changes and by SR writes; cleared when nothing is pending above the mask
    // direct code window: instructions fetched from [fetchBase, fetchBase+fetchSize) read fetchPtr without the bus
    uint8_t* fetchPtr = nullptr; uint32_t fetchBase = 0, fetchSize = 0;
    // data accesses inside the same window (SDRAM: plain memory, no side effects) also bypass the bus unless a --watch is armed
    inline uint8_t* ramFast(uint32_t a) const { uint32_t p = a < 0x40000000u ? (a & 0x1FFFFFFFu) : a; uint32_t o = p - fetchBase; return (o < fetchSize && !bus.watchHi) ? fetchPtr + o : nullptr; }
    void dumpMem(uint32_t addr, uint32_t len);
    void dumpRegs() const;
    FILE* traceOut = stdout;
    std::function<void(Sh2a&)> onTrap;      // optional hook for TRAPA
    std::string disasm(uint32_t pc, uint16_t op, uint16_t op2) const;
    uint64_t pcHits = 0;

    // memory helpers (big endian); address errors raise exceptions
    inline uint8_t  rd8(uint32_t a)  { if (uint8_t* q = ramFast(a)) return q[0]; return bus.read8(a); }
    inline uint16_t rd16(uint32_t a) { if (a & 1) addrError(a); if (uint8_t* q = ramFast(a)) return (uint16_t)((q[0] << 8) | q[1]); return bus.read16(a); }
    inline uint32_t rd32(uint32_t a) { if (a & 3) addrError(a); if (uint8_t* q = ramFast(a)) return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]; return bus.read32(a); }
    inline void wr8(uint32_t a, uint8_t v)   { if (uint8_t* q = ramFast(a)) { q[0] = v; return; } bus.write8(a, v); }
    inline void wr16(uint32_t a, uint16_t v) { if (a & 1) addrError(a); if (uint8_t* q = ramFast(a)) { q[0] = (uint8_t)(v >> 8); q[1] = (uint8_t)v; return; } bus.write16(a, v); }
    inline void wr32(uint32_t a, uint32_t v) { if (a & 3) addrError(a); if (uint8_t* q = ramFast(a)) { q[0] = (uint8_t)(v >> 24); q[1] = (uint8_t)(v >> 16); q[2] = (uint8_t)(v >> 8); q[3] = (uint8_t)v; return; } bus.write32(a, v); }

    Bus& bus;
private:
    struct AddrErr { uint32_t addr; };
    void addrError(uint32_t a);
    void exec(uint16_t op, uint32_t pc, bool inSlot);
    void execDelaySlot(uint32_t target);
    void doBranch(uint32_t target) { PC = target; }
    uint16_t fetch(uint32_t a);
    // helpers
    void divStep(int n, int m);
    void macl(int n, int m);
    void macw(int n, int m);
    // fpu
    bool fpPR() const { return FPSCR & (1u << 19); }
    bool fpSZ() const { return FPSCR & (1u << 20); }
    double getDR(int n) const;
    void setDR(int n, double v);
    float getFR(int n) const;
    void setFR(int n, float v);
    void execFpu(uint16_t op, uint32_t pc);
    void exec32(uint16_t op, uint16_t op2, uint32_t pc);
    uint32_t curPc = 0;
    bool inSlotNow = false;
};
