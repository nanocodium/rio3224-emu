#include "mtu2.h"
#include <cstdio>

// TCR.TPSC (bits 2:0): 0 Pφ/1, 1 Pφ/4, 2 Pφ/16, 3 Pφ/64, others: external/other channel (treated as Pφ/256, /1024 where defined)
int Mtu2::prescale(int c) const {
    // SH7203 table 12.3: ch0: 100 TCLKA 101 TCLKB 110 TCLKC 111 TCLKD ; ch1: 100 TCLKA 101 TCLKB 110 Pφ/256 111 TCNT2 ovf
    //                    ch2: 100 TCLKA 101 TCLKB 110 TCLKC 111 Pφ/1024 ; ch3/4: 100 Pφ/256 101 Pφ/1024 110 TCLKA 111 TCLKB
    int t = ch[c].tcr & 7;
    if (t < 4) return 1 << (2 * t);
    switch (c) {
    case 1: return t == 6 ? 256 : 1024;
    case 2: return t == 7 ? 1024 : 1024;
    case 3: case 4: return t == 4 ? 256 : 1024;
    default: return 1024;          // external clock inputs: not modelled, run slowly
    }
}

void Mtu2::updateIrq(int c) {
    // TIER: TGIEA(0) TGIEB(1) TGIEC(2) TGIED(3) TCIEV(4) TCIEU(5) TTGE(7) ; TSR: TGFA..TGFD(0..3) TCFV(4) TCFU(5) TCFD(7)
    int base = vecBase(c);
    int ntgr = (c == 0 || c == 3 || c == 4) ? 4 : 2;
    for (int i = 0; i < ntgr; i++) intc.setLine(base + i, (ch[c].tier & ch[c].tsr & (1 << i)) != 0);
    intc.setLine(vecOvf(c), (ch[c].tier & ch[c].tsr & 0x10) != 0);
    if (c == 1 || c == 2) intc.setLine(vecOvf(c) + 1, (ch[c].tier & ch[c].tsr & 0x20) != 0);   // TCIU_n
    if (c == 0) { intc.setLine(161, (ch[0].tier2 & ch[0].tsr2 & 1) != 0); intc.setLine(162, (ch[0].tier2 & ch[0].tsr2 & 2) != 0); }
}

void Mtu2::countStep(int c) {
    Ch& h = ch[c];
    int ntgr = (c == 0 || c == 3 || c == 4) ? 4 : 2;
    // clear source: TCR.CCLR (bits 7:5): 0 none, 1 TGRA, 2 TGRB, 3 sync, 5 TGRC, 6 TGRD (ch0/3/4)
    int cclr = (h.tcr >> 5) & 7;
    bool cleared = false;
    for (int i = 0; i < ntgr; i++) {
        if (h.tcnt == h.tgr[i]) {
            h.tsr |= (1 << i);
            if ((cclr == 1 && i == 0) || (cclr == 2 && i == 1) || (cclr == 5 && i == 2) || (cclr == 6 && i == 3)) cleared = true;
        }
    }
    if (c == 0) { if (h.tcnt == h.tgr[4]) h.tsr2 |= 1; if (h.tcnt == h.tgr[5]) h.tsr2 |= 2; }
    if (cleared) h.tcnt = 0;
    else { h.tcnt++; if (h.tcnt == 0) h.tsr |= 0x10; }
}

void Mtu2::tick(uint64_t cpuCycles) {
    double pclkCycles = (double)cpuCycles * (pclk / cpuHz);
    // channel 5 U/V/W: TSTR_5 bits 2,1,0 = CSTU5, CSTV5, CSTW5 ; TCR TPSC 0..3 = Pphi/1,4,16,64
    for (int i = 0; i < 3; i++) {
        if (!(tstr5 & (1 << (2 - i)))) continue;
        Ch5& h = c5[i];
        static const int div[4] = { 1, 4, 16, 64 };
        h.frac += pclkCycles / div[h.tcr & 3];
        if (h.frac < 1.0) continue;
        uint32_t steps = (uint32_t)h.frac; h.frac -= steps;
        while (steps) {
            uint32_t dist = (h.tgr >= h.tcnt) ? (h.tgr - h.tcnt) : (0x10000 - h.tcnt + h.tgr);
            if (dist > 0 && steps < dist) { h.tcnt += steps; steps = 0; break; }
            if (dist > 0) { h.tcnt += dist; steps -= dist; }
            // compare match
            tsr5 |= (1 << (2 - i));
            if (tcntcmpclr & (1 << (2 - i))) h.tcnt = 0; else h.tcnt++;
            if (steps) steps--;
        }
        intc.setLine(vecCh5[i], (tier5 & tsr5 & (1 << (2 - i))) != 0);
    }
    for (int c = 0; c < 5; c++) {
        if (!(tstr & (c < 3 ? (1 << c) : (1 << (c + 3))))) continue;     // TSTR: CST0,1,2 bits 0-2, CST3 bit6, CST4 bit7
        Ch& h = ch[c];
        h.frac += pclkCycles / prescale(c);
        if (h.frac < 1.0) continue;
        uint32_t steps = (uint32_t)h.frac; h.frac -= steps;
        // fast path: jump ahead while far from any compare value
        while (steps) {
            uint32_t minDist = 0x10000 - h.tcnt;   // to overflow
            int ntgr = (c == 0 || c == 3 || c == 4) ? 4 : 2;
            for (int i = 0; i < ntgr; i++) { uint32_t d = (h.tgr[i] >= h.tcnt) ? (h.tgr[i] - h.tcnt) : (0x10000 - h.tcnt + h.tgr[i]); if (d < minDist) minDist = d; }
            if (c == 0) for (int i = 4; i < 6; i++) { uint32_t d = (h.tgr[i] >= h.tcnt) ? (h.tgr[i] - h.tcnt) : (0x10000 - h.tcnt + h.tgr[i]); if (d < minDist) minDist = d; }
            if (minDist > 1 && steps >= minDist) { h.tcnt += minDist - 1; steps -= minDist - 1; if (!steps) break; }
            else if (minDist > 1 && steps < minDist) { h.tcnt += steps; steps = 0; break; }
            countStep(c); steps--;
        }
        updateIrq(c);
    }
}

uint32_t Mtu2::read(uint32_t a, int size) {
    uint32_t v = 0;
    uint32_t o = a & 0x3FF;
    auto rd8 = [&](uint32_t off) -> uint32_t {
        switch (off) {
        case 0x000: return ch[2].tcr; case 0x001: return ch[2].tmdr; case 0x002: return ch[2].tiorh; case 0x004: return ch[2].tier; case 0x005: return ch[2].tsr;
        case 0x006: return ch[2].tcnt >> 8; case 0x007: return ch[2].tcnt & 0xFF; case 0x008: return ch[2].tgr[0] >> 8; case 0x009: return ch[2].tgr[0] & 0xFF; case 0x00A: return ch[2].tgr[1] >> 8; case 0x00B: return ch[2].tgr[1] & 0xFF;
        case 0x200: return ch[3].tcr; case 0x201: return ch[4].tcr; case 0x202: return ch[3].tmdr; case 0x203: return ch[4].tmdr;
        case 0x204: return ch[3].tiorh; case 0x205: return ch[3].tiorl; case 0x206: return ch[4].tiorh; case 0x207: return ch[4].tiorl;
        case 0x208: return ch[3].tier; case 0x209: return ch[4].tier; case 0x20A: return toer; case 0x20D: return tgcr; case 0x20E: return tocr1; case 0x20F: return tocr2;
        case 0x210: return ch[3].tcnt >> 8; case 0x211: return ch[3].tcnt & 0xFF; case 0x212: return ch[4].tcnt >> 8; case 0x213: return ch[4].tcnt & 0xFF;
        case 0x214: return tcdr >> 8; case 0x215: return tcdr & 0xFF; case 0x216: return tddr >> 8; case 0x217: return tddr & 0xFF;
        case 0x218: return ch[3].tgr[0] >> 8; case 0x219: return ch[3].tgr[0] & 0xFF; case 0x21A: return ch[3].tgr[1] >> 8; case 0x21B: return ch[3].tgr[1] & 0xFF;
        case 0x21C: return ch[4].tgr[0] >> 8; case 0x21D: return ch[4].tgr[0] & 0xFF; case 0x21E: return ch[4].tgr[1] >> 8; case 0x21F: return ch[4].tgr[1] & 0xFF;
        case 0x220: case 0x221: return 0; case 0x222: return tcbr >> 8; case 0x223: return tcbr & 0xFF;
        case 0x224: return ch[3].tgr[2] >> 8; case 0x225: return ch[3].tgr[2] & 0xFF; case 0x226: return ch[3].tgr[3] >> 8; case 0x227: return ch[3].tgr[3] & 0xFF;
        case 0x228: return ch[4].tgr[2] >> 8; case 0x229: return ch[4].tgr[2] & 0xFF; case 0x22A: return ch[4].tgr[3] >> 8; case 0x22B: return ch[4].tgr[3] & 0xFF;
        case 0x22C: return ch[3].tsr; case 0x22D: return ch[4].tsr; case 0x230: return titcr; case 0x231: return 0; case 0x232: return tbter; case 0x234: return tder; case 0x236: return 0;
        case 0x238: return ch[3].tbtm; case 0x239: return ch[4].tbtm; case 0x240: return tadcr >> 8; case 0x241: return tadcr & 0xFF;
        case 0x244: return tadcora >> 8; case 0x245: return tadcora & 0xFF; case 0x246: return tadcorb >> 8; case 0x247: return tadcorb & 0xFF;
        case 0x248: return tadcobra >> 8; case 0x249: return tadcobra & 0xFF; case 0x24A: return tadcobrb >> 8; case 0x24B: return tadcobrb & 0xFF;
        case 0x080: return c5[0].tcnt >> 8; case 0x081: return c5[0].tcnt & 0xFF; case 0x082: return c5[0].tgr >> 8; case 0x083: return c5[0].tgr & 0xFF; case 0x084: return c5[0].tcr; case 0x086: return c5[0].tior;
        case 0x090: return c5[1].tcnt >> 8; case 0x091: return c5[1].tcnt & 0xFF; case 0x092: return c5[1].tgr >> 8; case 0x093: return c5[1].tgr & 0xFF; case 0x094: return c5[1].tcr; case 0x096: return c5[1].tior;
        case 0x0A0: return c5[2].tcnt >> 8; case 0x0A1: return c5[2].tcnt & 0xFF; case 0x0A2: return c5[2].tgr >> 8; case 0x0A3: return c5[2].tgr & 0xFF; case 0x0A4: return c5[2].tcr; case 0x0A6: return c5[2].tior;
        case 0x0B0: return tsr5; case 0x0B2: return tier5; case 0x0B4: return tstr5; case 0x0B6: return tcntcmpclr;
        case 0x260: return twcr; case 0x280: return tstr; case 0x281: return tsyr; case 0x284: return trwer;
        case 0x300: return ch[0].tcr; case 0x301: return ch[0].tmdr; case 0x302: return ch[0].tiorh; case 0x303: return ch[0].tiorl; case 0x304: return ch[0].tier; case 0x305: return ch[0].tsr;
        case 0x306: return ch[0].tcnt >> 8; case 0x307: return ch[0].tcnt & 0xFF;
        case 0x308: return ch[0].tgr[0] >> 8; case 0x309: return ch[0].tgr[0] & 0xFF; case 0x30A: return ch[0].tgr[1] >> 8; case 0x30B: return ch[0].tgr[1] & 0xFF;
        case 0x30C: return ch[0].tgr[2] >> 8; case 0x30D: return ch[0].tgr[2] & 0xFF; case 0x30E: return ch[0].tgr[3] >> 8; case 0x30F: return ch[0].tgr[3] & 0xFF;
        case 0x320: return ch[0].tgr[4] >> 8; case 0x321: return ch[0].tgr[4] & 0xFF; case 0x322: return ch[0].tgr[5] >> 8; case 0x323: return ch[0].tgr[5] & 0xFF;
        case 0x324: return ch[0].tier2; case 0x325: return ch[0].tsr2; case 0x326: return ch[0].tbtm;
        case 0x380: return ch[1].tcr; case 0x381: return ch[1].tmdr; case 0x382: return ch[1].tiorh; case 0x384: return ch[1].tier; case 0x385: return ch[1].tsr;
        case 0x386: return ch[1].tcnt >> 8; case 0x387: return ch[1].tcnt & 0xFF; case 0x388: return ch[1].tgr[0] >> 8; case 0x389: return ch[1].tgr[0] & 0xFF; case 0x38A: return ch[1].tgr[1] >> 8; case 0x38B: return ch[1].tgr[1] & 0xFF;
        case 0x390: return ticcr;
        default: fprintf(stderr, "[MTU2] read unknown %08X\n", a); return 0;
        }
    };
    for (int i = 0; i < size; i++) v = (v << 8) | rd8(o + i);
    if (log) fprintf(stderr, "[MTU2] rd %08X -> %0*X\n", a, size * 2, v);
    return v;
}

void Mtu2::write(uint32_t a, int size, uint32_t val) {
    if (log) fprintf(stderr, "[MTU2] wr %08X <- %0*X\n", a, size * 2, val);
    uint32_t o = a & 0x3FF;
    auto wr8 = [&](uint32_t off, uint8_t v) {
        auto hi = [&](uint16_t& r) { r = (r & 0x00FF) | (v << 8); };
        auto lo = [&](uint16_t& r) { r = (r & 0xFF00) | v; };
        auto wtsr = [&](Ch& h) { h.tsr = (h.tsr & ~0x3F) | (v & h.tsr & 0x3F) | 0xC0; };   // flags clear by writing 0 after reading 1
        switch (off) {
        case 0x000: ch[2].tcr = v; break; case 0x001: ch[2].tmdr = v; break; case 0x002: ch[2].tiorh = v; break; case 0x004: ch[2].tier = v; break; case 0x005: wtsr(ch[2]); break;
        case 0x006: hi(ch[2].tcnt); break; case 0x007: lo(ch[2].tcnt); break; case 0x008: hi(ch[2].tgr[0]); break; case 0x009: lo(ch[2].tgr[0]); break; case 0x00A: hi(ch[2].tgr[1]); break; case 0x00B: lo(ch[2].tgr[1]); break;
        case 0x200: ch[3].tcr = v; break; case 0x201: ch[4].tcr = v; break; case 0x202: ch[3].tmdr = v; break; case 0x203: ch[4].tmdr = v; break;
        case 0x204: ch[3].tiorh = v; break; case 0x205: ch[3].tiorl = v; break; case 0x206: ch[4].tiorh = v; break; case 0x207: ch[4].tiorl = v; break;
        case 0x208: ch[3].tier = v; break; case 0x209: ch[4].tier = v; break; case 0x20A: toer = v | 0xC0; break; case 0x20D: tgcr = v; break; case 0x20E: tocr1 = v; break; case 0x20F: tocr2 = v; break;
        case 0x210: hi(ch[3].tcnt); break; case 0x211: lo(ch[3].tcnt); break; case 0x212: hi(ch[4].tcnt); break; case 0x213: lo(ch[4].tcnt); break;
        case 0x214: hi(tcdr); break; case 0x215: lo(tcdr); break; case 0x216: hi(tddr); break; case 0x217: lo(tddr); break;
        case 0x218: hi(ch[3].tgr[0]); break; case 0x219: lo(ch[3].tgr[0]); break; case 0x21A: hi(ch[3].tgr[1]); break; case 0x21B: lo(ch[3].tgr[1]); break;
        case 0x21C: hi(ch[4].tgr[0]); break; case 0x21D: lo(ch[4].tgr[0]); break; case 0x21E: hi(ch[4].tgr[1]); break; case 0x21F: lo(ch[4].tgr[1]); break;
        case 0x222: hi(tcbr); break; case 0x223: lo(tcbr); break;
        case 0x224: hi(ch[3].tgr[2]); break; case 0x225: lo(ch[3].tgr[2]); break; case 0x226: hi(ch[3].tgr[3]); break; case 0x227: lo(ch[3].tgr[3]); break;
        case 0x228: hi(ch[4].tgr[2]); break; case 0x229: lo(ch[4].tgr[2]); break; case 0x22A: hi(ch[4].tgr[3]); break; case 0x22B: lo(ch[4].tgr[3]); break;
        case 0x22C: wtsr(ch[3]); break; case 0x22D: wtsr(ch[4]); break; case 0x230: titcr = v; break; case 0x232: tbter = v; break; case 0x234: tder = v; break;
        case 0x238: ch[3].tbtm = v; break; case 0x239: ch[4].tbtm = v; break; case 0x240: hi(tadcr); break; case 0x241: lo(tadcr); break;
        case 0x244: hi(tadcora); break; case 0x245: lo(tadcora); break; case 0x246: hi(tadcorb); break; case 0x247: lo(tadcorb); break;
        case 0x248: hi(tadcobra); break; case 0x249: lo(tadcobra); break; case 0x24A: hi(tadcobrb); break; case 0x24B: lo(tadcobrb); break;
        case 0x080: hi(c5[0].tcnt); break; case 0x081: lo(c5[0].tcnt); break; case 0x082: hi(c5[0].tgr); break; case 0x083: lo(c5[0].tgr); break; case 0x084: c5[0].tcr = v; break; case 0x086: c5[0].tior = v; break;
        case 0x090: hi(c5[1].tcnt); break; case 0x091: lo(c5[1].tcnt); break; case 0x092: hi(c5[1].tgr); break; case 0x093: lo(c5[1].tgr); break; case 0x094: c5[1].tcr = v; break; case 0x096: c5[1].tior = v; break;
        case 0x0A0: hi(c5[2].tcnt); break; case 0x0A1: lo(c5[2].tcnt); break; case 0x0A2: hi(c5[2].tgr); break; case 0x0A3: lo(c5[2].tgr); break; case 0x0A4: c5[2].tcr = v; break; case 0x0A6: c5[2].tior = v; break;
        case 0x0B0: tsr5 = (v & tsr5 & 7); break;    // flags cleared by writing 0
        case 0x0B2: tier5 = v & 7; break; case 0x0B4: tstr5 = v & 7; break; case 0x0B6: tcntcmpclr = v & 7; break;
        case 0x260: twcr = v; break; case 0x280: tstr = v; break; case 0x281: tsyr = v; break; case 0x284: trwer = v; break;
        case 0x300: ch[0].tcr = v; break; case 0x301: ch[0].tmdr = v; break; case 0x302: ch[0].tiorh = v; break; case 0x303: ch[0].tiorl = v; break; case 0x304: ch[0].tier = v; break; case 0x305: wtsr(ch[0]); break;
        case 0x306: hi(ch[0].tcnt); break; case 0x307: lo(ch[0].tcnt); break;
        case 0x308: hi(ch[0].tgr[0]); break; case 0x309: lo(ch[0].tgr[0]); break; case 0x30A: hi(ch[0].tgr[1]); break; case 0x30B: lo(ch[0].tgr[1]); break;
        case 0x30C: hi(ch[0].tgr[2]); break; case 0x30D: lo(ch[0].tgr[2]); break; case 0x30E: hi(ch[0].tgr[3]); break; case 0x30F: lo(ch[0].tgr[3]); break;
        case 0x320: hi(ch[0].tgr[4]); break; case 0x321: lo(ch[0].tgr[4]); break; case 0x322: hi(ch[0].tgr[5]); break; case 0x323: lo(ch[0].tgr[5]); break;
        case 0x324: ch[0].tier2 = v; break; case 0x325: ch[0].tsr2 = (ch[0].tsr2 & ~3) | (v & ch[0].tsr2 & 3) | 0xC0; break; case 0x326: ch[0].tbtm = v; break;
        case 0x380: ch[1].tcr = v; break; case 0x381: ch[1].tmdr = v; break; case 0x382: ch[1].tiorh = v; break; case 0x384: ch[1].tier = v; break; case 0x385: wtsr(ch[1]); break;
        case 0x386: hi(ch[1].tcnt); break; case 0x387: lo(ch[1].tcnt); break; case 0x388: hi(ch[1].tgr[0]); break; case 0x389: lo(ch[1].tgr[0]); break; case 0x38A: hi(ch[1].tgr[1]); break; case 0x38B: lo(ch[1].tgr[1]); break;
        case 0x390: ticcr = v; break;
        default: fprintf(stderr, "[MTU2] write unknown %08X = %X\n", a, v);
        }
    };
    for (int i = size - 1; i >= 0; i--) { wr8(o + i, val & 0xFF); val >>= 8; }
    for (int c = 0; c < 5; c++) updateIrq(c);
    for (int i = 0; i < 3; i++) intc.setLine(vecCh5[i], (tier5 & tsr5 & (1 << (2 - i))) != 0);
}
