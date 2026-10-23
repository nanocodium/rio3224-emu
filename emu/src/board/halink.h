// Head-amp control link: SCIF1 carries 2-byte frames from the host to the four "AK" (MAX 10) head-amp FPGAs.
//   byte 0 = 0xC0 | fpga<<3 | command   (fpga 0..3, command 0..7)      -- FUN_0c173440
//   byte 1 = value (< 0xC0)
// Commands come from the HA task tables (phantom / gain / ...).  The FPGAs do not need to answer.
#pragma once
#include <cstdio>
#include <cstdint>
#include <functional>

class HaLink {
public:
    bool log = false;
    uint8_t reg[4][8] = {};             // last value per FPGA and command
    bool seen[4] = {};                  // a frame was sent to this FPGA (sheet present as far as the firmware is concerned)
    uint64_t frames = 0, strays = 0;
    std::function<void(int fpga, int cmd, uint8_t val)> onCommand;

    void rxByteFromHost(uint8_t b) {
        if (b >= 0xC0) { hdr = b; haveHdr = true; return; }
        if (!haveHdr) { strays++; if (log) fprintf(stderr, "[HA] stray byte %02X\n", b); return; }
        int fpga = (hdr >> 3) & 3, cmd = hdr & 7;
        reg[fpga][cmd] = b; frames++; haveHdr = false; seen[fpga] = true;
        if (log) fprintf(stderr, "[HA] fpga %d cmd %d <- %02X\n", fpga, cmd, b);
        if (onCommand) onCommand(fpga, cmd, b);
    }
private:
    uint8_t hdr = 0; bool haveHdr = false;
};
