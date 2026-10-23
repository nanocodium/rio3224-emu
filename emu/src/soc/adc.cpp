#include "adc.h"

void Adc::start(int un) {
    Unit& x = u[un];
    int mode = (x.adcsr >> 3) & 7, ch = x.adcsr & 7;
    // single mode: convert the selected channel; multi/scan: channels 0..ch
    int lo = (mode == 0) ? (ch & 3) : 0, hi = ch & 3;
    for (int c = lo; c <= hi; c++) x.addr[c] = (uint16_t)((input[un * 4 + c] & 0x3FF) << 6);
    x.adcsr |= 0x8000;                                   // ADF
    if (mode != 4) x.adcsr &= ~0x2000;                   // ADST clears except in scan mode
    intc.setLine(92 + un * 4, (x.adcsr & 0xC000) == 0xC000);   // ADI0 = 92, ADI1 = 96
}

uint32_t Adc::read(uint32_t a, int size) {
    uint32_t o = a & 0xFF, v = 0;
    if (o < 0x08) v = u[0].addr[(o >> 1) & 3];
    else if (o >= 0x10 && o < 0x18) v = u[1].addr[(o >> 1) & 3];
    else if (o == 0x20) v = u[0].adcsr;
    else if (o == 0x22) v = u[1].adcsr;
    else fprintf(stderr, "[ADC] read unknown %08X\n", a);
    if (size == 1) v = (a & 1) ? (v & 0xFF) : (v >> 8);
    if (size == 4) v = (v << 16) | read(a + 2, 2);
    if (log) fprintf(stderr, "[ADC] rd %08X -> %04X\n", a, v);
    return v;
}

void Adc::write(uint32_t a, int size, uint32_t val) {
    if (size == 4) { write(a, 2, val >> 16); write(a + 2, 2, val & 0xFFFF); return; }
    uint32_t o = a & 0xFF;
    if (log) fprintf(stderr, "[ADC] wr %08X <- %04X\n", a, val);
    if (o == 0x20 || o == 0x22) {
        int un = (o == 0x20) ? 0 : 1;
        Unit& x = u[un];
        uint16_t old = x.adcsr;
        // ADF is cleared only by writing 0 after reading 1
        uint16_t adf = (val & 0x8000) ? (old & 0x8000) : 0;
        x.adcsr = (val & 0x7FFF) | adf;
        if ((val & 0x2000) && !(old & 0x2000)) start(un);
        intc.setLine(92 + un * 4, (x.adcsr & 0xC000) == 0xC000);
    } else fprintf(stderr, "[ADC] write unknown %08X = %X\n", a, val);
}
