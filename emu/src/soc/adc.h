// A/D converter as used by the Rio firmware: two units at 0xFFFE5800 (ADDRA-D 5800..5806, ADCSR 5820)
// and 0xFFFE5810 (ADDRE-H 5810..5816, ADCSR2 5822). ADCSR layout per SH7203: ADF(15) ADIE(14) ADST(13)
// TRGS(11:8) CKS(7:6) MDS(5:3) CH(2:0). Conversions complete instantly; 10-bit results left-justified.
#pragma once
#include "bus.h"
#include "intc.h"
#include <cstdio>

class Adc : public MmioDevice {
public:
    explicit Adc(Intc& intc) : intc(intc) {}
    const char* name() const override { return "ADC"; }
    uint32_t read(uint32_t addr, int size) override;
    void write(uint32_t addr, int size, uint32_t val) override;
    // analog inputs 0..7 (unit0: 0-3, unit1: 4-7), 10-bit values
    uint16_t input[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    bool log = false;
private:
    struct Unit { uint16_t adcsr = 0; uint16_t addr[4] = { 0, 0, 0, 0 }; } u[2];
    Intc& intc;
    void start(int unit);
};
