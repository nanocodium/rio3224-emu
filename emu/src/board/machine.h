// The Rio3224-D2 main board: SH7203 + NOR flash (CS0) + FPGA window (CS1) + SDRAM (CS3) + peripherals.
#pragma once
#include "bus.h"
#include "sh2a/cpu.h"
#include "soc/intc.h"
#include "soc/cmt.h"
#include "soc/scif.h"
#include "soc/regblock.h"
#include "soc/mtu2.h"
#include "soc/adc.h"
#include "board/pld.h"
#include "board/flash.h"
#include "board/mainfpga.h"
#include "board/dspif.h"
#include "board/halink.h"
#include "board/dantemodule.h"
#include "board/dm9000.h"
#include <memory>
#include <map>
#include <string>
#include <vector>
#include "audio_glue.h"
#include "panel_server.h"

struct Machine {
    Bus bus;
    Sh2a cpu{bus};
    std::unique_ptr<Intc> intc;
    std::unique_ptr<Cmt> cmt;
    std::unique_ptr<Mtu2> mtu2;
    std::unique_ptr<Adc> adc;
    std::unique_ptr<Pld> pld;
    std::unique_ptr<NorFlash> flash;
    std::string flashPath;
    std::unique_ptr<MainFpga> mainFpga;      // CS5: clock manager, OLED port, channel control
    std::unique_ptr<DspIf> dspIf;            // CS4: DSP register transport + converter serial master
    HaLink haLink;                           // SCIF1: head-amp FPGA command link
    std::unique_ptr<AudioGlue> audio;        // the PCM engine (head amps, DSP, Dante TX/RX, outputs) fed by the firmware's writes
    std::unique_ptr<PanelServer> panelHttp;  // --panel-http PORT: live front/rear panel page
    bool realtimeFlag = false;               // shown on the panel page
    std::string oledPgmBytes() const;        // both OLED panels as a PGM image (used by --oled and the panel page)
    bool startPanelHttp(uint16_t port);
    void panelAction(const std::string& path, const std::map<std::string, std::string>& q);
    std::string panelStateJson();
    uint64_t danteRunningSinceMs = 0;        // when the module model last came up (DNT_MUTE release timing)
    int dntMuteReleaseMs = 1500;             // module releases its mute line this long after it is running
    std::unique_ptr<DanteModule> dante;      // behavioural Dante module on DNT_UART_A (unless bridged to TCP)
    std::unique_ptr<Dm9000> enet;            // CS6: DM9000 Ethernet controller (IRQ0)
    std::string pcapPath, tapName;           // network backends: pcap dump, Linux TAP device
    FILE* pcap = nullptr; int tapFd = -1; uint64_t netPollAcc = 0;
    void openNetwork();
    void pollNetwork();
    void attachDanteModel();
    std::string oledDumpPath;                // write the OLED panel image (PGM) here at shutdown
    struct OledAt { uint64_t ms; std::string path; bool done = false; };
    std::vector<OledAt> oledAt;              // --oled-at MS=FILE: timed captures of the two panels (same PGM format)
    void writeOled(const std::string& path);
    std::string oledFullPath;                // write the whole SSD1322 RAM (480x128) here at shutdown
    bool oledAscii = false;                  // print the OLED panel as text at shutdown
    bool panelAscii = false;                 // print the front-panel LED state at shutdown
    double fanHz = 100.0;                    // fan running when > 0 (PLD +4 bit7 alarm input low); 0 = stopped fan
    struct ConsoleLine { uint64_t ms; std::string text; };
    std::vector<ConsoleLine> consoleScript;   // --console-script: lines typed into the SCIF0 debug monitor at the given times
    size_t consolePos = 0; std::string consolePending; uint64_t consoleNextByteMs = 0;
    struct Rotate { int steps; uint64_t fromMs; int msPerStep; bool gray; };
    std::vector<Rotate> rotations;           // scheduled rotary-encoder movements (--rotate STEPS@T[,x,MSPERSTEP]): quadrature transitions
    int rotaryCode = 0;                      // current encoder position (transitions)
    struct Press { int sw; uint64_t fromMs, toMs; };
    std::vector<Press> presses;              // scheduled front-panel switch presses (--press)
    struct PldEvent { uint64_t ms; int kind; bool on; bool done = false; };   // kind 0 = /UVLO (power going away), 1 = PSU A fail, 2 = PSU B fail, 3 = +48V MASTER
    std::vector<PldEvent> pldEvents;          // --power-off MS, --psu-fail A|B@MS[-MS], --48v-master [on|off@MS]
    struct Inject { uint64_t ms; int ch; std::vector<uint8_t> bytes; bool done = false; };
    std::vector<Inject> injects;              // --dntb-send MS:HEX: bytes arriving from the Dante module on UART B (console HA remote) at MS
    std::vector<std::pair<uint64_t, bool>> haRemoteEvents;   // --dante-haremote MS[,MS_OFF]: module reports a console connected (/ disconnected) on the HA remote channel
    uint64_t powerHoldMs = 8000;              // time from /UVLO to the rails dropping (--power-off MS,HOLD); generous so the firmware's save (31 ms, or 5.4 s after a panel edit) completes
    uint64_t powerLossMs = 0; bool powerLost = false;   // set when the hold-up time has elapsed (main loop stops like a real power-down)
    uint8_t mac[6] = { 0x00, 0xA0, 0xDE, 0x12, 0x34, 0x56 };   // host MAC written into the board-info flash record when blank
    std::string serial = "Z1234567", label = "";              // board-info records 1 (17 bytes) and 2 (33 bytes)
    void initBoardInfo();
    std::unique_ptr<Scif> scif[4];
    std::vector<std::unique_ptr<RegBlock>> blocks;
    RegBlock* fpga = nullptr;     // CS1 window
    RegBlock* pfc = nullptr;      // port function controller / GPIO
    RegBlock* bsc = nullptr;
    RegBlock* cpg = nullptr;

    static constexpr uint32_t kLoadAddr = 0x0C0F1000;
    static constexpr uint32_t kSdramBase = 0x0C000000, kSdramSize = 64u << 20;
    static constexpr uint32_t kFlashBase = 0x00000000, kFlashSize = 16u << 20;   // 2x S29GL064-class x16 chips, 64 KB sectors each -> 128 KB combined (FMD asserts otherwise)
    static constexpr uint32_t kRamBase = 0xFFF80000, kRamSize = 128u << 10;

    Machine();
    bool loadFirmware(const std::string& path);
    void tick(uint64_t cycles);
    void enableLog(const std::string& dev);
    void shutdown();
    RegBlock* addBlock(const std::string& name, uint32_t base, uint32_t size);
};
