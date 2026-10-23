#include "scif.h"
#include <cstdio>

// SCFSR bits: ER(7) TEND(6) TDFE(5) BRK(4) FER(3) PER(2) RDF(1) DR(0)
// SCSCR bits: TIE(7) RIE(6) TE(5) RE(4) REIE(3) CKE1,0

void Scif::rxByte(uint8_t b) {
    std::lock_guard<std::mutex> g(mu);
    rxPending.push_back(b);
    if (logAccess) fprintf(stderr, "[%s] rxByte %02X (pending %zu, fifo %zu, baud %u)\n", nm, b, rxPending.size(), rxFifo.size(), baud);
}

void Scif::tick(uint64_t cpuCycles) {
    // move pending bytes into the FIFO at roughly the configured baud rate (or quickly if unknown)
    cycAcc += cpuCycles;
    uint64_t perByte = baud ? (uint64_t)(200e6 * 10.0 / baud) : 20000;
    if (cycAcc < perByte) return;
    std::lock_guard<std::mutex> g(mu);
    while (cycAcc >= perByte && !rxPending.empty() && rxFifo.size() < 16) {
        cycAcc -= perByte;
        rxFifo.push_back(rxPending.front()); rxPending.pop_front();
    }
    if (cycAcc >= perByte) cycAcc = 0;
    if (!rxFifo.empty() && (scscr & 0x10)) {
        int trig = 1;
        switch ((scfcr >> 6) & 3) { case 0: trig = 1; break; case 1: trig = 4; break; case 2: trig = 8; break; case 3: trig = 14; break; }
        if ((int)rxFifo.size() >= trig) scfsr |= 0x02;      // RDF
        else scfsr |= 0x01;                                 // DR (data ready but below trigger) — set after timeout in HW
    }
    updateIrq();
}

void Scif::updateIrq() {
    bool txi = (scscr & 0x80) && (scfsr & 0x20);
    bool rxi = (scscr & 0x40) && (scfsr & 0x03);
    intc.setLine(vecBase() + 3, txi);   // vectors per channel: BRI, ERI, RXI, TXI (FIFO-type SCIF ordering)
    intc.setLine(vecBase() + 2, rxi);
}

uint32_t Scif::read(uint32_t a, int size) {
    uint32_t v = 0;
    switch (a & 0xFF) {
    case 0x00: v = scsmr; break;
    case 0x04: v = scbrr; break;
    case 0x08: v = scscr; break;
    case 0x0C: v = 0; break;                                   // SCFTDR write-only
    case 0x10: v = scfsr | 0x60; break;                        // TEND, TDFE always set (we transmit instantly)
    case 0x14: {                                               // SCFRDR
        std::lock_guard<std::mutex> g(mu);
        if (!rxFifo.empty()) { v = rxFifo.front(); rxFifo.pop_front(); }
        if (rxFifo.empty()) scfsr &= ~0x03;
        break;
    }
    case 0x18: v = scfcr; break;
    case 0x1C: { std::lock_guard<std::mutex> g(mu); v = (uint32_t)rxFifo.size() & 0x1F; break; }   // SCFDR: R count low, T count high (0)
    case 0x20: v = scsptr; break;
    case 0x24: v = sclsr; break;
    case 0x28: v = scemr; break;
    default: fprintf(stderr, "[%s] read unknown %08X\n", nm, a);
    }
    if (logAccess) fprintf(stderr, "[%s] rd %08X -> %X\n", nm, a, v);
    return v;
}

void Scif::write(uint32_t a, int size, uint32_t val) {
    if (logAccess) fprintf(stderr, "[%s] wr %08X <- %X\n", nm, a, val);
    switch (a & 0xFF) {
    case 0x00: scsmr = val; break;
    case 0x04: scbrr = val; {
        // baud = Pφ / (64 * 2^(2n-1) * (N+1)) with n = CKS; Pφ = 33.33MHz
        int cks = scsmr & 3; double div = 64.0 * (cks == 0 ? 0.5 : (1 << (2 * cks - 1)));
        baud = (uint32_t)(16000000.0 / (div * (scbrr + 1)));   // Pφ = 16 MHz
        break;
    }
    case 0x08: scscr = val; break;
    case 0x0C: if (onTx) onTx(val & 0xFF); break;              // SCFTDR
    case 0x10: {                                               // SCFSR: flags cleared by writing 0 (after reading 1)
        uint16_t clr = ~val & 0x00FF;
        scfsr &= ~(clr & 0xDC);                                // ER, BRK, FER, PER, TEND? (TEND/TDFE are forced on read anyway)
        if (clr & 0x02) { std::lock_guard<std::mutex> g(mu); if (rxFifo.empty()) scfsr &= ~0x02; }
        if (clr & 0x01) { std::lock_guard<std::mutex> g(mu); if (rxFifo.empty()) scfsr &= ~0x01; }
        break;
    }
    case 0x18:
        scfcr = val;
        if (val & 0x02) { std::lock_guard<std::mutex> g(mu); rxFifo.clear(); scfsr &= ~0x03; }   // RFRST
        break;
    case 0x20: scsptr = val; break;
    case 0x24: sclsr = val & 0x01; break;
    case 0x28: scemr = val; break;
    default: fprintf(stderr, "[%s] write unknown %08X = %X\n", nm, a, val);
    }
    updateIrq();
}
