// Solomon SSD1322 OLED controller model (480x128 GDDRAM, 4 bit/pixel).
// The Rio3224-D2 firmware drives it through a byte FIFO in the main FPGA (CS5 +0x1000) with the
// data/command line on PLD register +6 bit 7.  Command parameter counts match the firmware's own table.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>

class Ssd1322 {
public:
    static constexpr int kCols = 120;        // column addresses (4 pixels each)
    static constexpr int kRows = 128;
    uint8_t ram[kRows][kCols * 2] = {};      // two bytes per column address: 4 pixels
    bool on = false, log = false;
    uint32_t frames = 0;                     // number of completed RAM writes (0x5C bursts)
    std::function<void()> onFrame;           // called when a RAM write burst ends (next command)

    // active window set by the firmware (column 0x17..0x60, row 0..0x2F for the Rio3224-D2 panel)
    int colStart = 0, colEnd = 0x77, rowStart = 0, rowEnd = 0x7F;
    uint8_t remap = 0, remap2 = 0, startLine = 0, offset = 0, mux = 0x7F, contrast = 0x7F;

    void reset() { memset(ram, 0, sizeof ram); cmd = -1; need = 0; nParams = 0; writing = false; }

    void byte(uint8_t b, bool data) {
        if (!data) { command(b); return; }
        if (writing) { ramWrite(b); return; }
        if (cmd >= 0 && nParams < need) {
            params[nParams++] = b;
            if (nParams == need) finishCommand();
        } else if (log) fprintf(stderr, "[oled] stray data %02X\n", b);
    }

    // rendering helpers --------------------------------------------------------------------------
    // pixel (x,y) in controller coordinates, x 0..479, y 0..127 ; returns 0..15
    // The firmware packs 4 pixels per column address as  byte0 = (px3<<4)|px2, byte1 = (px1<<4)|px0
    // (see FUN_0c13495c); this is the pixel order the panel shows with the remap the firmware programs.
    bool yamahaOrder = true;
    uint8_t pixel(int x, int y) const {
        if (x < 0 || x >= kCols * 4 || y < 0 || y >= kRows) return 0;
        int k = x & 3;
        if (yamahaOrder) {
            uint8_t b = ram[y][(x >> 2) * 2 + (k < 2 ? 1 : 0)];
            return (k & 1) ? (b >> 4) : (b & 0xF);
        }
        uint8_t b = ram[y][(x >> 2) * 2 + (k >> 1)];
        return (k & 1) ? (b & 0xF) : (b >> 4);
    }
    bool writePgm(const std::string& path, int x0, int y0, int w, int h, bool flipX, bool flipY) const {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        fprintf(f, "P5\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int sx = flipX ? x0 + w - 1 - x : x0 + x, sy = flipY ? y0 + h - 1 - y : y0 + y;
            fputc(pixel(sx, sy) * 17, f);
        }
        fclose(f);
        return true;
    }
    std::string ascii(int x0, int y0, int w, int h, bool flipX, bool flipY) const {
        static const char ramp[] = " .:-=+*#%@";
        std::string s;
        for (int y = 0; y < h; y += 2) {
            for (int x = 0; x < w; x += 2) {
                int sum = 0;
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                    int sx = flipX ? x0 + w - 1 - (x + dx) : x0 + x + dx, sy = flipY ? y0 + h - 1 - (y + dy) : y0 + y + dy;
                    sum += pixel(sx, sy);
                }
                s += ramp[sum * 9 / 60];
            }
            s += '\n';
        }
        return s;
    }

private:
    int cmd = -1, need = 0, nParams = 0;
    uint8_t params[16] = {};
    bool writing = false;
    int col = 0, row = 0, byteIdx = 0;

    static int paramCount(uint8_t c) {
        switch (c) {
        case 0x15: case 0x75: case 0xA0: case 0xA8: case 0xB4: case 0xD1: return 2;
        case 0xA1: case 0xA2: case 0xAB: case 0xB1: case 0xB3: case 0xB5: case 0xB6: case 0xBB:
        case 0xBE: case 0xC1: case 0xC7: case 0xCA: case 0xFD: return 1;
        case 0xB8: return 15;
        default: return 0;
        }
    }
    void command(uint8_t c) {
        if (writing) { writing = false; frames++; if (onFrame) onFrame(); }
        cmd = c; need = paramCount(c); nParams = 0;
        if (need == 0) finishCommand();
    }
    void finishCommand() {
        switch (cmd) {
        case 0x15: colStart = params[0] & 0x7F; colEnd = params[1] & 0x7F; break;
        case 0x75: rowStart = params[0] & 0x7F; rowEnd = params[1] & 0x7F; break;
        case 0x5C: writing = true; col = colStart; row = rowStart; byteIdx = 0; break;
        case 0xA0: remap = params[0]; remap2 = params[1]; break;
        case 0xA1: startLine = params[0]; break;
        case 0xA2: offset = params[0]; break;
        case 0xAE: on = false; break;
        case 0xAF: on = true; break;
        case 0xC1: contrast = params[0]; break;
        case 0xCA: mux = params[0]; break;
        default: break;
        }
        if (log) {
            fprintf(stderr, "[oled] cmd %02X", cmd);
            for (int i = 0; i < nParams; i++) fprintf(stderr, " %02X", params[i]);
            fprintf(stderr, "\n");
        }
        if (cmd != 0x5C) cmd = -1;
    }
    void ramWrite(uint8_t b) {
        if (row < kRows && col < kCols) ram[row][col * 2 + byteIdx] = b;
        if (++byteIdx == 2) {
            byteIdx = 0;
            if (remap & 1) {                      // vertical address increment
                if (++row > rowEnd) { row = rowStart; if (++col > colEnd) col = colStart; }
            } else {
                if (++col > colEnd) { col = colStart; if (++row > rowEnd) row = rowStart; }
            }
        }
    }
};
