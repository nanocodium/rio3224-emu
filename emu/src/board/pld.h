// Rio3224-D2 board PLD at 0x40001400 (16-bit registers). Semantics recovered from RIO2_PROG.BIN:
//   +0x00  control / reset lines (debug monitor "misc" names): bit0 res_dsp, bit1 res_mac, bit2 res_dnt,
//          bit3 res_ada, bit4/5 pllinh?, bit7 res_hacpu, bits 8..11 gpo[0-3]; readback of bit15 polled
//   +0x02  serial buses:
//            bit1  nCS   FPGA identification bus (active low)       bit2 CLK   bit3 MOSI   bits4-7 MISO from FPGA1..4
//            bit8/9/10/12/0  front-panel LED shift chain: bit9 CLK (data latched on rising edge), bit12 data
//                  chain A, bit0 data chain B, bit10 latch pulse (FUN_0c1c7914 waveform, 96 bits per chain),
//                  bit8 output enable (held high while shifting)
//            bits 13.. other outputs
//   +0x04  board / sheet id in bits 12:8 (0x18 -> Rio3224-D2 code 9, 0x19 -> Rio1608-D2 code 10); bits 2,13,14,15 outputs
//   +0x06  outputs: bit7 OLED D/C# (1 = data), bits 10/11 OLED reset, bits 12..15 "UP port" flags to the FPGAs
//   +0x0A  inputs: DIP switches (active low) in bits 4:1, other switches/status TBD
//   +0x10  inputs: bits 3:0 option code, bits 4,5,6 switches
#pragma once
#include "bus.h"
#include "aestx.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

class Pld : public MmioDevice {
public:
    const char* name() const override { return "PLD"; }

    uint32_t read(uint32_t addr, int size) override {
        uint32_t o = addr & 0xFF, v = 0;
        switch (o) {
        case 0x00: v = (ctrl & 0x17FF) | encBits() | (aes.miso() ? 0x0800 : 0) | (button(5) ? 0 : 0x8000); break;   // bit11: AES chip MISO, bit15: front-panel switch 5 (active low)
        case 0x02: v = (reg2 & ~0x00F0u & 0x3FFFu) | (fpgaMiso << 4) | (button(3) ? 0 : 0x8000) | (button(4) ? 0 : 0x4000);   // bits 15/14: switches 3/4
                   if (logSerial && !(reg2 & 0x02)) fprintf(stderr, "[PLD] fpga bus read -> miso %X (bits %d)\n", fpgaMiso, fpgaBits); break;
        case 0x04:   // low byte: bits 0,1,3 Dante GPI0/GPI1/AUXB (active high), bits 4..7 IRQ1 sources, ACTIVE LOW
                     //   (bit4 main FPGA interrupt, bit6 power-off detect; FUN_0c1c819c/81ae return 1 when the bit is 0)
                   v = ((boardId & 0x1F) << 8) | (reg4 & 0xE004) | (uvlo ? 0x30 : 0x70) | (gpiInputs ? gpiInputs() & 0x0B : 0);   // bit6 low = /UVLO: supply voltage falling -> firmware power-off sequence (saves settings)
                   if (irqInputs) v &= ~irqInputs();
                   if (!fanOk) v |= 0x80;                                                        // bit7: fan alarm, LOW = fan running (FUN_0c1c82ca; high -> E00-05 FAN HAS MALFUNCTIONED)
                   break;
        case 0x06: v = reg6; break;
        case 0x08: v = reg8; break;
        case 0x0A: v = inputs(); break;
        case 0x10: v = reg10; break;
        default: fprintf(stderr, "[PLD] read unknown +%02X\n", o); v = 0;
        }
        if (size == 1) v = (addr & 1) ? (v & 0xFF) : (v >> 8);
        if (log) fprintf(stderr, "[PLD] rd +%02X -> %04X\n", o, v);
        return v;
    }

    void write(uint32_t addr, int size, uint32_t val) override {
        uint32_t o = addr & 0xFF;
        if (log) fprintf(stderr, "[PLD] wr +%02X <- %04X\n", o, val);
        switch (o) {
        case 0x00: { uint16_t old = ctrl; ctrl = val; if (onCtrl) onCtrl(val);
                     if ((old ^ val) & 0x0700) aes.pins(!(val & 0x100) ? false : true, (val & 0x400) != 0, (val & 0x200) != 0); break; }
        case 0x02: writeReg2(val); break;
        case 0x04: reg4 = val; break;
        case 0x06: reg6 = val; break;
        case 0x08: reg8 = val; break;
        default: fprintf(stderr, "[PLD] write unknown +%02X = %X\n", o, val);
        }
    }

    uint16_t inputs() const {
        uint16_t v = 0xFFFF;
        v &= ~((dipSw & 0xF) << 1);          // DIP switches active-low in bits 4:1
        if (button(1) || master48) v &= ~1u; // bit0: the rear +48V MASTER switch (scanned as "switch 1"; low = ON)
        if (poffA) v &= ~0x20u;             // bit5: /POFF1 low = power supply A failed (E00-13)
        if (poffB) v &= ~0x40u;             // bit6: /POFF2 low = power supply B failed
        return v & inputMaskHigh;
    }
    // front-panel switches as scanned by the PNLSCAN task (FUN_0c1c7700): index 1 = +0xA bit0, 3 = +2 bit15,
    // 4 = +2 bit14, 5 = +0 bit15; index 0 is the 4-bit DIP field. All active low.
    uint8_t buttons = 0;                     // bit n set = switch n pressed
    uint8_t encPhase = 0;                    // rotary encoder quadrature phase 0..3 -> +0 bit13 (A) / bit14 (B); FUN_0c1c8220 reads (PLD[0] >> 13) & 3
    uint16_t encBits() const { static const uint8_t gray[4] = { 0, 1, 3, 2 }; return (uint16_t)(gray[encPhase & 3] << 13); }
    bool button(int n) const { return (buttons >> n) & 1; }
    bool fanOk = true;                       // fan alarm input on +4 bit7 (low = running)
    AesTx aes;                               // AES/EBU transmitter on the +0 bit-banged serial port (bits 8..11)

    // ---- FPGA identification bus: 7-bit command, 8-bit reply per FPGA (4 MISO lines) ----
    //   cmd 0 = device code (model table expects 3), cmd 1 = version, cmd 2 = status: bits 6:4 device type (1 = "AK (10M02)")
    uint8_t fpgaBid[4] = { 3, 3, 3, 3 };
    uint8_t fpgaVer[4] = { 1, 1, 1, 1 };
    uint8_t fpgaStatus[4] = { 0x10, 0x10, 0x10, 0x10 };

    // ---- front-panel LED chains (96 bits each), latched copy visible to the user ----
    static constexpr int kLeds = 96;
    bool led[2][kLeds] = {};                 // latched state, index = position in the chain
    uint32_t ledLatches = 0;
    std::function<void()> onLedLatch;

    uint16_t ctrl = 0, reg2 = 0, reg4 = 0, reg6 = 0, reg8 = 0, reg10 = 0;
    uint8_t boardId = 0x18;     // sheet id: 0x18 -> code 9 -> Rio3224-D2 (0x19 -> Rio1608-D2)
    uint8_t dipSw = 0;          // 4 DIP switches, 1 = ON
    uint16_t inputMaskHigh = 0xFFFF;
    bool uvlo = false, poffA = false, poffB = false, master48 = false;   // PSU / rear-panel states (see read +4 / inputs())
    bool log = false, logSerial = false;
    std::function<void(uint16_t)> onCtrl;
    std::function<uint16_t()> irqInputs;    // pending interrupt sources: returned bits are CLEARED in +4 (active low)
    std::function<uint16_t()> gpiInputs;    // Dante module GPI0 (bit0), GPI1 (bit1), AUXB (bit3) levels

    // text rendering of the LED chains (position in chain; the firmware's map for sheet 9 puts
    // LED index 0x5F..0x00 on chain A and 0x6F..0x60, 0x77..0x70 on the tail of chain B)
    // logical LED ids (firmware LED controller, 124 entries) per chain position for board sheet 9
    // (map table at 0x0C2E56D0): ids 0..31 = +48V in 1-32, 32..63 = SIG in 1-32, 64..95 = PEAK in 1-32,
    // 96..111 = SIG out 1-16, 112..119 = SIG AES out 1-8 (FUN_0c1c5f84 phases 0/1/2); ids 120..123 are the
    // Dante/sync status LEDs driven outside the chains (FUN_0c168bfe), read back from the panel object by the machine.
    static const uint8_t* chainMapA() {
        static const uint8_t m[kLeds] = { 0x5f,0x5e,0x5d,0x5c,0x5b,0x5a,0x59,0x58, 0x1f,0x1e,0x1d,0x1c,0x1b,0x1a,0x19,0x18, 0x3f,0x3e,0x3d,0x3c,0x3b,0x3a,0x39,0x38,
                                          0x57,0x56,0x55,0x54,0x53,0x52,0x51,0x50, 0x17,0x16,0x15,0x14,0x13,0x12,0x11,0x10, 0x37,0x36,0x35,0x34,0x33,0x32,0x31,0x30,
                                          0x4f,0x4e,0x4d,0x4c,0x4b,0x4a,0x49,0x48, 0x0f,0x0e,0x0d,0x0c,0x0b,0x0a,0x09,0x08, 0x2f,0x2e,0x2d,0x2c,0x2b,0x2a,0x29,0x28,
                                          0x47,0x46,0x45,0x44,0x43,0x42,0x41,0x40, 0x07,0x06,0x05,0x04,0x03,0x02,0x01,0x00, 0x27,0x26,0x25,0x24,0x23,0x22,0x21,0x20 };
        return m;
    }
    static const uint8_t* chainMapB() {
        static uint8_t m[kLeds]; static bool init = false;
        if (!init) { memset(m, 0xFF, sizeof m); for (int i = 0; i < 16; i++) m[72 + i] = 0x6f - i; for (int i = 0; i < 8; i++) m[88 + i] = 0x77 - i; init = true; }
        return m;
    }
    std::function<int(int id)> ledExtra;     // state of LED ids outside the chains (120..123), -1 = unknown
    int ledById(int id) const {              // decoded LED state by logical id
        const uint8_t* a = chainMapA(); const uint8_t* b = chainMapB();
        for (int i = 0; i < kLeds; i++) { if (a[i] == id) return led[0][i]; if (b[i] == id) return led[1][i]; }
        return ledExtra ? ledExtra(id) : -1;
    }
    std::string ledAscii() const {
        std::string s;
        auto row = [&](const char* name, int base, int n) {
            s += name; s += ": ";
            for (int i = 0; i < n; i++) { int v = ledById(base + i); s += v < 0 ? '?' : v ? '#' : '.'; if (i % 8 == 7) s += ' '; }
            s += '\n';
        };
        row("+48V in 01-32  ", 0, 32); row("SIG  in 01-32  ", 32, 32); row("PEAK in 01-32  ", 64, 32);
        row("SIG  out 01-16 ", 96, 16); row("SIG  AES out1-8", 112, 8);
        const char* st[4] = { "DANTE PRIMARY", "DANTE PRIMARY ACT", "DANTE SECONDARY", "SYNC" };
        s += "status: ";
        for (int i = 0; i < 4; i++) { int v = ledById(120 + i); s += st[i]; s += v < 0 ? "=? " : v ? "=on " : "=off "; }
        s += '\n';
        s += "raw chain A: "; for (int i = 0; i < kLeds; i++) { s += led[0][i] ? '#' : '.'; if (i % 8 == 7) s += ' '; } s += '\n';
        s += "raw chain B: "; for (int i = 0; i < kLeds; i++) { s += led[1][i] ? '#' : '.'; if (i % 8 == 7) s += ' '; } s += '\n';
        return s;
    }

private:
    // FPGA ID bus state
    int fpgaBits = 0; uint8_t fpgaCmd = 0; uint8_t fpgaMiso = 0;
    // LED chain shift registers
    bool shiftA[kLeds] = {}, shiftB[kLeds] = {};

    void writeReg2(uint16_t val) {
        uint16_t old = reg2; reg2 = val;
        if (logSerial && !(val & 0x02)) fprintf(stderr, "[PLD] +2 <- %04X (fpga bus: cs0 clk%d mosi%d bits %d)\n", val, (val >> 2) & 1, (val >> 3) & 1, fpgaBits);
        // --- FPGA ID bus: nCS bit1, CLK bit2, MOSI bit3 ---
        if (val & 0x02) { fpgaBits = 0; fpgaMiso = 0; }
        else if ((val & 0x04) && !(old & 0x04)) {               // rising clock edge
            if (fpgaBits < 7) {
                fpgaCmd = (fpgaCmd << 1) | ((val >> 3) & 1);
                fpgaBits++;
                if (fpgaBits == 7 && logSerial) fprintf(stderr, "[PLD] fpga-id bus cmd %d\n", fpgaCmd & 0x7F);
            } else if (fpgaBits < 15) {
                int bit = 7 - (fpgaBits - 7);                    // MSB first
                uint8_t m = 0;
                for (int i = 0; i < 4; i++) { int c = fpgaCmd & 0x7F; uint8_t r = c == 0 ? fpgaBid[i] : c == 1 ? fpgaVer[i] : c == 2 ? fpgaStatus[i] : 0; if ((r >> bit) & 1) m |= 1 << i; }
                fpgaMiso = m; fpgaBits++;
            }
        }
        // --- LED chains: CLK bit9 (rising edge shifts data bit12 -> chain A, bit0 -> chain B), latch bit10 ---
        if ((val & 0x200) && !(old & 0x200)) {
            memmove(shiftA + 1, shiftA, (kLeds - 1) * sizeof(bool)); shiftA[0] = (val >> 12) & 1;
            memmove(shiftB + 1, shiftB, (kLeds - 1) * sizeof(bool)); shiftB[0] = val & 1;
        }
        if ((val & 0x400) && !(old & 0x400)) {
            // the firmware shifts the sheet-9 map out in table order (entry 0 first), so the last bit clocked (shift[0])
            // is table entry kLeds-1: store by table position so led[c][p] matches the map at 0x0C2E56D0
            for (int i = 0; i < kLeds; i++) { led[0][kLeds - 1 - i] = shiftA[i]; led[1][kLeds - 1 - i] = shiftB[i]; }
            ledLatches++;
            if (onLedLatch) onLedLatch();
        }
    }
};
