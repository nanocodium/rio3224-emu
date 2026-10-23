// SH7203 SCIF (serial with FIFO), channels at 0xFFFE8000 + n*0x800
#pragma once
#include "bus.h"
#include "intc.h"
#include <deque>
#include <functional>
#include <mutex>

class Scif : public MmioDevice {
public:
    Scif(int channel, Intc& intc) : chan(channel), intc(intc) {}
    const char* name() const override { return nm; }
    uint32_t read(uint32_t addr, int size) override;
    void write(uint32_t addr, int size, uint32_t val) override;
    void tick(uint64_t cpuCycles) override;

    std::function<void(uint8_t)> onTx;        // byte transmitted
    void rxByte(uint8_t b);                   // byte arrives from the outside (thread-safe)
    bool logAccess = false;
    int chan;
    char nm[8] = "SCIFx";
    uint32_t baud = 0;
private:
    Intc& intc;
    uint16_t scsmr = 0, scscr = 0, scfsr = 0x0060, scfcr = 0, scsptr = 0, sclsr = 0, scemr = 0;
    uint8_t scbrr = 0xFF;
    std::deque<uint8_t> rxFifo;      // bytes visible to the CPU
    std::deque<uint8_t> rxPending;   // bytes from outside not yet "received" (paced)
    std::mutex mu;
    uint64_t cycAcc = 0;
    void updateIrq();
    int vecBase() const { return 240 + chan * 4; }   // BRI, ERI, RXI, TXI
};
