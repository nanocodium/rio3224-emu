#include "cmt.h"
#include <cstdio>

static const int kDiv[4] = { 8, 32, 128, 512 };

void Cmt::updateIrq(int i) {
    bool irq = (ch[i].csr & 0x80) && (ch[i].csr & 0x40);
    intc.setLine(i == 0 ? 140 : 144, irq);
}

void Cmt::tick(uint64_t cpuCycles) {
    for (int i = 0; i < 2; i++) {
        if (!(cmstr & (1 << i))) continue;
        double pclkCycles = (double)cpuCycles * (pclk / cpuHz);
        ch[i].frac += pclkCycles / kDiv[ch[i].csr & 3];
        if (ch[i].frac < 1.0) continue;
        uint32_t steps = (uint32_t)ch[i].frac;
        ch[i].frac -= steps;
        while (steps--) {
            if (ch[i].cnt == ch[i].cor) { ch[i].cnt = 0; ch[i].csr |= 0x80; updateIrq(i); }
            else ch[i].cnt++;
        }
    }
}

uint32_t Cmt::read(uint32_t a, int size) {
    uint32_t v = 0;
    switch (a & 0xFFFF) {
    case 0xC000: v = cmstr; break;
    case 0xC002: v = ch[0].csr; break;
    case 0xC004: v = ch[0].cnt; break;
    case 0xC006: v = ch[0].cor; break;
    case 0xC008: v = ch[1].csr; break;
    case 0xC00A: v = ch[1].cnt; break;
    case 0xC00C: v = ch[1].cor; break;
    default: fprintf(stderr, "[CMT] read unknown %08X\n", a);
    }
    if (size == 4) v = (v << 16) | read(a + 2, 2);
    if (size == 1) v = (a & 1) ? (v & 0xFF) : (v >> 8);
    return v;
}

void Cmt::write(uint32_t a, int size, uint32_t val) {
    if (size == 4) { write(a, 2, val >> 16); write(a + 2, 2, val & 0xFFFF); return; }
    switch (a & 0xFFFF) {
    case 0xC000: cmstr = val & 3; return;
    case 0xC002: case 0xC008: {
        int i = (a & 0xFFFF) == 0xC002 ? 0 : 1;
        // CMF (bit7) can only be cleared by writing 0 after reading 1
        uint16_t newCsr = (val & 0x7F) | ((val & 0x80) ? (ch[i].csr & 0x80) : 0);
        ch[i].csr = newCsr; updateIrq(i); return;
    }
    case 0xC004: ch[0].cnt = val; return;
    case 0xC006: ch[0].cor = val; return;
    case 0xC00A: ch[1].cnt = val; return;
    case 0xC00C: ch[1].cor = val; return;
    default: fprintf(stderr, "[CMT] write unknown %08X = %X\n", a, val);
    }
}
