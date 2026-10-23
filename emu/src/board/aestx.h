// AES/EBU transmitter chip on the board PLD's bit-banged serial port (PLD +0: bit8 nCS, bit9 MOSI/SDA,
// bit10 SCLK, bit11 MISO). The host supports two parts and probes for them at boot (FUN_0c14d580):
//   AK4101: 16-bit command 0xC0 | 0, reg (read) then 8 data bits; detected when reg0 & 0x8C == 0x80
//           (FUN_0c0fff32 bit-bangs through PLD +0 bits 8..11)
//   CS8406: SPI mode, chip address 0x20 (write) / 0x21 (read), MAP byte with auto-increment; detected when
//           register 0x7F (ID/version) & 0xF0 == 0xE0 (FUN_0c13a148, pin functions 0c13a27e..0c13a2ea)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

class AesTx {
public:
    enum Chip { NONE = 0, AK4101 = 1, CS8406 = 2 };
    int chip = AK4101;
    bool log = false;
    uint8_t ak[256] = {}, cs[128] = {};
    uint64_t transfers = 0;

    AesTx() { ak[0] = 0x80; cs[0x7F] = 0xE0; }

    // pin changes: called with the new levels whenever PLD +0 bits 8..10 change
    void pins(bool ncs, bool clk, bool mosi) {
        if (ncs != lastCs) {
            lastCs = ncs;
            if (!ncs) { nbits = 0; shift = 0; phase = 0; misoBit = false; outByte = 0; }        // selected: start of a transfer
            else if (nbits) { finish(); }
        }
        if (!ncs && clk && !lastClk) {                                                            // rising edge: sample MOSI
            shift = (shift << 1) | (mosi ? 1 : 0); nbits++;
            onBit();
        }
        if (!ncs && !clk && lastClk) { prepareMiso(); }                                           // falling edge: present next output bit
        lastClk = clk;
    }
    bool miso() const { return misoBit; }

private:
    bool lastCs = true, lastClk = false, misoBit = false;
    uint32_t shift = 0; int nbits = 0; int phase = 0;
    uint8_t outByte = 0, map = 0, chipAddr = 0; int outBits = 0;

    void onBit() {
        if (chip == AK4101) {
            if (nbits == 16) {                                                                  // first byte 0xC0 = read, 0xC1 = write (FUN_0c0fff32 / FUN_0c0fffec)
                uint16_t cmd = shift & 0xFFFF;
                if ((cmd >> 8) == 0xC0) {
                    uint8_t reg = cmd & 0xFF; outByte = ak[reg]; outBits = 8; phase = 1; transfers++;
                    if (log) fprintf(stderr, "[AES] AK4101 read  reg %02X -> %02X%c", reg, outByte, 10);
                } else if ((cmd >> 8) == 0xC1) phase = 2;                                         // data byte follows
            } else if (nbits == 24 && phase == 2) {
                uint8_t reg = (shift >> 8) & 0xFF, val = shift & 0xFF;
                ak[reg] = val; transfers++; phase = 0;
                if (log) fprintf(stderr, "[AES] AK4101 write reg %02X <- %02X%c", reg, val, 10);
            }
        } else if (chip == CS8406) {
            if (nbits % 8 == 0) {
                uint8_t b = shift & 0xFF; int idx = nbits / 8;
                if (idx == 1) { chipAddr = b; if (chipAddr == 0x21) { outByte = cs[map & 0x7F]; outBits = 8; if (log) fprintf(stderr, "[AES] CS8406 read  reg %02X -> %02X\n", map & 0x7F, outByte); map = (map & 0x80) ? map + 1 : map; transfers++; } }
                else if (chipAddr == 0x20) { if (idx == 2) map = b; else { cs[map & 0x7F] = b; if (log) fprintf(stderr, "[AES] CS8406 write reg %02X <- %02X\n", map & 0x7F, b); if (map & 0x80) map++; transfers++; } }
                else if (chipAddr == 0x21) { outByte = cs[map & 0x7F]; outBits = 8; if (map & 0x80) map++; }
            }
        }
    }
    void prepareMiso() {
        if (outBits > 0) { misoBit = (outByte & 0x80) != 0; outByte <<= 1; outBits--; }
        else misoBit = false;
    }
    void finish() { phase = 0; }
};
