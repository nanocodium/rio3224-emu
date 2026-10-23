#include "intc.h"

#include "intc_vectors.h"

Intc::Intc(Sh2a& c) : cpu(c) {
    cpu.pendingInterrupt = [this](int imask, int& level, int& vec) { return pick(imask, level, vec); };
    cpu.interruptAccepted = [this](int vec) {
        if (vec >= 64 && vec <= 71) {                       // IRQ: edge-detected ones are cleared on acceptance
            int i = vec - 64;
            int sense = (icr1 >> (i * 2)) & 3;             // 00 low level, 01 falling, 10 rising, 11 both
            if (sense != 0) { irqrr &= ~(1 << i); pending[vec] = false; dirty = true; cpu.irqCheck = true; }
        }
    };
}

int Intc::priorityOf(int vec) const {
    if (vec == 11) return 16;
    if (vec == 12 || vec == 14) return 15;
    for (auto& e : kVecIpr) if (vec == e.vec) return (ipr[e.ipr] >> e.shift) & 0xF;
    return 0;
}

void Intc::recompute() {
    bestLevel = -1; bestVec = -1;
    for (int v = 11; v < 512; v++) {
        if (!pending[v]) continue;
        int p = priorityOf(v);
        if (p > bestLevel) { bestLevel = p; bestVec = v; }
    }
    dirty = false;
}

bool Intc::pick(int imask, int& level, int& vec) {
    if (dirty) recompute();
    if (bestVec < 0 || bestLevel <= imask || bestLevel == 0) return false;
    level = bestLevel > 15 ? 15 : bestLevel; vec = bestVec;
    return true;
}

void Intc::setLine(int vector, bool active) {
    if (vector < 0 || vector >= 512) return;
    if (pending[vector] == active) return;
    pending[vector] = active; dirty = true; cpu.irqCheck = true;
    if (vector >= 64 && vector <= 71) { if (active) irqrr |= 1 << (vector - 64); else irqrr &= ~(1 << (vector - 64)); }
}

void Intc::updateCpu() { cpu.ibnr = ibnr; cpu.ibcr = ibcr; }

uint32_t Intc::read(uint32_t a, int size) {
    uint32_t v = 0;
    switch (a) {
    case 0xFFFE0800: v = icr0; break;
    case 0xFFFE0802: v = icr1; break;
    case 0xFFFE0804: v = icr2; break;
    case 0xFFFE0806: v = irqrr; break;
    case 0xFFFE0808: v = pinter; break;
    case 0xFFFE080A: v = pirr; break;
    case 0xFFFE080C: v = ibcr; break;
    case 0xFFFE080E: v = cpu.ibnr; break;
    case 0xFFFE0818: v = ipr[1]; break;
    case 0xFFFE081A: v = ipr[2]; break;
    case 0xFFFE0820: v = ipr[5]; break;
    default:
        if (a >= 0xFFFE0C00 && a <= 0xFFFE0C16) v = ipr[6 + (a - 0xFFFE0C00) / 2];
        else fprintf(stderr, "[INTC] read of unknown reg %08X\n", a);
    }
    if (size == 4) v = (v << 16) | read(a + 2, 2);
    if (logAccess) fprintf(stderr, "[INTC] rd %08X -> %0*X\n", a, size * 2, v);
    return v;
}

void Intc::write(uint32_t a, int size, uint32_t val) {
    dirty = true; cpu.irqCheck = true;
    if (size == 4) { write(a, 2, val >> 16); write(a + 2, 2, val & 0xFFFF); return; }
    if (size == 1) { uint16_t cur = read(a & ~1u, 2); if (a & 1) val = (cur & 0xFF00) | (val & 0xFF); else val = (cur & 0xFF) | (val << 8); a &= ~1u; }
    if (logAccess) fprintf(stderr, "[INTC] wr %08X <- %04X\n", a, val);
    switch (a) {
    case 0xFFFE0800: icr0 = val & 0xFF00; return;
    case 0xFFFE0802: icr1 = val; return;
    case 0xFFFE0804: icr2 = val; return;
    case 0xFFFE0806: irqrr &= val; for (int i = 0; i < 8; i++) if (!(irqrr & (1 << i)) && (((icr1 >> (i * 2)) & 3) != 0)) pending[64 + i] = false; dirty = true; return;   // write 0 to clear
    case 0xFFFE0808: pinter = val; return;
    case 0xFFFE080A: return;
    case 0xFFFE080C: ibcr = val & 0xFFFE; updateCpu(); return;
    case 0xFFFE080E: ibnr = (val & 0xE000) | (cpu.ibnr & 0xF); cpu.ibnr = ibnr; return;
    case 0xFFFE0818: ipr[1] = val; dirty = true; return;
    case 0xFFFE081A: ipr[2] = val; dirty = true; return;
    case 0xFFFE0820: ipr[5] = val; dirty = true; return;
    default:
        if (a >= 0xFFFE0C00 && a <= 0xFFFE0C16) { ipr[6 + (a - 0xFFFE0C00) / 2] = val; dirty = true; return; }
        fprintf(stderr, "[INTC] write to unknown reg %08X = %X\n", a, val);
    }
}
