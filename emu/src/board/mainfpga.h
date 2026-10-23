// Main FPGA on CS5 (0x14000000, firmware uses the cache-through mirror 0x34000000).
// Register map recovered from RIO2_PROG.BIN:
//   +0x0002 RW  clock source select: bits 5:3 source code (0=internal(bit0 set), 1,2,3 -> 0x00/0x08/0x10,
//               4..7 -> 0x18, 8=Dante -> 0x20), bits 2:1 mode
//   +0x0004 R   status (bit0; bit15 = DNT_MUTE input)
//   +0x0006 R   lock status: bit1..3 = inputs 1..3 locked, bit5 = Dante (source 8) locked
//   +0x000A R   period counter of the selected clock (10 bits)
//   +0x000C/0E/10/12/14 R  period counters of inputs 1,2,3,(4),8 : 24.576 MHz / fs  (48k -> 0x200)
//   +0x0016 RW  output control bits 0,1,8,9 ; +0x0018 bit0 ; +0x001A bit0
//   +0x0800 / +0x0810  UART channels 0/1 = DNT_UART_A (Brooklyn "dac") / DNT_UART_B (conmon HA remote):
//               +0 control (bit1 TX enable, written by the host), +2 TX data (low byte), +4 RX data (low byte),
//               +6 status: bits 3:0 TX FIFO fill (8 deep), bits 12:8 RX FIFO fill (max 31)
//   +0x1000 OLED byte port: +0 ctrl/status (bit0 busy, bits 5:4 mode), +2 data (low byte), +6 FIFO counts (low 7 bits TX)
//   +0x1800 R   interrupt status, +0x1802 RW interrupt enable: 0x10/0x40 UART0/1 TX empty, 0x100,0x200 / 0x400,0x800
//               UART0/1 RX data, 0x0001 and 0x1000.. OLED port.  (status & enable) drives IRQ1 via PLD +4 bit 4.
#pragma once
#include "bus.h"
#include "ssd1322.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include "platform.h"

class MainFpga : public MmioDevice {
public:
    const char* name() const override { return "CS5"; }

    struct ClockInput { bool present = false; int fs = 48000; };
    ClockInput src[9];                     // 0 internal, 1..3 word clock / AES inputs, 8 Dante module
    uint32_t refHz = 24576000;             // counter reference (gives the firmware's expected 0x200 at 48 kHz)
    uint16_t clkSel = 0, reg04 = 0, ctrl16 = 0, ctrl18 = 0, ctrl1a = 0;
    uint16_t oledCtrl = 0, oledDataHi = 0;
    std::function<bool()> oledDc;          // data(true)/command(false) line, PLD +6 bit 7
    std::function<uint8_t()> oledSel;      // chip selects from the PLD: bit0 = panel 0 (+6 bit8 low), bit1 = panel 1 (+6 bit9 low)
    // The Rio3224-D2 has two SSD1322 OLED panels side by side driven through the same byte port; the UI canvas is
    // 589 pixels wide: columns 0..295 go to panel 1 (left, channels 1-16), 296..588 to panel 0 (right, 17-32).
    Ssd1322 oled[2];
    bool log = false, logUart = false;
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000);

    // ---- Dante module UARTs --------------------------------------------------------------------
    static constexpr uint32_t kRxFifo = 16;   // RX FIFO depth (host ISR copies the fill count into a 16-byte buffer)
    struct Uart {
        std::deque<uint8_t> tx, rx;        // tx: towards the module, rx: from the module to the host
        uint16_t ctrl = 0;
        int listenFd = -1, fd = -1;
        int port = 0;
        uint64_t txBytes = 0, rxBytes = 0, rxRead = 0;   // rxBytes: pushed into the RX FIFO, rxRead: popped by the host
    } uart[2];
    std::function<void(int ch, uint8_t b)> onHostTx;   // if set, host->module bytes go to a model instead of the socket
    std::function<void(uint8_t b)> onHostTxB;          // additionally observes host->module bytes on UART B (HA remote frames) when a model handles UART A
    uint16_t irqEnable = 0;
    uint16_t irqLatched = 0;               // event bits (TX FIFO became empty, OLED FIFO empty), cleared when +0x1800 is read
    bool irqActive = false;
    std::function<void(bool)> setIrq;      // -> INTC IRQ1 (vector 65) and PLD +4 bit 4

    // The Dante module's word clock (source 8) only appears once the module has booted; the host
    // firmware's DSP task relies on seeing the DIO lock sequence as separate events after its own init.
    std::vector<int> danteClockToggleMs = { 3000 };   // times (ms) at which the Dante clock toggles present/absent, starting absent
    double cyclesPerMs = 200e3;            // Iφ = 200 MHz
    uint64_t elapsedCycles = 0, pollAcc = 0;
    size_t toggleIdx = 0;

    // Dante module status lines seen on PLD +4: GPI0 (bit0) toggles about once a second when the module's
    // Dante core runs, AUXB (bit3) toggles at 10 Hz in normal mode / 20 Hz in safe mode (the host counts
    // transitions per 2 s window: 2 -> alive, 19..21 -> normal, 38..42 -> safe mode).  They follow the clock.
    bool gpi0 = false, gpi1 = false, auxb = false;
    int gpi0PeriodMs = 500, gpi1PeriodMs = 500, auxbPeriodMs = 50;   // flip intervals: the host counts FALLING edges per 2 s window (2 -> alive, 19..21 -> normal)
    bool moduleRunning() const { return src[8].present; }
    bool dntMute = true;                   // Dante module mute request line (released once the module runs with a stable clock)
    uint16_t gpiBits() const { return moduleRunning() ? ((gpi0 ? 1 : 0) | (gpi1 ? 2 : 0) | (auxb ? 8 : 0)) : 0; }

    MainFpga() { src[8].present = false; src[8].fs = 48000; }
    ~MainFpga() { for (auto& u : uart) { if (u.fd >= 0) sockClose(u.fd); if (u.listenFd >= 0) sockClose(u.listenFd); } }

    // open a TCP listener per UART; a client (e.g. QEMU -serial tcp:127.0.0.1:PORT) carries the bytes
    void openUartPorts(int portA, int portB) {
        int ports[2] = { portA, portB };
        for (int i = 0; i < 2; i++) {
            uart[i].port = ports[i];
            if (ports[i] <= 0) continue;
            int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
            int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
            sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = htons(ports[i]);
            if (bind(fd, (sockaddr*)&sa, sizeof sa) < 0 || listen(fd, 1) < 0) { fprintf(stderr, "[CS5] DNT UART %c: cannot listen on port %d (%s)\n", 'A' + i, ports[i], strerror(errno)); close(fd); continue; }
            sockNonBlocking(fd);
            uart[i].listenFd = fd;
            fprintf(stderr, "[CS5] DNT UART %c listening on 127.0.0.1:%d\n", 'A' + i, ports[i]);
        }
    }

    void tick(uint64_t cycles) override {
        elapsedCycles += cycles;
        while (toggleIdx < danteClockToggleMs.size() && elapsedCycles >= (uint64_t)(danteClockToggleMs[toggleIdx] * cyclesPerMs)) {
            src[8].present = !src[8].present;
            fprintf(stderr, "[CS5] Dante module word clock %s (%d Hz) at %d ms\n", src[8].present ? "present" : "lost", src[8].fs, danteClockToggleMs[toggleIdx]);
            toggleIdx++;
        }
        pollAcc += cycles;
        if (pollAcc >= (uint64_t)cyclesPerMs) { pollAcc = 0; pollUarts(); }
        pumpPaced(cycles);
        uint64_t ms = (uint64_t)(elapsedCycles / cyclesPerMs);
        gpi0 = (ms / gpi0PeriodMs) & 1; gpi1 = (ms / gpi1PeriodMs) & 1; auxb = (ms / auxbPeriodMs) & 1;
    }

    uint32_t read(uint32_t addr, int size) override {
        uint32_t o = addr & 0x03FFFFFF, v;
        switch (o) {
        case 0x0002: v = clkSel; break;
        case 0x0004: v = (reg04 & 0x7FFF) | (dntMute ? 0x8000 : 0); break;   // bit 15 = DNT_MUTE from the module (1 = module requests mute)
        case 0x0006: v = lockBits(); break;
        case 0x000A: v = counter(selectedSource()); break;
        case 0x000C: v = counter(1); break;
        case 0x000E: v = counter(2); break;
        case 0x0010: v = counter(3); break;
        case 0x0012: v = counter(4); break;
        case 0x0014: v = counter(8); break;
        case 0x0016: v = ctrl16; break;
        case 0x0018: v = ctrl18; break;
        case 0x001A: v = ctrl1a; break;
        case 0x0800: case 0x0810: v = uart[(o >> 4) & 1].ctrl; break;
        case 0x0802: case 0x0812: v = 0; break;
        case 0x0804: case 0x0814: { auto& u = uart[(o >> 4) & 1]; v = 0; if (!u.rx.empty()) { v = u.rx.front(); u.rx.pop_front(); u.rxRead++; if (logUart) fprintf(stderr, "[DNT%c] <= %02X\n", 'A' + (int)((o >> 4) & 1), (unsigned)v); } else if (logUart) fprintf(stderr, "[DNT%c] <= (empty)\n", 'A' + (int)((o >> 4) & 1)); updateIrq(); break; }
        case 0x0806: case 0x0816: { auto& u = uart[(o >> 4) & 1]; uint32_t rxn = u.rx.size() > kRxFifo ? kRxFifo : (uint32_t)u.rx.size(); v = (rxn << 8) | (u.tx.size() & 0xF); break; }
        case 0x1000: v = oledCtrl & ~1u; break;                 // never busy
        case 0x1002: v = oledDataHi; break;
        case 0x1006: v = 0; break;                              // FIFO empty
        case 0x1800: v = irqStatus(); irqLatched = 0; updateIrq(); break;      // read clears the event bits
        case 0x1802: v = irqEnable; break;
        default: v = 0; for (int i = 0; i < size; i++) v = (v << 8) | at(o + i); break;
        }
        if (size == 1) v = (o & 1) ? (v & 0xFF) : ((v >> 8) & 0xFF);
        if (log) fprintf(stderr, "[CS5] rd %08X -> %0*X\n", addr, size * 2, v);
        return v;
    }
    void write(uint32_t addr, int size, uint32_t val) override {
        uint32_t o = addr & 0x03FFFFFF;
        if (log) fprintf(stderr, "[CS5] wr %08X <- %0*X\n", addr, size * 2, val);
        switch (o) {
        case 0x0002: clkSel = val; break;
        case 0x0016: ctrl16 = val; break;
        case 0x0018: ctrl18 = val; break;
        case 0x001A: ctrl1a = val; break;
        case 0x0800: case 0x0810: uart[(o >> 4) & 1].ctrl = val; break;
        case 0x0802: case 0x0812: { int ch = (o >> 4) & 1; auto& u = uart[ch]; u.txBytes++;
                                    if (logUart) fprintf(stderr, "[DNT%c] -> %02X\n", 'A' + ch, val & 0xFF);
                                    if (onHostTx && u.fd < 0 && u.listenFd < 0) onHostTx(ch, val & 0xFF);
                                    if (ch == 1 && onHostTxB && u.fd < 0 && u.listenFd < 0) onHostTxB(val & 0xFF);
                                    else { u.tx.push_back(val & 0xFF); flushTx(ch); }
                                    irqLatched |= ch ? 0x40 : 0x10; updateIrq(); break; }
        case 0x1000: oledCtrl = val; break;
        case 0x1002: { oledDataHi = val & 0xFF00; bool dc = oledDc ? oledDc() : true; uint8_t sel = oledSel ? oledSel() : 3;
                       if (sel & 1) oled[0].byte(val & 0xFF, dc); if (sel & 2) oled[1].byte(val & 0xFF, dc);
                       irqLatched |= 0x1000; updateIrq(); break; }
        case 0x1802: irqEnable = val; updateIrq(); break;
        default: for (int i = size - 1; i >= 0; i--) { at(o + i) = val & 0xFF; val >>= 8; } break;
        }
    }

    // bytes from the module side (socket poll or the module model); delivered to the host at UART pace
    // (115200 baud = one byte per ~87 us) so the firmware sees realistic timing
    std::deque<uint8_t> pacedRx[2];
    double baudBytesPerMs = 11.52;
    void moduleToHost(int ch, const uint8_t* p, size_t n) { auto& q = pacedRx[ch & 1]; for (size_t i = 0; i < n; i++) q.push_back(p[i]); }
    void pumpPaced(uint64_t cycles) {
        static double credit[2] = { 0, 0 };
        for (int ch = 0; ch < 2; ch++) {
            auto& q = pacedRx[ch]; if (q.empty()) { credit[ch] = 0; continue; }
            credit[ch] += cycles / cyclesPerMs * baudBytesPerMs;
            auto& u = uart[ch];
            while (credit[ch] >= 1.0 && !q.empty() && u.rx.size() < kRxFifo) { u.rx.push_back(q.front()); q.pop_front(); u.rxBytes++; credit[ch] -= 1.0; }
            if (credit[ch] > 64) credit[ch] = 64;
        }
        updateIrq();
    }

    uint16_t irqStatus() const {
        uint16_t s = irqLatched;                                // TX-empty / OLED events
        if (!uart[0].rx.empty()) s |= 0x300;                    // RX data pending (level)
        if (!uart[1].rx.empty()) s |= 0xC00;
        return s;
    }
    int selectedSource() const {
        if (clkSel & 1) return 0;
        switch ((clkSel >> 3) & 7) { case 1: return 1; case 2: return 2; case 3: return 3; case 4: return 8; default: return 4; }
    }
    uint16_t lockBits() const {
        uint16_t v = 0;
        for (int s = 1; s <= 3; s++) if (src[s].present) v |= 1 << s;
        if (src[8].present) v |= 0x20;
        return v;
    }
    uint16_t counter(int s) const {
        if (s < 0 || s > 8 || !src[s].present || src[s].fs <= 0) return 0;
        return (uint16_t)((refHz / (uint32_t)src[s].fs) & 0x3FF);
    }
private:
    uint8_t& at(uint32_t o) { static uint8_t dummy; return o < mem.size() ? mem[o] : dummy; }
    void updateIrq() {
        bool a = (irqStatus() & irqEnable) != 0;
        if (a != irqActive) { irqActive = a; if (setIrq) setIrq(a); }
    }
    void flushTx(int ch) {
        auto& u = uart[ch];
        if (u.fd >= 0) {
            while (!u.tx.empty()) {
                uint8_t b = u.tx.front();
                ssize_t r = send(u.fd, (const char*)&b, 1, MSG_NOSIGNAL);
                if (r == 1) u.tx.pop_front();
                else if (r < 0 && sockWouldBlock()) break;
                else { sockClose(u.fd); u.fd = -1; fprintf(stderr, "[CS5] DNT UART %c: client disconnected\n", 'A' + ch); break; }
            }
        } else u.tx.clear();                                    // nobody listening: bytes vanish like on an unconnected module
    }
    void pollUarts() {
        for (int i = 0; i < 2; i++) {
            auto& u = uart[i];
            if (u.listenFd >= 0 && u.fd < 0) {
                int c = (int)accept(u.listenFd, nullptr, nullptr);
                if (c >= 0) { sockNonBlocking(c); int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one); u.fd = c; fprintf(stderr, "[CS5] DNT UART %c: client connected\n", 'A' + i); }
            }
            if (u.fd >= 0) {
                if (!u.tx.empty()) flushTx(i);
                uint8_t buf[256];
                ssize_t r = recv(u.fd, (char*)buf, sizeof buf, 0);
                if (r > 0) { if (logUart) for (ssize_t k = 0; k < r; k++) fprintf(stderr, "[DNT%c] <- %02X\n", 'A' + i, buf[k]); moduleToHost(i, buf, r); }
                else if (r == 0) { sockClose(u.fd); u.fd = -1; fprintf(stderr, "[CS5] DNT UART %c: client closed\n", 'A' + i); }
            }
        }
        updateIrq();
    }
};
