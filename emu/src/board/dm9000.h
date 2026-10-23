// Davicom DM9000 Ethernet controller on CS6 (0x18000000): 16-bit index port at +0, data port at +2,
// interrupt on SH-2A IRQ0 (vector 64). Register model after the DM9000A datasheet, enough for the Rio
// firmware's driver (reset, PHY via EPCR/EPAR/EPDR, MAC/multicast filters, TX through MWCMD, RX through
// MRCMD with the 4-byte status/length header, ISR/IMR). Frames go to a pluggable backend (null, pcap dump,
// Linux TAP); the link is reported up at 100 Mbit full duplex.
#pragma once
#include "bus.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <functional>

class Dm9000 : public MmioDevice {
public:
    const char* name() const override { return "DM9000"; }
    bool log = false, logFrames = false;
    std::function<void(bool)> setIrq;                        // -> INTC IRQ0
    std::function<void(const uint8_t*, size_t)> txBackend;   // frames leaving the chip
    uint64_t txFrames = 0, rxFrames = 0;
    uint8_t mac[6] = {};

    Dm9000() { reset(true); }

    // time base from the machine (ms): the PHY link comes up ~300 ms after it is powered (GPR bit0 cleared)
    void tick(uint64_t ms) {
        nowMs = ms;
        if (linkUpAtMs && ms >= linkUpAtMs) { linkUpAtMs = 0; linkUp = true; reg[0xFE] |= 0x20; irqUpdate(); if (logFrames) fprintf(stderr, "[DM9000] link up\n"); }
    }

    // deliver a frame from the network into the RX ring (called by the backend / machine)
    void receive(const uint8_t* p, size_t n) {
        if (!(reg[0x05] & 0x01) || n < 14 || n > 1536) return;          // RCR RXEN off / bad size
        bool prmsc = reg[0x05] & 0x02;
        if (!prmsc && memcmp(p, mac, 6) != 0 && !(p[0] & 1)) return;    // not for us
        size_t len = (n < 60 ? 60 : n) + 4;                             // padded + CRC
        if (rxFree() < len + 4) { reg[0xFE] |= 0x04; irqUpdate(); return; }   // ROS: RX overflow
        uint8_t hdr[4] = { 0x01, 0x00, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };   // ready, status, length (LE)
        for (int i = 0; i < 4; i++) rxPut(hdr[i]);
        for (size_t i = 0; i < n; i++) rxPut(p[i]);
        for (size_t i = n; i < len; i++) rxPut(0);
        rxFrames++;
        reg[0xFE] |= 0x01;                                              // PRS: packet received
        irqUpdate();
        if (logFrames) fprintf(stderr, "[DM9000] rx frame %zu bytes\n", n);
    }

    uint32_t read(uint32_t addr, int size) override {
        uint32_t o = addr & 0x3, v = 0;
        if (o == 0) v = index;
        else {
            switch (index) {
            case 0x01: v = (reg[1] & 0x2C) | (linkUp ? 0x40 : 0); break;   // NSR: LINKST, 100 Mbit (SPEED bit clear)
            case 0x0B: v = reg[0x0B] & ~0x09; break;                    // EPCR: access finished
            case 0x0D: v = epdrl; break;
            case 0x0E: v = epdrh; break;
            case 0x28: v = 0x46; break; case 0x29: v = 0x0A; break;     // VID 0x0A46
            case 0x2A: v = 0x00; break; case 0x2B: v = 0x90; break;     // PID 0x9000
            case 0x2C: v = 0x19; break;                                 // CHIPR (DM9000A)
            case 0xF0: v = sram[rxPtr]; break;                          // MRCMDX: peek without increment
            case 0xF2: v = mrcmd(); break;                              // MRCMD: read with increment (16-bit)
            case 0xF4: v = rxPtr & 0xFF; break; case 0xF5: v = (rxPtr >> 8) & 0xFF; break;   // MRR
            case 0xFA: v = txPtr & 0xFF; break; case 0xFB: v = (txPtr >> 8) & 0xFF; break;   // MWR
            case 0xFE: v = reg[0xFE]; break;                            // ISR (IOMODE bit 7 = 0: 16-bit bus)
            default: v = reg[index]; break;
            }
        }
        if (log && index != 0xF2) fprintf(stderr, "[DM9000] rd %s %02X -> %04X\n", o == 0 ? "index" : "data ", index, v);
        return size == 1 ? (v & 0xFF) : v;
    }

    void write(uint32_t addr, int size, uint32_t val) override {
        uint32_t o = addr & 0x3;
        if (o == 0) { index = val & 0xFF; return; }
        if (log && index != 0xF8) fprintf(stderr, "[DM9000] wr data  %02X <- %04X\n", index, val);
        switch (index) {
        case 0x00: reg[0] = val & 0xFE; if (val & 0x01) reset(false); break;          // NCR: RST self-clears
        case 0x01: reg[1] &= ~(val & 0x2C); break;                                    // NSR: write 1 clears TX1END/TX2END/WAKEST
        case 0x02: reg[2] = val & 0xFF; if (val & 0x01) transmit(); break;            // TCR: TXREQ
        case 0x0B: reg[0x0B] = val & 0xFF; if (val & 0x08) phyAccess(); break;       // EPCR: ERRE (bit3) starts an EEPROM/PHY cycle
        case 0x0D: epdrl = val & 0xFF; break;
        case 0x0E: epdrh = val & 0xFF; break;
        case 0xF4: rxPtr = (rxPtr & 0xFF00) | (val & 0xFF); break;
        case 0xF5: rxPtr = (rxPtr & 0x00FF) | ((val & 0xFF) << 8); break;
        case 0xF8: sramPut16(val); break;                                             // MWCMD: write with increment
        case 0xFC: txLen = (txLen & 0xFF00) | (val & 0xFF); break;                    // TXPLL
        case 0xFD: txLen = (txLen & 0x00FF) | ((val & 0xFF) << 8); break;             // TXPLH
        case 0xFE: reg[0xFE] &= ~(val & 0x3F); irqUpdate(); break;                    // ISR: write 1 to clear
        case 0xFF: reg[0xFF] = val & 0xFF; irqUpdate(); break;                        // IMR
        case 0x1F: reg[0x1F] = val & 0xFF;                                            // GPR: bit0 PHY power down
                   if (val & 0x01) { linkUp = false; linkUpAtMs = 0; }
                   else if (!linkUp && !linkUpAtMs) linkUpAtMs = nowMs + 300;
                   break;
        default: reg[index] = val & 0xFF; if (index >= 0x10 && index <= 0x15) mac[index - 0x10] = val & 0xFF; break;
        }
    }

private:
    uint8_t reg[256] = {};
    uint8_t index = 0;
    uint8_t epdrl = 0, epdrh = 0;
    uint16_t phy[32] = {};
    uint8_t sram[0x4000] = {};           // 16 KB: 0x0000..0x0BFF TX, 0x0C00..0x3FFF RX ring
    uint16_t txPtr = 0, txLen = 0;
    uint16_t rxPtr = 0x0C00;             // host read pointer (MRR)
    uint16_t rxWr = 0x0C00;              // chip write pointer
    bool irqLevel = false, linkUp = false;
    uint64_t nowMs = 0, linkUpAtMs = 0;

    void reset(bool hard) {
        memset(reg, 0, sizeof reg);
        reg[0x08] = 0x37; reg[0x09] = 0x38; reg[0x0A] = 0x29;
        reg[0x1F] = 0x01;                                 // GPR: PHY powered down after reset
        linkUp = false; linkUpAtMs = 0;
        txPtr = 0; rxPtr = rxWr = 0x0C00; txLen = 0;
        memset(phy, 0, sizeof phy);                      // internal 10/100 PHY: autoneg done, link up, 100 FDX
        phy[0] = 0x3100; phy[1] = 0x786D; phy[2] = 0x0181; phy[3] = 0xB8A0; phy[4] = 0x01E1; phy[5] = 0x45E1; phy[0x11] = 0x4000;
        if (hard) index = 0;
        irqUpdate();
    }
    void irqUpdate() {
        bool a = (reg[0xFE] & reg[0xFF] & 0x3F) != 0;
        if (a != irqLevel) { irqLevel = a; if (setIrq) setIrq(a); }
    }
    // PHY MII access: EPCR bit2 EPOS selects the PHY, bit1 ERPRW write / bit2+bit3 read; EPAR bits 5:0 register
    void phyAccess() {
        if (reg[0x0B] & 0x04) {
            int r = reg[0x0C] & 0x1F;
            if (reg[0x0B] & 0x02) {
                uint16_t v = (epdrh << 8) | epdrl;
                if (r == 0) v &= ~0x8200;                 // reset / restart-autoneg complete at once
                phy[r] = v;
            } else {
                epdrl = phy[r] & 0xFF; epdrh = phy[r] >> 8;
            }
        }
        reg[0x0B] &= ~0x08;
    }
    void sramPut16(uint32_t val) {
        sram[txPtr] = val & 0xFF; sram[(txPtr + 1) % 0x0C00] = (val >> 8) & 0xFF;
        txPtr = (uint16_t)((txPtr + 2) % 0x0C00);
    }
    void transmit() {
        size_t n = txLen;
        std::vector<uint8_t> f(n);
        uint16_t start = (uint16_t)((txPtr + 0x0C00 - ((n + 1) & ~1u)) % 0x0C00);
        for (size_t i = 0; i < n; i++) f[i] = sram[(start + i) % 0x0C00];
        txFrames++;
        if (logFrames && n >= 14) fprintf(stderr, "[DM9000] tx frame %zu bytes to %02X:%02X:%02X:%02X:%02X:%02X type %02X%02X\n", n, f[0], f[1], f[2], f[3], f[4], f[5], f[12], f[13]);
        if (txBackend) txBackend(f.data(), n);
        reg[0x02] &= ~0x01;                               // TXREQ done
        reg[0x01] |= 0x04;                                // NSR TX1END
        reg[0xFE] |= 0x02;                                // ISR PTS
        irqUpdate();
    }
    size_t rxFree() const { size_t used = (rxWr + 0x3400 - rxPtr) % 0x3400; return 0x3400 - used - 1; }
    void rxPut(uint8_t b) { sram[rxWr] = b; rxWr = (uint16_t)(rxWr + 1); if (rxWr >= 0x4000) rxWr = 0x0C00; }
    uint32_t mrcmd() {
        uint32_t lo = sram[rxPtr]; uint16_t p1 = (uint16_t)(rxPtr + 1 >= 0x4000 ? 0x0C00 : rxPtr + 1);
        uint32_t v = lo | (sram[p1] << 8);
        rxPtr = (uint16_t)(rxPtr + 2); if (rxPtr >= 0x4000) rxPtr = (uint16_t)(0x0C00 + (rxPtr - 0x4000));
        return v;
    }
};
