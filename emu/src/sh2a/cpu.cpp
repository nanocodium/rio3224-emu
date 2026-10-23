// SH-2A interpreter. Big-endian. Delay slots are executed recursively right after the branch.
#include "cpu.h"
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstdlib>

static inline int32_t  s8(uint32_t v)  { return (int8_t)v; }
static inline int32_t  s16(uint32_t v) { return (int16_t)v; }
static inline uint32_t f2u(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float    u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

void Sh2a::reset() {
    for (auto& r : R) r = 0;
    SR = 0xF0; PC = 0; PR = 0; GBR = 0; VBR = 0; TBR = 0; MACH = MACL = 0;
    FPSCR = 0x00040001; FPUL = 0;
    for (auto& f : FR) f = 0;
    for (auto& b : bank) b.valid = false;
    ibnr = 0; cycles = 0; insns = 0; sleeping = false;
}

void Sh2a::fatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\n*** CPU FATAL at PC=%08X: ", curPc);
    vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n  R0-7  %08X %08X %08X %08X %08X %08X %08X %08X\n  R8-15 %08X %08X %08X %08X %08X %08X %08X %08X\n  PR=%08X SR=%08X GBR=%08X VBR=%08X insns=%llu\n",
            R[0],R[1],R[2],R[3],R[4],R[5],R[6],R[7],R[8],R[9],R[10],R[11],R[12],R[13],R[14],R[15],PR,SR,GBR,VBR,(unsigned long long)insns);
    exit(2);
}

uint16_t Sh2a::fetch(uint32_t a) {
    if (a & 1) throw AddrErr{a};                                  // odd PC: CPU address error exception
    uint32_t p = a & 0x1FFFFFFF;
    if (fetchPtr && p - fetchBase < fetchSize) { const uint8_t* q = fetchPtr + (p - fetchBase); return (uint16_t)((q[0] << 8) | q[1]); }
    return bus.read16(a);
}

void Sh2a::addrError(uint32_t a) {
    throw AddrErr{a};
}

// Exception entry (SH-2A): SR then PC are pushed to the stack, PC <- (VBR + vec*4)
void Sh2a::raiseException(int vec, uint32_t savedPc) {
    R[15] -= 4; bus.write32(R[15], SR);
    R[15] -= 4; bus.write32(R[15], savedPc);
    PC = bus.read32(VBR + vec * 4);
    if (debugExc || PC < 0x1000) fprintf(stderr, "[cpu] exception vec %d at PC=%08X -> handler %08X (VBR=%08X, SP=%08X, insn %llu)\n", vec, savedPc, PC, VBR, R[15], (unsigned long long)insns);
    if (traceOnIrq) traceCountdown = traceOnIrq;
    cycles += 6;
}

// Interrupt acceptance including SH-2A register-bank save
bool Sh2a::checkInterrupts() {
    if (!irqCheck || !pendingInterrupt) return false;
    int level = 0, vec = 0;
    if (!pendingInterrupt(imask(), level, vec)) { irqCheck = false; return false; }
    sleeping = false;
    // IBNR.BE[15:14]: 00 banks not used, 01 banks used for all interrupts except NMI/user break,
    // 11 use controlled per priority level by IBCR (bit E<level>), 10 reserved
    int be = (ibnr >> 14) & 3;
    int bn = ibnr & 0xF;
    bool useBank = false;
    if (vec != VEC_NMI && vec != VEC_UBC) {
        if (be == 1) useBank = true;
        else if (be == 3) useBank = (ibcr >> level) & 1;
    }
    uint32_t savedPc = PC;
    bool overflowToStack = false;
    if (useBank) {
        if (bn >= 15) {
            if (ibnr & (1u << 13)) {                     // BOVE: bank overflow exception instead of accepting
                raiseException(VEC_BANK_OVER, PC);
                return true;
            }
            overflowToStack = true;
        } else {
            Sh2aBank& b = bank[bn];
            for (int i = 0; i < 15; i++) b.R[i] = R[i];
            b.GBR = GBR; b.MACH = MACH; b.MACL = MACL; b.PR = PR; b.IVN = vec * 4; b.valid = true;
            ibnr = (ibnr & ~0xFu) | (bn + 1);
            SR &= ~(uint32_t)SR_BO;
        }
    }
    R[15] -= 4; bus.write32(R[15], SR);
    R[15] -= 4; bus.write32(R[15], savedPc);
    if (overflowToStack) {
        // §6.8.3: all banks used -> banked registers go to the stack after SR/PC, order MACL, MACH, GBR, PR, R14..R0
        R[15] -= 4; bus.write32(R[15], MACL);
        R[15] -= 4; bus.write32(R[15], MACH);
        R[15] -= 4; bus.write32(R[15], GBR);
        R[15] -= 4; bus.write32(R[15], PR);
        for (int i = 14; i >= 0; i--) { R[15] -= 4; bus.write32(R[15], R[i]); }
        SR |= SR_BO;                                     // BO=1 tells RESBANK that registers are on the stack
    }
    SR = (SR & ~(uint32_t)SR_I_MASK) | ((uint32_t)(level & 0xF) << 4);
    PC = bus.read32(VBR + vec * 4);
    if (debugExc || PC < 0x1000) fprintf(stderr, "[cpu] interrupt vec %d level %d at PC=%08X (SR.I was %d) -> handler %08X (bank=%d, insn %llu)\n", vec, level, savedPc, (bus.read32(R[15] + 4) >> 4) & 0xF, PC, useBank, (unsigned long long)insns);
    if (traceOnIrq) traceCountdown = traceOnIrq;
    cycles += 8;
    if (interruptAccepted) interruptAccepted(vec);
    return true;
}

void Sh2a::dumpRing(int n) const {
    if (n > kRing) n = kRing;
    fprintf(stderr, "---- last %d instructions ----\n", n);
    for (int i = n; i > 0; i--) {
        int k = (ringPos - i) & (kRing - 1);
        uint16_t op = ringOp[k];
        bool is32 = ((op & 0xF000) == 0x3000 && ((op & 0xF) == 1 || (op & 0xF) == 9)) || ((op & 0xF00E) == 0x0000 && (op & 0xF0FF) != 0);
        uint16_t op2 = is32 ? const_cast<Bus&>(bus).read16(ringPc[k] + 2) : 0;
        fprintf(stderr, "%08X  %04X  %s\n", ringPc[k], op, disasm(ringPc[k], op, op2).c_str());
    }
}

void Sh2a::dumpMem(uint32_t addr, uint32_t len) {
    for (uint32_t o = 0; o < len; o += 16) {
        fprintf(stderr, "  %08X:", addr + o);
        for (uint32_t k = 0; k < 16 && o + k < len; k++) fprintf(stderr, "%s%02X", (k % 4) ? "" : " ", bus.read8(addr + o + k));
        fprintf(stderr, "\n");
    }
}

void Sh2a::dumpRegs() const {
    fprintf(stderr, "PC=%08X PR=%08X SR=%08X GBR=%08X VBR=%08X TBR=%08X\n  R0-7  %08X %08X %08X %08X %08X %08X %08X %08X\n  R8-15 %08X %08X %08X %08X %08X %08X %08X %08X\n",
            PC, PR, SR, GBR, VBR, TBR, R[0],R[1],R[2],R[3],R[4],R[5],R[6],R[7],R[8],R[9],R[10],R[11],R[12],R[13],R[14],R[15]);
    fprintf(stderr, "  stack:");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", const_cast<Bus&>(bus).read32(R[15] + i * 4));
    fprintf(stderr, "\n");
}

void Sh2a::run(uint64_t maxInsns) {
    uint64_t end = insns + maxInsns;
    while (insns < end) step();
}

void Sh2a::step() {
    if (irqCheck && pendingInterrupt) checkInterrupts();   // the INTC/SR-write flag keeps the (out-of-line) check off the hot path
    if (sleeping) { cycles += 4; insns++; return; }
    if (idlePc && (PC & 0x1FFFFFFF) == idlePc && fetch(PC) == 0x62E1) {   // idle task spin loop (any cache mirror): run idleIters iterations at once
        uint32_t a = R[14];
        uint16_t v = bus.read16(a);
        bus.write16(a, (uint16_t)(v + idleIters));
        insns += 4ull * idleIters; cycles += 4ull * idleIters; idleSkipped += idleIters; idleHit = true;
        return;
    }
    curPc = PC;
    if (tracePcLen && PC == tracePc) traceCountdown = tracePcLen;
    if (!hooks.empty()) for (auto& h : hooks) if (h.pc == PC && (h.pr == 0 || h.pr == PR)) { R[h.reg] = h.val; if (debugExc) fprintf(stderr, "[cpu] hook at %08X: r%d = %08X\n", PC, h.reg, h.val); }
    if (!probes.empty() && probeLeft > 0 && insns >= probeAfter) for (auto& p : probes) if (p.pc == PC) { probeLeft--; fprintf(stderr, "[probe] PC=%08X insn %llu\n", PC, (unsigned long long)insns); dumpRegs(); if (p.memReg >= 0) dumpMem(R[p.memReg], p.memLen); }
    try {
        uint16_t op = fetch(PC);
        if (ringEnable) { ringPc[ringPos] = PC; ringOp[ringPos] = op; ringPos = (ringPos + 1) & (kRing - 1); }
        if (trace || traceCountdown > 0) {
            if (traceCountdown > 0) traceCountdown--;
            bool is32 = ((op & 0xF000) == 0x3000 && ((op & 0xF) == 1 || (op & 0xF) == 9)) || ((op & 0xF00E) == 0x0000 && (op & 0xF0FF) != 0);
            uint16_t op2 = is32 ? bus.read16(PC + 2) : 0;
            fprintf(traceOut, "%08X  %04X  %s\n", PC, op, disasm(PC, op, op2).c_str());
        }
        PC += 2;
        exec(op, curPc, false);
        insns++; cycles++;
    } catch (AddrErr& e) {
        // CPU address error exception (vector 9); saved PC is the faulting instruction
        static int n = 0;
        if (++n < 20) fprintf(stderr, "[cpu] address error at PC=%08X accessing %08X\n", curPc, e.addr);
        PC = curPc;
        raiseException(VEC_CPU_ADDR_ERR, curPc);
        insns++;
    }
}

// execute the delay slot instruction at PC, then jump
void Sh2a::execDelaySlot(uint32_t target) {
    uint32_t slotPc = PC;
    uint16_t op = fetch(slotPc);
    if (trace || traceCountdown > 0) fprintf(traceOut, "%08X  %04X    %s   [slot]\n", slotPc, op, disasm(slotPc, op, 0).c_str());
    PC = slotPc + 2;
    exec(op, slotPc, true);
    PC = target;
    cycles++;
}

static inline uint32_t pcDisp8l(uint32_t pc, uint16_t op) { return ((pc + 4) & ~3u) + (op & 0xFF) * 4; }

void Sh2a::exec(uint16_t op, uint32_t pc, bool inSlot) {
    const int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF;
    switch (op >> 12) {
    case 0x0:
        switch (op & 0xF) {
        case 0x0: case 0x1: {                         // MOVI20 / MOVI20S (32-bit)
            uint16_t op2 = fetch(PC); PC += 2;
            int32_t imm = ((op >> 4) & 0xF) << 16 | op2;
            if (imm & 0x80000) imm -= 0x100000;
            if (op & 1) imm <<= 8;
            R[n] = imm;
            return;
        }
        case 0x2:
            switch (m) {
            case 0: R[n] = SR; return;                // STC SR,Rn
            case 1: R[n] = GBR; return;
            case 2: R[n] = VBR; return;
            }
            break;
        case 0x3:
            switch (m) {
            case 0: if (inSlot) break; { uint32_t t = pc + 4 + R[n]; PR = pc + 4; execDelaySlot(t); return; }   // BSRF
            case 2: if (inSlot) break; { uint32_t t = pc + 4 + R[n]; execDelaySlot(t); return; }                 // BRAF
            case 8: return;                                                                                       // PREF
            }
            break;
        case 0x4: wr8(R[0] + R[n], R[m]); return;     // MOV.B Rm,@(R0,Rn)
        case 0x5: wr16(R[0] + R[n], R[m]); return;
        case 0x6: wr32(R[0] + R[n], R[m]); return;
        case 0x7: MACL = R[n] * R[m]; cycles += 2; return;   // MUL.L
        case 0x8:
            switch (m) {
            case 0: setT(false); return;              // CLRT
            case 1: setT(true); return;               // SETT
            case 2: MACH = MACL = 0; return;          // CLRMAC
            case 4: SR &= ~(uint32_t)SR_S; return;    // CLRS
            case 5: SR |= SR_S; return;               // SETS
            case 6: setT(!T()); return;               // NOTT
            }
            break;
        case 0x9:
            switch (m) {
            case 0: return;                           // NOP
            case 1: SR &= ~(uint32_t)(SR_M | SR_Q); setT(false); return;   // DIV0U
            case 2: R[n] = T(); return;               // MOVT
            case 3: R[n] = !T(); return;              // MOVRT
            }
            break;
        case 0xA:
            switch (m) {
            case 0: R[n] = MACH; return;              // STS MACH
            case 1: R[n] = MACL; return;
            case 2: R[n] = PR; return;
            case 4: R[n] = TBR; return;               // STC TBR,Rn
            case 5: R[n] = FPUL; return;
            case 6: R[n] = FPSCR & 0x003FFFFF; return;
            }
            break;
        case 0xB:
            switch (m) {
            case 0: if (inSlot) break; execDelaySlot(PR); return;                 // RTS
            case 1: sleeping = true; return;                                         // SLEEP
            case 2: {                                                                 // RTE
                if (inSlot) break;
                uint32_t newPc = rd32(R[15]); R[15] += 4;
                uint32_t newSr = rd32(R[15]); R[15] += 4;
                execDelaySlot(newPc);
                SR = newSr & 0x000063F3; irqCheck = true;
                cycles += 4;
                return;
            }
            case 5: {                                                                 // RESBANK
                int bn = ibnr & 0xF;
                if (!(SR & SR_BO) && bn > 0 && bank[bn - 1].valid) {
                    Sh2aBank& b = bank[bn - 1];
                    for (int i = 0; i < 15; i++) R[i] = b.R[i];
                    GBR = b.GBR; MACH = b.MACH; MACL = b.MACL; PR = b.PR; b.valid = false;
                    ibnr = (ibnr & ~0xFu) | (bn - 1);
                } else if (SR & SR_BO) {
                    // registers were saved to the stack (bank overflow): restore R0..R14, PR, GBR, MACH, MACL
                    for (int i = 0; i <= 14; i++) { R[i] = rd32(R[15]); R[15] += 4; }
                    PR = rd32(R[15]); R[15] += 4;
                    GBR = rd32(R[15]); R[15] += 4;
                    MACH = rd32(R[15]); R[15] += 4;
                    MACL = rd32(R[15]); R[15] += 4;
                } else {
                    raiseException(VEC_BANK_UNDER, pc);    // RESBANK with no saved bank
                }
                cycles += 8;
                return;
            }
            case 6: if (inSlot) break; PC = PR; cycles += 2; return;                 // RTS/N
            case 7: if (inSlot) break; R[0] = R[n]; PC = PR; cycles += 2; return;    // RTV/N Rm
            }
            break;
        case 0xC: R[n] = (int32_t)s8(rd8(R[0] + R[m])); return;    // MOV.B @(R0,Rm),Rn
        case 0xD: R[n] = (int32_t)s16(rd16(R[0] + R[m])); return;
        case 0xE: R[n] = rd32(R[0] + R[m]); return;
        case 0xF: macl(n, m); return;                              // MAC.L @Rm+,@Rn+
        }
        break;

    case 0x1: wr32(R[n] + (op & 0xF) * 4, R[m]); return;           // MOV.L Rm,@(disp,Rn)

    case 0x2:
        switch (op & 0xF) {
        case 0x0: wr8(R[n], R[m]); return;
        case 0x1: wr16(R[n], R[m]); return;
        case 0x2: wr32(R[n], R[m]); return;
        case 0x4: R[n] -= 1; wr8(R[n], R[m]); return;
        case 0x5: R[n] -= 2; wr16(R[n], R[m]); return;
        case 0x6: R[n] -= 4; wr32(R[n], R[m]); return;
        case 0x7: {                                                // DIV0S
            bool q = R[n] >> 31, mm = R[m] >> 31;
            SR = (SR & ~(uint32_t)(SR_Q | SR_M)) | (q ? SR_Q : 0) | (mm ? SR_M : 0);
            setT(q != mm); return;
        }
        case 0x8: setT((R[n] & R[m]) == 0); return;               // TST
        case 0x9: R[n] &= R[m]; return;
        case 0xA: R[n] ^= R[m]; return;
        case 0xB: R[n] |= R[m]; return;
        case 0xC: {                                                // CMP/STR
            uint32_t t = R[n] ^ R[m];
            setT(!(t & 0xFF000000) || !(t & 0x00FF0000) || !(t & 0x0000FF00) || !(t & 0xFF)); return;
        }
        case 0xD: R[n] = (R[m] << 16) | (R[n] >> 16); return;      // XTRCT
        case 0xE: MACL = (uint32_t)(R[n] & 0xFFFF) * (uint32_t)(R[m] & 0xFFFF); return;   // MULU.W
        case 0xF: MACL = (uint32_t)((int32_t)s16(R[n]) * (int32_t)s16(R[m])); return;     // MULS.W
        }
        break;

    case 0x3:
        switch (op & 0xF) {
        case 0x0: setT(R[n] == R[m]); return;                      // CMP/EQ
        case 0x1: { uint16_t op2 = fetch(PC); PC += 2; exec32(op, op2, pc); return; }   // 32-bit MOV forms
        case 0x2: setT(R[n] >= R[m]); return;                      // CMP/HS
        case 0x3: setT((int32_t)R[n] >= (int32_t)R[m]); return;    // CMP/GE
        case 0x4: divStep(n, m); return;                           // DIV1
        case 0x5: { uint64_t p = (uint64_t)R[n] * R[m]; MACH = p >> 32; MACL = (uint32_t)p; cycles += 2; return; }  // DMULU.L
        case 0x6: setT(R[n] > R[m]); return;                       // CMP/HI
        case 0x7: setT((int32_t)R[n] > (int32_t)R[m]); return;     // CMP/GT
        case 0x8: R[n] -= R[m]; return;
        case 0x9: { uint16_t op2 = fetch(PC); PC += 2; exec32(op, op2, pc); return; }   // bit ops @(disp12,Rn)
        case 0xA: { uint32_t t0 = R[n] - R[m]; uint32_t t1 = t0 - T(); setT((R[n] < t0) || (t0 < t1)); R[n] = t1; return; }  // SUBC
        case 0xB: { int32_t d = (int32_t)R[n] - (int32_t)R[m]; bool ov = ((R[n] ^ R[m]) & (R[n] ^ (uint32_t)d)) >> 31; R[n] = d; setT(ov); return; }  // SUBV
        case 0xC: R[n] += R[m]; return;
        case 0xD: { int64_t p = (int64_t)(int32_t)R[n] * (int32_t)R[m]; MACH = (uint64_t)p >> 32; MACL = (uint32_t)p; cycles += 2; return; }  // DMULS.L
        case 0xE: { uint32_t t0 = R[n] + R[m]; uint32_t t1 = t0 + T(); setT((t0 < R[n]) || (t1 < t0)); R[n] = t1; return; }  // ADDC
        case 0xF: { int32_t d = (int32_t)R[n] + (int32_t)R[m]; bool ov = (~(R[n] ^ R[m]) & (R[n] ^ (uint32_t)d)) >> 31; R[n] = d; setT(ov); return; }  // ADDV
        }
        break;

    case 0x4:
        switch (op & 0xFF) {
        case 0x00: setT(R[n] >> 31); R[n] <<= 1; return;           // SHLL
        case 0x10: R[n] -= 1; setT(R[n] == 0); return;             // DT
        case 0x20: setT(R[n] >> 31); R[n] <<= 1; return;           // SHAL
        case 0x01: setT(R[n] & 1); R[n] >>= 1; return;             // SHLR
        case 0x11: setT((int32_t)R[n] >= 0); return;               // CMP/PZ
        case 0x21: setT(R[n] & 1); R[n] = (uint32_t)((int32_t)R[n] >> 1); return;   // SHAR
        case 0x02: R[n] -= 4; wr32(R[n], MACH); return;            // STS.L MACH,@-Rn
        case 0x12: R[n] -= 4; wr32(R[n], MACL); return;
        case 0x22: R[n] -= 4; wr32(R[n], PR); return;
        case 0x52: R[n] -= 4; wr32(R[n], FPUL); return;
        case 0x62: R[n] -= 4; wr32(R[n], FPSCR & 0x003FFFFF); return;
        case 0x03: R[n] -= 4; wr32(R[n], SR); return;              // STC.L SR,@-Rn
        case 0x13: R[n] -= 4; wr32(R[n], GBR); return;
        case 0x23: R[n] -= 4; wr32(R[n], VBR); return;
        case 0x04: setT(R[n] >> 31); R[n] = (R[n] << 1) | (R[n] >> 31); return;              // ROTL
        case 0x24: { bool t = R[n] >> 31; R[n] = (R[n] << 1) | T(); setT(t); return; }      // ROTCL
        case 0x05: setT(R[n] & 1); R[n] = (R[n] >> 1) | (R[n] << 31); return;                // ROTR
        case 0x15: setT((int32_t)R[n] > 0); return;                // CMP/PL
        case 0x25: { bool t = R[n] & 1; R[n] = (R[n] >> 1) | ((uint32_t)T() << 31); setT(t); return; }  // ROTCR
        case 0x06: MACH = rd32(R[n]); R[n] += 4; return;           // LDS.L @Rm+,MACH
        case 0x16: MACL = rd32(R[n]); R[n] += 4; return;
        case 0x26: PR = rd32(R[n]); R[n] += 4; return;
        case 0x56: FPUL = rd32(R[n]); R[n] += 4; return;
        case 0x66: FPSCR = rd32(R[n]) & 0x003FFFFF; R[n] += 4; return;
        case 0x07: SR = rd32(R[n]) & 0x000063F3; R[n] += 4; cycles += 3; irqCheck = true; return;   // LDC.L @Rm+,SR
        case 0x17: GBR = rd32(R[n]); R[n] += 4; return;
        case 0x27: VBR = rd32(R[n]); R[n] += 4; return;
        case 0x08: R[n] <<= 2; return;
        case 0x18: R[n] <<= 8; return;
        case 0x28: R[n] <<= 16; return;
        case 0x09: R[n] >>= 2; return;
        case 0x19: R[n] >>= 8; return;
        case 0x29: R[n] >>= 16; return;
        case 0x0A: MACH = R[n]; return;                            // LDS Rm,MACH
        case 0x1A: MACL = R[n]; return;
        case 0x2A: PR = R[n]; return;
        case 0x4A: TBR = R[n]; return;                             // LDC Rm,TBR
        case 0x5A: FPUL = R[n]; return;
        case 0x6A: FPSCR = R[n] & 0x003FFFFF; return;
        case 0x0B: if (inSlot) break; PR = pc + 4; execDelaySlot(R[n]); return;    // JSR @Rm
        case 0x2B: if (inSlot) break; execDelaySlot(R[n]); return;                 // JMP @Rm
        case 0x4B: if (inSlot) break; PR = pc + 2; PC = R[n]; cycles += 2; return;  // JSR/N @Rm
        case 0x1B: { uint8_t v = rd8(R[n]); setT(v == 0); wr8(R[n], v | 0x80); cycles += 3; return; }   // TAS.B
        case 0x0E: SR = R[n] & 0x000063F3; cycles += 2; irqCheck = true; return;    // LDC Rm,SR
        case 0x1E: GBR = R[n]; return;
        case 0x2E: VBR = R[n]; return;
        // ---- SH-2A additions ----
        case 0x80: R[n] = (uint32_t)((int32_t)R[0] * (int32_t)R[n]); cycles += 2; return;   // MULR R0,Rn
        case 0x84:                                                                              // DIVU R0,Rn
            if (R[0] == 0) { raiseException(VEC_DIV_ZERO, pc + 2); return; }
            R[n] = R[n] / R[0]; cycles += 20; return;
        case 0x94: {                                                                            // DIVS R0,Rn
            int32_t a = R[n], b = R[0];
            if (b == 0) { raiseException(VEC_DIV_ZERO, pc + 2); return; }
            if (a == INT32_MIN && b == -1) { raiseException(VEC_DIV_OVF, pc + 2); return; }
            R[n] = (uint32_t)(a / b); cycles += 20; return;
        }
        case 0x81: if (R[n] > 0xFF) { R[n] = 0xFF; SR |= SR_CS; } return;                    // CLIPU.B
        case 0x85: if (R[n] > 0xFFFF) { R[n] = 0xFFFF; SR |= SR_CS; } return;                // CLIPU.W
        case 0x91: { int32_t v = R[n]; if (v > 127) { R[n] = 127; SR |= SR_CS; } else if (v < -128) { R[n] = (uint32_t)-128; SR |= SR_CS; } return; }
        case 0x95: { int32_t v = R[n]; if (v > 32767) { R[n] = 32767; SR |= SR_CS; } else if (v < -32768) { R[n] = (uint32_t)-32768; SR |= SR_CS; } return; }
        case 0x8B: wr8(R[n], R[0]); R[n] += 1; return;             // MOV.B R0,@Rn+
        case 0x9B: wr16(R[n], R[0]); R[n] += 2; return;            // MOV.W R0,@Rn+
        case 0xAB: wr32(R[n], R[0]); R[n] += 4; return;            // MOV.L R0,@Rn+
        case 0xCB: R[n] -= 1; R[0] = (int32_t)s8(rd8(R[n])); return;     // MOV.B @-Rm,R0
        case 0xDB: R[n] -= 2; R[0] = (int32_t)s16(rd16(R[n])); return;   // MOV.W @-Rm,R0
        case 0xEB: R[n] -= 4; R[0] = rd32(R[n]); return;                  // MOV.L @-Rm,R0
        // bank entry addressing (SH-2A software manual): Rm[10:7] = bank, Rm[6:2] = register:
        //   0..14 R0..R14, 15 MACH, 16 IVN (vector table offset), 17 PR, 18 GBR, 19 MACL
        case 0xE1: {                                                       // STBANK R0,@Rn
            uint32_t a = R[n]; int b = (a >> 7) & 0xF; int idx = (a >> 2) & 0x1F;
            if (b < 15) { Sh2aBank& bk = bank[b]; if (idx < 15) bk.R[idx] = R[0]; else if (idx == 15) bk.MACH = R[0]; else if (idx == 16) bk.IVN = R[0]; else if (idx == 17) bk.PR = R[0]; else if (idx == 18) bk.GBR = R[0]; else if (idx == 19) bk.MACL = R[0]; }
            cycles += 6; return;
        }
        case 0xE5: {                                                       // LDBANK @Rm,R0
            uint32_t a = R[n]; int b = (a >> 7) & 0xF; int idx = (a >> 2) & 0x1F;
            uint32_t v = 0;
            if (b < 15) { Sh2aBank& bk = bank[b]; if (idx < 15) v = bk.R[idx]; else if (idx == 15) v = bk.MACH; else if (idx == 16) v = bk.IVN; else if (idx == 17) v = bk.PR; else if (idx == 18) v = bk.GBR; else if (idx == 19) v = bk.MACL; }
            R[0] = v; cycles += 5; return;
        }
        case 0xF0:                                                         // MOVMU.L Rm,@-R15 : push PR, R14 .. Rm   (Rm=R15 means PR only)
            R[15] -= 4; wr32(R[15], PR);
            for (int i = 14; i >= n; i--) { R[15] -= 4; wr32(R[15], R[i]); }
            cycles += 15 - n + 1; return;
        case 0xF1:                                                         // MOVML.L Rm,@-R15 : push Rm .. R0   (Rm=R15 is read as PR)
            for (int i = n; i >= 0; i--) { R[15] -= 4; wr32(R[15], i == 15 ? PR : R[i]); }
            cycles += n + 1; return;
        case 0xF4:                                                         // MOVMU.L @R15+,Rn : pop Rn .. R14, PR
            for (int i = n; i <= 14; i++) { R[i] = rd32(R[15]); R[15] += 4; }
            PR = rd32(R[15]); R[15] += 4;
            cycles += 15 - n + 1; return;
        case 0xF5:                                                         // MOVML.L @R15+,Rn : pop R0 .. Rn   (Rn=R15 is PR)
            for (int i = 0; i <= n; i++) { uint32_t v = rd32(R[15]); R[15] += 4; if (i == 15) PR = v; else R[i] = v; }
            cycles += n + 1; return;
        }
        switch (op & 0xF) {
        case 0xC: {                                                // SHAD Rm,Rn
            int32_t s = (int32_t)R[m];
            if (s >= 0) R[n] <<= (s & 31);
            else if ((s & 31) == 0) R[n] = (int32_t)R[n] < 0 ? 0xFFFFFFFF : 0;
            else R[n] = (uint32_t)((int32_t)R[n] >> ((-s) & 31));
            return;
        }
        case 0xD: {                                                // SHLD Rm,Rn
            int32_t s = (int32_t)R[m];
            if (s >= 0) R[n] <<= (s & 31);
            else if ((s & 31) == 0) R[n] = 0;
            else R[n] >>= ((-s) & 31);
            return;
        }
        case 0xF: macw(n, m); return;                              // MAC.W @Rm+,@Rn+
        }
        break;

    case 0x5: R[n] = rd32(R[m] + (op & 0xF) * 4); return;          // MOV.L @(disp,Rm),Rn

    case 0x6:
        switch (op & 0xF) {
        case 0x0: R[n] = (int32_t)s8(rd8(R[m])); return;
        case 0x1: R[n] = (int32_t)s16(rd16(R[m])); return;
        case 0x2: R[n] = rd32(R[m]); return;
        case 0x3: R[n] = R[m]; return;
        case 0x4: { uint32_t v = (int32_t)s8(rd8(R[m])); if (n != m) R[m] += 1; R[n] = v; return; }
        case 0x5: { uint32_t v = (int32_t)s16(rd16(R[m])); if (n != m) R[m] += 2; R[n] = v; return; }
        case 0x6: { uint32_t v = rd32(R[m]); if (n != m) R[m] += 4; R[n] = v; return; }
        case 0x7: R[n] = ~R[m]; return;
        case 0x8: R[n] = (R[m] & 0xFFFF0000) | ((R[m] & 0xFF) << 8) | ((R[m] >> 8) & 0xFF); return;   // SWAP.B
        case 0x9: R[n] = (R[m] << 16) | (R[m] >> 16); return;      // SWAP.W
        case 0xA: { uint32_t t = 0 - R[m]; R[n] = t - T(); setT((0 < t) || (t < R[n])); return; }   // NEGC
        case 0xB: R[n] = 0 - R[m]; return;
        case 0xC: R[n] = R[m] & 0xFF; return;
        case 0xD: R[n] = R[m] & 0xFFFF; return;
        case 0xE: R[n] = (int32_t)s8(R[m]); return;
        case 0xF: R[n] = (int32_t)s16(R[m]); return;
        }
        break;

    case 0x7: R[n] += (int32_t)s8(op); return;                     // ADD #imm,Rn

    case 0x8:
        switch ((op >> 8) & 0xF) {
        case 0x0: wr8(R[m] + (op & 0xF), R[0]); return;            // MOV.B R0,@(disp,Rn)
        case 0x1: wr16(R[m] + (op & 0xF) * 2, R[0]); return;
        case 0x4: R[0] = (int32_t)s8(rd8(R[m] + (op & 0xF))); return;
        case 0x5: R[0] = (int32_t)s16(rd16(R[m] + (op & 0xF) * 2)); return;
        case 0x8: setT((int32_t)R[0] == (int32_t)s8(op)); return;  // CMP/EQ #imm,R0
        case 0x9: if (inSlot) break; if (T()) { PC = pc + 4 + (int32_t)s8(op) * 2; cycles++; } return;      // BT
        case 0xB: if (inSlot) break; if (!T()) { PC = pc + 4 + (int32_t)s8(op) * 2; cycles++; } return;     // BF
        case 0xD: if (inSlot) break; if (T()) execDelaySlot(pc + 4 + (int32_t)s8(op) * 2); return;         // BT/S
        case 0xF: if (inSlot) break; if (!T()) execDelaySlot(pc + 4 + (int32_t)s8(op) * 2); return;        // BF/S
        case 0x3: {                                                 // JSR/N @@(disp8,TBR)
            if (inSlot) break;
            uint32_t t = rd32(TBR + (op & 0xFF) * 4);
            PR = pc + 2; PC = t; cycles += 4; return;
        }
        case 0x6: {                                                 // BCLR/BSET #imm3,Rn
            int bit = op & 7;
            if (op & 8) R[m] |= 1u << bit; else R[m] &= ~(1u << bit);
            return;
        }
        case 0x7: {                                                 // BST/BLD #imm3,Rn
            int bit = op & 7;
            if (op & 8) setT((R[m] >> bit) & 1);
            else R[m] = (R[m] & ~(1u << bit)) | ((uint32_t)T() << bit);
            return;
        }
        }
        break;

    case 0x9: R[n] = (int32_t)s16(rd16(pc + 4 + (op & 0xFF) * 2)); return;   // MOV.W @(disp,PC),Rn
    case 0xA: { if (inSlot) break; int32_t d = op & 0xFFF; if (d & 0x800) d -= 0x1000; execDelaySlot(pc + 4 + d * 2); return; }   // BRA
    case 0xB: { if (inSlot) break; int32_t d = op & 0xFFF; if (d & 0x800) d -= 0x1000; PR = pc + 4; execDelaySlot(pc + 4 + d * 2); return; }   // BSR

    case 0xC:
        switch ((op >> 8) & 0xF) {
        case 0x0: wr8(GBR + (op & 0xFF), R[0]); return;
        case 0x1: wr16(GBR + (op & 0xFF) * 2, R[0]); return;
        case 0x2: wr32(GBR + (op & 0xFF) * 4, R[0]); return;
        case 0x3: {                                                 // TRAPA #imm
            if (inSlot) break;
            if (onTrap) onTrap(*this);
            raiseException(op & 0xFF, pc + 2);
            cycles += 2;
            return;
        }
        case 0x4: R[0] = (int32_t)s8(rd8(GBR + (op & 0xFF))); return;
        case 0x5: R[0] = (int32_t)s16(rd16(GBR + (op & 0xFF) * 2)); return;
        case 0x6: R[0] = rd32(GBR + (op & 0xFF) * 4); return;
        case 0x7: R[0] = pcDisp8l(pc, op); return;                 // MOVA
        case 0x8: setT((R[0] & (op & 0xFF)) == 0); return;         // TST #imm,R0
        case 0x9: R[0] &= (op & 0xFF); return;
        case 0xA: R[0] ^= (op & 0xFF); return;
        case 0xB: R[0] |= (op & 0xFF); return;
        case 0xC: setT((rd8(GBR + R[0]) & (op & 0xFF)) == 0); return;                   // TST.B
        case 0xD: { uint32_t a = GBR + R[0]; wr8(a, rd8(a) & (op & 0xFF)); return; }
        case 0xE: { uint32_t a = GBR + R[0]; wr8(a, rd8(a) ^ (op & 0xFF)); return; }
        case 0xF: { uint32_t a = GBR + R[0]; wr8(a, rd8(a) | (op & 0xFF)); return; }
        }
        break;

    case 0xD: R[n] = rd32(pcDisp8l(pc, op)); return;               // MOV.L @(disp,PC),Rn
    case 0xE: R[n] = (int32_t)s8(op); return;                      // MOV #imm,Rn
    case 0xF: execFpu(op, pc); return;
    }
    // illegal instruction
    static int illegalCount = 0;
    if (++illegalCount < 20) fprintf(stderr, "[cpu] illegal instruction %04X at %08X%s\n", op, pc, inSlot ? " (slot)" : "");
    PC = pc;   // exception saved PC = this instruction
    raiseException(inSlot ? VEC_SLOT_ILLEGAL : VEC_ILLEGAL, pc);
}

// 32-bit instruction formats: 0011nnnnmmmm0001 + op2 (MOV disp12), 0011nnnn0iii1001 + op2 (bit ops)
void Sh2a::exec32(uint16_t op, uint16_t op2, uint32_t pc) {
    const int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF;
    uint32_t disp = op2 & 0xFFF;
    if ((op & 0xF) == 1) {
        switch (op2 >> 12) {
        case 0x0: wr8(R[n] + disp, R[m]); return;                 // MOV.B Rm,@(disp12,Rn)
        case 0x1: wr16(R[n] + disp * 2, R[m]); return;
        case 0x2: wr32(R[n] + disp * 4, R[m]); return;
        case 0x3:                                                  // FMOV.S FRm,@(disp12,Rn) / FMOV.D DRm,@(disp12,Rn)
            if (fpSZ()) { uint32_t a = R[n] + disp * 8; wr32(a, FR[m & 0xE]); wr32(a + 4, FR[(m & 0xE) + 1]); }
            else wr32(R[n] + disp * 4, FR[m]);
            return;
        case 0x4: R[n] = (int32_t)s8(rd8(R[m] + disp)); return;    // MOV.B @(disp12,Rm),Rn
        case 0x5: R[n] = (int32_t)s16(rd16(R[m] + disp * 2)); return;
        case 0x6: R[n] = rd32(R[m] + disp * 4); return;
        case 0x7:                                                  // FMOV.S @(disp12,Rm),FRn / FMOV.D
            if (fpSZ()) { uint32_t a = R[m] + disp * 8; FR[n & 0xE] = rd32(a); FR[(n & 0xE) + 1] = rd32(a + 4); }
            else FR[n] = rd32(R[m] + disp * 4);
            return;
        case 0x8: R[n] = rd8(R[m] + disp); return;                 // MOVU.B
        case 0x9: R[n] = rd16(R[m] + disp * 2); return;            // MOVU.W
        }
    } else {                                                       // bit operations on a memory byte
        int bit = m & 7;
        uint32_t a = R[n] + disp;
        uint8_t v = rd8(a);
        bool b = (v >> bit) & 1;
        switch (op2 >> 12) {
        case 0x0: wr8(a, v & ~(1 << bit)); cycles += 2; return;   // BCLR.B
        case 0x1: wr8(a, v | (1 << bit)); cycles += 2; return;    // BSET.B
        case 0x2: wr8(a, (v & ~(1 << bit)) | (T() << bit)); cycles += 2; return;   // BST.B
        case 0x3: setT(b); cycles += 2; return;                    // BLD.B
        case 0x4: setT(b && T()); cycles += 2; return;             // BAND.B
        case 0x5: setT(b || T()); cycles += 2; return;             // BOR.B
        case 0x6: setT(b != T()); cycles += 2; return;             // BXOR.B
        case 0xB: setT(!b); cycles += 2; return;                   // BLDNOT.B
        case 0xC: setT(!b && T()); cycles += 2; return;            // BANDNOT.B
        case 0xD: setT(!b || T()); cycles += 2; return;            // BORNOT.B
        }
    }
    fprintf(stderr, "[cpu] illegal 32-bit instruction %04X %04X at %08X\n", op, op2, pc);
    PC = pc;
    raiseException(VEC_ILLEGAL, pc);
}

void Sh2a::divStep(int n, int m) {
    // DIV1 Rm,Rn — algorithm from the SH programming manual
    uint32_t tmp0, tmp2;
    uint8_t old_q, tmp1;
    old_q = (SR & SR_Q) ? 1 : 0;
    uint8_t Q = (R[n] >> 31) & 1;
    uint8_t M = (SR & SR_M) ? 1 : 0;
    R[n] = (R[n] << 1) | T();
    if (old_q == 0) {
        if (M == 0) {
            tmp0 = R[n]; R[n] -= R[m]; tmp1 = (R[n] > tmp0);
            Q = (Q == 0) ? tmp1 : (tmp1 == 0);
        } else {
            tmp0 = R[n]; R[n] += R[m]; tmp1 = (R[n] < tmp0);
            Q = (Q == 0) ? (tmp1 == 0) : tmp1;
        }
    } else {
        if (M == 0) {
            tmp0 = R[n]; R[n] += R[m]; tmp1 = (R[n] < tmp0);
            Q = (Q == 0) ? tmp1 : (tmp1 == 0);
        } else {
            tmp0 = R[n]; R[n] -= R[m]; tmp1 = (R[n] > tmp0);
            Q = (Q == 0) ? (tmp1 == 0) : tmp1;
        }
    }
    (void)tmp2;
    SR = (SR & ~(uint32_t)SR_Q) | (Q ? SR_Q : 0);
    setT(Q == M);
}

void Sh2a::macl(int n, int m) {
    int64_t a = (int32_t)rd32(R[n]); R[n] += 4;
    int64_t b = (int32_t)rd32(R[m]); R[m] += 4;
    int64_t mac = ((int64_t)MACH << 32) | MACL;
    int64_t prod = a * b;
    if (SR & SR_S) {
        int64_t sum = mac + prod;
        const int64_t mx = 0x00007FFFFFFFFFFFLL, mn = -0x0000800000000000LL;
        if (sum > mx) sum = mx; else if (sum < mn) sum = mn;
        mac = sum;
    } else mac += prod;
    MACH = (uint64_t)mac >> 32; MACL = (uint32_t)mac;
    cycles += 2;
}

void Sh2a::macw(int n, int m) {
    int32_t a = s16(rd16(R[n])); R[n] += 2;
    int32_t b = s16(rd16(R[m])); R[m] += 2;
    int64_t prod = (int64_t)a * b;
    if (SR & SR_S) {
        int64_t sum = (int64_t)(int32_t)MACL + prod;
        if (sum > 0x7FFFFFFFLL) { MACL = 0x7FFFFFFF; MACH |= 1; }
        else if (sum < -0x80000000LL) { MACL = 0x80000000; MACH |= 1; }
        else MACL = (uint32_t)sum;
    } else {
        int64_t mac = ((int64_t)MACH << 32) | MACL;
        mac += prod;
        MACH = (uint64_t)mac >> 32; MACL = (uint32_t)mac;
    }
    cycles += 2;
}

// ---------------- FPU ----------------
double Sh2a::getDR(int n) const { uint64_t u = ((uint64_t)FR[n & 0xE] << 32) | FR[(n & 0xE) + 1]; double d; memcpy(&d, &u, 8); return d; }
void Sh2a::setDR(int n, double v) { uint64_t u; memcpy(&u, &v, 8); FR[n & 0xE] = u >> 32; FR[(n & 0xE) + 1] = (uint32_t)u; }
float Sh2a::getFR(int n) const { return u2f(FR[n]); }
void Sh2a::setFR(int n, float v) { FR[n] = f2u(v); }

void Sh2a::execFpu(uint16_t op, uint32_t pc) {
    const int n = (op >> 8) & 0xF, m = (op >> 4) & 0xF;
    const bool pr = fpPR(), sz = fpSZ();
    switch (op & 0xF) {
    case 0x0: if (pr) setDR(n, getDR(n) + getDR(m)); else setFR(n, getFR(n) + getFR(m)); return;   // FADD
    case 0x1: if (pr) setDR(n, getDR(n) - getDR(m)); else setFR(n, getFR(n) - getFR(m)); return;   // FSUB
    case 0x2: if (pr) setDR(n, getDR(n) * getDR(m)); else setFR(n, getFR(n) * getFR(m)); return;   // FMUL
    case 0x3: if (pr) setDR(n, getDR(n) / getDR(m)); else setFR(n, getFR(n) / getFR(m)); cycles += pr ? 22 : 10; return;   // FDIV
    case 0x4: setT(pr ? getDR(n) == getDR(m) : getFR(n) == getFR(m)); return;                      // FCMP/EQ
    case 0x5: setT(pr ? getDR(n) > getDR(m) : getFR(n) > getFR(m)); return;                        // FCMP/GT
    case 0x6:                                                                                      // FMOV.S @(R0,Rm),FRn
        if (sz) { uint32_t a = R[0] + R[m]; FR[n & 0xE] = rd32(a); FR[(n & 0xE) + 1] = rd32(a + 4); } else FR[n] = rd32(R[0] + R[m]); return;
    case 0x7:                                                                                      // FMOV.S FRm,@(R0,Rn)
        if (sz) { uint32_t a = R[0] + R[n]; wr32(a, FR[m & 0xE]); wr32(a + 4, FR[(m & 0xE) + 1]); } else wr32(R[0] + R[n], FR[m]); return;
    case 0x8:                                                                                      // FMOV.S @Rm,FRn
        if (sz) { uint32_t a = R[m]; FR[n & 0xE] = rd32(a); FR[(n & 0xE) + 1] = rd32(a + 4); } else FR[n] = rd32(R[m]); return;
    case 0x9:                                                                                      // FMOV.S @Rm+,FRn
        if (sz) { uint32_t a = R[m]; FR[n & 0xE] = rd32(a); FR[(n & 0xE) + 1] = rd32(a + 4); R[m] += 8; } else { FR[n] = rd32(R[m]); R[m] += 4; } return;
    case 0xA:                                                                                      // FMOV.S FRm,@Rn
        if (sz) { uint32_t a = R[n]; wr32(a, FR[m & 0xE]); wr32(a + 4, FR[(m & 0xE) + 1]); } else wr32(R[n], FR[m]); return;
    case 0xB:                                                                                      // FMOV.S FRm,@-Rn
        if (sz) { R[n] -= 8; uint32_t a = R[n]; wr32(a, FR[m & 0xE]); wr32(a + 4, FR[(m & 0xE) + 1]); } else { R[n] -= 4; wr32(R[n], FR[m]); } return;
    case 0xC:                                                                                      // FMOV FRm,FRn
        if (sz) { FR[n & 0xE] = FR[m & 0xE]; FR[(n & 0xE) + 1] = FR[(m & 0xE) + 1]; } else FR[n] = FR[m]; return;
    case 0xE: setFR(n, getFR(0) * getFR(m) + getFR(n)); return;                                    // FMAC FR0,FRm,FRn
    case 0xD:
        switch (m) {
        case 0x0: FR[n] = FPUL; return;                                                            // FSTS FPUL,FRn
        case 0x1: FPUL = FR[n]; return;                                                            // FLDS FRm,FPUL
        case 0x2: if (pr) setDR(n, (double)(int32_t)FPUL); else setFR(n, (float)(int32_t)FPUL); return;   // FLOAT FPUL,FRn
        case 0x3: {                                                                                // FTRC FRm,FPUL
            double v = pr ? getDR(n) : (double)getFR(n);
            int32_t r;
            if (std::isnan(v)) r = (int32_t)0x80000000;
            else if (v >= 2147483647.0) r = 0x7FFFFFFF;
            else if (v <= -2147483648.0) r = (int32_t)0x80000000;
            else r = (int32_t)v;
            FPUL = (uint32_t)r; return;
        }
        case 0x4: if (pr) FR[n & 0xE] ^= 0x80000000; else FR[n] ^= 0x80000000; return;             // FNEG
        case 0x5: if (pr) FR[n & 0xE] &= 0x7FFFFFFF; else FR[n] &= 0x7FFFFFFF; return;             // FABS
        case 0x6: if (pr) setDR(n, std::sqrt(getDR(n))); else setFR(n, std::sqrt(getFR(n))); cycles += 10; return;   // FSQRT
        case 0x8: FR[n] = 0; return;                                                               // FLDI0
        case 0x9: FR[n] = 0x3F800000; return;                                                      // FLDI1
        case 0xA: setDR(n, (double)u2f(FPUL)); return;                                             // FCNVSD FPUL,DRn
        case 0xB: FPUL = f2u((float)getDR(n)); return;                                             // FCNVDS DRm,FPUL
        case 0xF: if (op == 0xF3FD) { FPSCR ^= (1u << 20); return; } break;                        // FSCHG
        }
        break;
    }
    fprintf(stderr, "[cpu] illegal FPU instruction %04X at %08X\n", op, pc);
    PC = pc;
    raiseException(VEC_ILLEGAL, pc);
}
