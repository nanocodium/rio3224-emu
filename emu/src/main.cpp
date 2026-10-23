// rio-emu: Yamaha Rio3224-D2 host MCU emulator — boots RIO2_PROG.BIN
#include "bus.h"
#include "sh2a/cpu.h"
#include "board/machine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include "platform.h"
#ifdef _WIN32
#include <timeapi.h>
#endif
#ifndef _WIN32
#include <termios.h>
#endif
#include <signal.h>

static std::atomic<bool> g_quit{false};
static void onSig(int) { g_quit = true; }

static void usage() {
    fprintf(stderr, "usage: rio-emu [--fw file] [--max insns] [--trace] [--trace-from N] [--trace-file f] [--log DEV]\n"
                    "               [--no-unmapped] [--no-raw] [--pc-hist] [--debug-exc] [--disasm ADDR[,COUNT]]\n");
}

int main(int argc, char** argv) {
    std::string fw = "../fw/d2/RIO2_PROG.BIN", traceFile, disasmAt, dumpAt, flashPath = "../fw/d2/flash.bin", oledPath;
    bool oledAscii = false, panelAscii = false; std::string oledFullPath; std::vector<Machine::OledAt> oledAt; int danteFs = 48000;      // --dante-fs 0: no clock from the Dante module
    double fanHz = 100.0; int aesChip = 1; std::string pcapPath, tapName; std::vector<Machine::Press> presses; uint8_t macAddr[6] = {}; bool haveMac = false;
    unsigned pldIn = 0xFFFF, pldIn10 = 0; int dntPortA = 0, dntPortB = 0; bool danteModel = true; int danteBootMs = 12000; int browseC = -1, browseF = 7, browseG = 1; int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath, ramDumpPath; std::vector<Machine::Rotate> rotations;
    std::vector<Machine::PldEvent> pldEvents; unsigned long long powerHoldMs = 8000;
    std::string audioDevIn, audioDevOut; double audioDevLatency = 30.0;
    std::vector<Machine::Inject> injects; std::vector<std::pair<uint64_t, bool>> haRemoteEvents;
    uint16_t panelHttpPort = 0;
    bool realtime = false; bool audioOn = false, audioLoop = false; std::string audioIn, audioOut, aesOut, txOut, dantePatch, audioMap; std::vector<std::string> tones;
    std::string danteName; unsigned long long runMs = 0;
    std::string danteNet, danteNet2, danteCtl; bool danteLeader = true; std::vector<AudioGlue::NetFail> danteFails;
    double danteLatencyMs = 1.0, danteDrop = 0.0, hostInDbu = 24.0, hostOutDbu = 24.0; int traceInput = 0, traceMs = 500, meterPrintMs = 0; int danteRespMs = -1, danteDeferMs = -1;
    std::vector<int> danteClockMs = { 3000 };         // the module's word clock appears this long after power-up (real module: tens of seconds)
    uint64_t maxInsns = 0, traceFrom = 0;
    bool trace = false, logUnmapped = true, rawTty = true, pcHist = false, debugExc = false;
    int traceOnIrq = 0, ringDump = 0, tracePcLen = 0; unsigned tracePc = 0, watchLo = 0, watchLen = 0; int watchMax = 4;
    std::vector<Sh2a::Hook> hooks;
    std::vector<Sh2a::Probe> probes; unsigned long long probeAfter = 0; int probeMax = 60; unsigned idlePc = 0x0C35FE14;
    uint32_t stopAt = 0; bool haveStop = false;
    std::vector<std::string> logDevs;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--fw" && i + 1 < argc) fw = argv[++i];
        else if (a == "--max" && i + 1 < argc) maxInsns = strtoull(argv[++i], 0, 0);
        else if (a == "--trace") trace = true;
        else if (a == "--trace-from" && i + 1 < argc) { trace = true; traceFrom = strtoull(argv[++i], 0, 0); }
        else if (a == "--trace-file" && i + 1 < argc) traceFile = argv[++i];
        else if (a == "--no-unmapped") logUnmapped = false;
        else if (a == "--no-raw") rawTty = false;
        else if (a == "--pc-hist") pcHist = true;
        else if (a == "--debug-exc") debugExc = true;
        else if (a == "--trace-irq" && i + 1 < argc) traceOnIrq = atoi(argv[++i]);
        else if (a == "--trace-pc" && i + 1 < argc) { unsigned pc, n; if (sscanf(argv[++i], "%x,%u", &pc, &n) == 2) { tracePc = pc; tracePcLen = n; } }
        else if (a == "--dump" && i + 1 < argc) dumpAt = argv[++i];
        else if (a == "--watch-max" && i + 1 < argc) watchMax = atoi(argv[++i]);
        else if (a == "--watch" && i + 1 < argc) { unsigned lo, len; if (sscanf(argv[++i], "%x,%x", &lo, &len) == 2) { watchLo = lo; watchLen = len; } }
        else if (a == "--flash" && i + 1 < argc) flashPath = argv[++i];
        else if (a == "--probe" && i + 1 < argc) {          // PC[:rN,LEN]  (LEN hex) -> registers, plus memory at R[N]
            Sh2a::Probe p; unsigned pc, reg, len; const char* s = argv[++i];
            if (sscanf(s, "%x:r%u,%x", &pc, &reg, &len) == 3) { p.pc = pc; p.memReg = (int)reg; p.memLen = len; }
            else p.pc = strtoul(s, 0, 16);
            probes.push_back(p);
        }
        else if (a == "--oled" && i + 1 < argc) oledPath = argv[++i];
        else if (a == "--oled-ascii") oledAscii = true;
        else if (a == "--probe-after" && i + 1 < argc) probeAfter = strtoull(argv[++i], 0, 10);
        else if (a == "--idle-pc" && i + 1 < argc) idlePc = strtoul(argv[++i], 0, 16) & 0x1FFFFFFF;   // 0 = no idle-loop fast forward
        else if (a == "--no-idle-skip") idlePc = 0;
        else if (a == "--probe-max" && i + 1 < argc) probeMax = atoi(argv[++i]);
        else if (a == "--oled-full" && i + 1 < argc) oledFullPath = argv[++i];
        else if (a == "--oled-at" && i + 1 < argc) { unsigned long long t = 0; const char* s = argv[++i]; char* e = nullptr; t = strtoull(s, &e, 10); if (e && *e == '=') oledAt.push_back({ t, std::string(e + 1) }); }   // --oled-at MS=FILE
        else if (a == "--panel-http" && i + 1 < argc) panelHttpPort = (uint16_t)atoi(argv[++i]);                                   // live panel page at http://127.0.0.1:PORT/
        else if (a == "--dntb-send" && i + 1 < argc) { const char* v = argv[++i]; const char* c = strchr(v, ':'); if (c) { Machine::Inject inj; inj.ms = strtoull(v, 0, 10); inj.ch = 1; for (const char* p = c + 1; p[0] && p[1]; p += 2) { unsigned b; if (sscanf(p, "%2x", &b) == 1) inj.bytes.push_back((uint8_t)b); } injects.push_back(inj); } }   // --dntb-send MS:HEX (console -> unit frame on UART B)
        else if (a == "--dante-haremote" && i + 1 < argc) { char* e = nullptr; unsigned long long t = strtoull(argv[++i], &e, 10); haRemoteEvents.push_back({ t, true }); if (e && *e == ',') haRemoteEvents.push_back({ strtoull(e + 1, 0, 10), false }); }   // --dante-haremote MS[,MS_OFF]
        else if (a == "--panel") panelAscii = true;
        else if (a == "--dnt-uart" && i + 1 < argc) { if (sscanf(argv[++i], "%d,%d", &dntPortA, &dntPortB) < 1) { usage(); return 1; } }   // TCP ports for DNT_UART_A,B
        else if (a == "--no-dante-model") danteModel = false;
        else if (a == "--dante-browse" && i + 1 < argc) { unsigned c, f, g; if (sscanf(argv[++i], "%x,%x,%x", &c, &f, &g) == 3) { browseC = c; browseF = f; browseG = g; } }
        else if (a == "--dante-boot-ms" && i + 1 < argc) danteBootMs = atoi(argv[++i]);
        else if (a == "--dante-swmode" && i + 1 < argc) danteSwMode = atoi(argv[++i]);
        else if (a == "--dante-fresh") danteFresh = true;
        else if (a == "--audio-in" && i + 1 < argc) { std::string v = argv[++i]; size_t c = v.find(",loop"); audioIn = v.substr(0, c); audioLoop = c != std::string::npos; audioOn = true; }
        else if (a == "--audio-out" && i + 1 < argc) { audioOut = argv[++i]; audioOn = true; }
        else if (a == "--aes-out" && i + 1 < argc) { aesOut = argv[++i]; audioOn = true; }
        else if (a == "--tx-out" && i + 1 < argc) { txOut = argv[++i]; audioOn = true; }
        else if (a == "--tone" && i + 1 < argc) { tones.push_back(argv[++i]); audioOn = true; }
        else if (a == "--audio-dev" && i + 1 < argc) { std::string v = argv[++i]; audioOn = true; realtime = true; size_t p = 0; while (p < v.size()) { size_t c = v.find(',', p); std::string kv = v.substr(p, c == std::string::npos ? std::string::npos : c - p); size_t e = kv.find('='); std::string k = kv.substr(0, e), val = e == std::string::npos ? "" : kv.substr(e + 1); if (k == "in") audioDevIn = val.empty() ? "default" : val; else if (k == "out") audioDevOut = val.empty() ? "default" : val; else if (k == "latency") audioDevLatency = atof(val.c_str()); p = c == std::string::npos ? v.size() : c + 1; } }   // --audio-dev in=SPEC,out=SPEC[,latency=MS] (implies --realtime)
        else if (a == "--audio-list") { std::string err; auto devs = rio::wasapiListDevices(&err); if (!err.empty()) fprintf(stderr, "%s\n", err.c_str()); for (auto& d : devs) printf("%-7s %s%s  (%d ch @ %u Hz)\n", d.capture ? "input" : "output", d.name.c_str(), d.isDefault ? " [default]" : "", d.channels, d.rate); return 0; }
        else if (a == "--dante-patch" && i + 1 < argc) { dantePatch = argv[++i]; audioOn = true; }
        else if (a == "--dante-latency-ms" && i + 1 < argc) danteLatencyMs = atof(argv[++i]);
        else if (a == "--dante-drop" && i + 1 < argc) danteDrop = atof(argv[++i]);
        else if (a == "--dante-net") { danteNet = "on"; audioOn = true; if (i + 1 < argc && argv[i + 1][0] != '-') danteNet = argv[++i]; }           // --dante-net [GROUP:PORT[@IFADDR]]: UDP virtual Dante network (primary)
        else if (a == "--dante-net2") { danteNet2 = "on"; if (i + 1 < argc && argv[i + 1][0] != '-') danteNet2 = argv[++i]; }                      // --dante-net2 [GROUP:PORT[@IFADDR]]: secondary network (redundant mode)
        else if (a == "--dante-clock" && i + 1 < argc) danteLeader = strcmp(argv[++i], "follower") != 0;
        else if (a == "--dante-name" && i + 1 < argc) danteName = argv[++i];
        else if (a == "--dante-ctl" && i + 1 < argc) { danteCtl = argv[++i]; audioOn = true; realtime = true; }                                      // --dante-ctl IFADDR[;sec=IF2;trace=FILE;latency=MS;prefer=1;ports=A,C,I,F;persist=FILE;verbose=1]: Controller-interoperable endpoint on the real network (implies --realtime)
        else if (a == "--run-ms" && i + 1 < argc) runMs = strtoull(argv[++i], 0, 10);                                                 // stop after this much emulated time
        else if (a == "--dante-fail" && i + 1 < argc) { const char* v = argv[++i]; bool sec = strncmp(v, "sec", 3) == 0; const char* at = strchr(v, '@'); unsigned long long t0 = at ? strtoull(at + 1, 0, 10) : 0, t1 = 0; if (at) { const char* d = strchr(at, '-'); if (d) t1 = strtoull(d + 1, 0, 10); } danteFails.push_back({ t0, sec, true }); if (t1) danteFails.push_back({ t1, sec, false }); }   // --dante-fail primary|secondary@MS[-MS2]
        else if (a == "--host-in-dbu" && i + 1 < argc) hostInDbu = atof(argv[++i]);
        else if (a == "--host-out-dbu" && i + 1 < argc) hostOutDbu = atof(argv[++i]);
        else if (a == "--audio-map" && i + 1 < argc) audioMap = argv[++i];
        else if (a == "--trace-input" && i + 1 < argc) { traceInput = atoi(argv[++i]); audioOn = true; }
        else if (a == "--trace-ms" && i + 1 < argc) traceMs = atoi(argv[++i]);
        else if (a == "--rotate" && i + 1 < argc) {                     // STEPS@T[,gray|bin][,MSPERSTEP]: turn the front-panel encoder
            int steps = 0; unsigned t = 0; char mode[8] = "bin"; int per = 60;
            if (sscanf(argv[++i], "%d@%u,%7[a-z],%d", &steps, &t, mode, &per) >= 2 || sscanf(argv[i], "%d@%u", &steps, &t) == 2)
                rotations.push_back({ steps, t, per, std::string(mode) == "gray" });
        }
        else if (a == "--meter-print" && i + 1 < argc) { meterPrintMs = atoi(argv[++i]); audioOn = true; }
        else if (a == "--audio") audioOn = true;
        else if (a == "--realtime") realtime = true;                 // pace emulated time to wall-clock time (1x)
        else if (a == "--power-off" && i + 1 < argc) { char* e = nullptr; unsigned long long t = strtoull(argv[++i], &e, 10); pldEvents.push_back({ t, 0, true }); if (e && *e == ',') powerHoldMs = strtoull(e + 1, 0, 10); }              // /UVLO at MS: the firmware runs its power-off sequence (saves settings)
        else if (a == "--psu-fail" && i + 1 < argc) { const char* v = argv[++i]; int k = (v[0] == 'B' || v[0] == 'b') ? 2 : 1; unsigned long long t0 = 0, t1 = 0; if (v[1] == '@') { char* e = nullptr; t0 = strtoull(v + 2, &e, 10); if (e && *e == '-') t1 = strtoull(e + 1, 0, 10); } pldEvents.push_back({ t0, k, true }); if (t1) pldEvents.push_back({ t1, k, false }); }   // --psu-fail A|B@MS[-MS2]
        else if (a == "--48v-master") { unsigned long long t0 = 0; bool on = true; if (i + 1 < argc && argv[i + 1][0] != '-') { const char* v = argv[++i]; on = strncmp(v, "off", 3) != 0; if (const char* at = strchr(v, '@')) t0 = strtoull(at + 1, 0, 10); } pldEvents.push_back({ t0, 3, on }); }   // --48v-master [on|off@MS]
        else if (a == "--ramdump" && i + 1 < argc) ramDumpPath = argv[++i];   // SDRAM image at exit (parameter-location hunting)
        else if (a == "--console-script" && i + 1 < argc) consoleScriptPath = argv[++i];   // file of "<ms> <text>" lines
        else if (a == "--dante-resp-ms" && i + 1 < argc) { if (sscanf(argv[++i], "%d,%d", &danteRespMs, &danteDeferMs) < 1) danteRespMs = -1; }
        else if (a == "--fan-hz" && i + 1 < argc) fanHz = atof(argv[++i]);                // fan running when > 0
        else if (a == "--aes" && i + 1 < argc) aesChip = atoi(argv[++i]);                   // AES/EBU transmitter: 0 none, 1 AK4101, 2 CS8406
        else if (a == "--pcap" && i + 1 < argc) pcapPath = argv[++i];                       // write Ethernet frames to a pcap file
        else if (a == "--tap" && i + 1 < argc) tapName = argv[++i];                         // Linux TAP device for the DM9000
        else if (a == "--press" && i + 1 < argc) { int sw; unsigned t0, t1; const char* v = argv[++i];                // --press SW@T0[-T1] (ms)
                                                   if (sscanf(v, "%d@%u-%u", &sw, &t0, &t1) == 3) presses.push_back({ sw, t0, t1 });
                                                   else if (sscanf(v, "%d@%u", &sw, &t0) == 2) presses.push_back({ sw, t0, t0 + 300 }); }
        else if (a == "--mac" && i + 1 < argc) { unsigned m[6]; if (sscanf(argv[++i], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) { for (int k = 0; k < 6; k++) macAddr[k] = (uint8_t)m[k]; haveMac = true; } }
        else if (a == "--pld-in" && i + 1 < argc) pldIn = strtoul(argv[++i], 0, 16);       // AND mask on PLD +0xA inputs
        else if (a == "--pld-in10" && i + 1 < argc) pldIn10 = strtoul(argv[++i], 0, 16);   // value of PLD +0x10 inputs
        else if (a == "--dante-fs" && i + 1 < argc) danteFs = atoi(argv[++i]);
        else if (a == "--dante-clock-ms" && i + 1 < argc) {          // T1[,T2,...]: toggle times in ms (present at T1, lost at T2, ...)
            danteClockMs.clear();
            for (char* s = strtok(argv[++i], ","); s; s = strtok(nullptr, ",")) danteClockMs.push_back(atoi(s));
        }
        else if (a == "--hook" && i + 1 < argc) {           // PC[@PR]:rN=VAL (hex)
            unsigned pc, reg, val, pr = 0; const char* h = argv[++i];
            if (sscanf(h, "%x@%x:r%u=%x", &pc, &pr, &reg, &val) == 4 || (sscanf(h, "%x:r%u=%x", &pc, &reg, &val) == 3 && (pr = 0, true))) hooks.push_back({pc, (int)reg, val, pr});
            else { usage(); return 1; }
        }
        else if (a == "--stop-at" && i + 1 < argc) { stopAt = strtoul(argv[++i], 0, 16); haveStop = true; }
        else if (a == "--ring" && i + 1 < argc) ringDump = atoi(argv[++i]);
        else if (a == "--disasm" && i + 1 < argc) disasmAt = argv[++i];
        else if (a == "--log" && i + 1 < argc) logDevs.push_back(argv[++i]);
        else { usage(); return 1; }
    }

    Machine mach;
    mach.flashPath = flashPath;
    if (panelHttpPort) mach.startPanelHttp(panelHttpPort); mach.realtimeFlag = realtime;
    mach.oledDumpPath = oledPath; mach.oledAscii = oledAscii; mach.panelAscii = panelAscii; mach.oledFullPath = oledFullPath; mach.oledAt = oledAt;
    mach.pld->inputMaskHigh = pldIn; mach.pld->reg10 = pldIn10; mach.rotations = rotations; mach.pldEvents = pldEvents; mach.injects = injects; mach.haRemoteEvents = haRemoteEvents; mach.powerHoldMs = powerHoldMs;
    mach.fanHz = fanHz; mach.pld->aes.chip = aesChip; mach.presses = presses; mach.pcapPath = pcapPath; mach.tapName = tapName; mach.openNetwork(); if (haveMac) memcpy(mach.mac, macAddr, 6);
    if (dntPortA || dntPortB) mach.mainFpga->openUartPorts(dntPortA, dntPortB);
    if (danteModel && !dntPortA) { mach.attachDanteModel(); mach.dante->bootMs = danteBootMs; if (danteRespMs >= 0) mach.dante->respDelayMs = danteRespMs; if (danteDeferMs >= 0) mach.dante->deferredDelayMs = danteDeferMs; if (danteSwMode >= 0) mach.dante->swMode = mach.dante->swModeConfigured = (uint8_t)danteSwMode; mach.dante->deviceName = danteFresh ? mach.dante->freshName() : mach.dante->configuredName(1); if (browseC >= 0) { mach.dante->browseCode = browseC; mach.dante->browseFlags = browseF; mach.dante->browseChanged = browseG; } }   // behavioural module unless UART A is bridged out
    for (auto& d : logDevs) if (d == "DANTE") mach.enableLog(d);
    mach.mainFpga->src[8].present = false; mach.mainFpga->src[8].fs = danteFs;
    mach.mainFpga->danteClockToggleMs = danteFs > 0 ? danteClockMs : std::vector<int>{};
    if (!mach.loadFirmware(fw)) return 1;
    for (auto& d : logDevs) mach.enableLog(d);
    mach.cpu.debugExc = debugExc;
    mach.cpu.traceOnIrq = traceOnIrq;
    mach.cpu.tracePc = tracePc; mach.cpu.tracePcLen = tracePcLen;
    if (watchLen) {
        mach.bus.watchLo = Bus::phys(watchLo); mach.bus.watchHi = Bus::phys(watchLo) + watchLen;
        mach.bus.onWatch = [&](uint32_t addr, int size, bool write, uint32_t val) {
            // report each distinct (address, PC, direction) at most 4 times so busy registers do not drown the rest
            static std::map<uint64_t, int> seen;
            uint64_t key = ((uint64_t)addr << 33) | ((uint64_t)(mach.cpu.PC & 0x1FFFFFFF) << 1) | (write ? 1 : 0);
            if (seen[key]++ < watchMax) fprintf(stderr, "[watch] %s %08X size %d val %0*X at PC=%08X insn %llu\n", write ? "wr" : "rd", addr, size, size * 2, val, mach.cpu.PC, (unsigned long long)mach.cpu.insns);
        };
    }
    mach.cpu.hooks = hooks;
    mach.cpu.probes = probes; mach.cpu.probeAfter = probeAfter; mach.cpu.probeLeft = probeMax; mach.cpu.idlePc = idlePc;
    mach.cpu.ringEnable = ringDump > 0;
    if (audioOn) {
        mach.audio = std::make_unique<AudioGlue>(mach);
        auto& g = *mach.audio;
        g.inPath = audioIn; g.inLoop = audioLoop; g.outPath = audioOut; g.aesPath = aesOut; g.txPath = txOut; g.tones = tones; g.devIn = audioDevIn; g.devOut = audioDevOut; g.devLatencyMs = audioDevLatency;
        g.patchSpec = dantePatch; g.mapSpec = audioMap; g.latencyMs = danteLatencyMs; g.dropPercent = danteDrop; g.netPrimary = danteNet; g.netSecondary = danteNet2; g.netClockLeader = danteLeader; g.netFails = danteFails; g.netName = danteName; g.ctlSpec = danteCtl;
        g.hostInDbu = hostInDbu; g.hostOutDbu = hostOutDbu; g.traceInput = traceInput; g.traceMs = traceMs; g.meterPrintMs = meterPrintMs;
        if (!g.start()) return 1;
    }
    if (!consoleScriptPath.empty()) {
        FILE* cf = fopen(consoleScriptPath.c_str(), "r");
        if (!cf) { fprintf(stderr, "cannot open console script %s\n", consoleScriptPath.c_str()); return 1; }
        char line[512];
        while (fgets(line, sizeof line, cf)) {
            char* end = nullptr; unsigned long long t = strtoull(line, &end, 10);
            if (end == line) continue;
            while (*end == ' ' || *end == '\t') end++;
            std::string text(end); while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            mach.consoleScript.push_back({ t, text });
        }
        fclose(cf);
    }

    if (!disasmAt.empty()) {           // --disasm ADDR[,COUNT]: print disassembly of the loaded image and exit
        uint32_t a = strtoul(disasmAt.c_str(), 0, 16);
        int cnt = 32;
        if (auto c = disasmAt.find(','); c != std::string::npos) cnt = atoi(disasmAt.c_str() + c + 1);
        for (int i = 0; i < cnt; i++) {
            uint16_t op = mach.bus.read16(a), op2 = mach.bus.read16(a + 2);
            bool is32 = ((op & 0xF000) == 0x3000 && ((op & 0xF) == 1 || (op & 0xF) == 9)) || (op & 0xF00E) == 0x0000;   // 0x3nm1/0x3nm9 forms, movi20/movi20s
            if (is32) printf("%08X  %04X %04X  %s\n", a, op, op2, mach.cpu.disasm(a, op, op2).c_str());
            else      printf("%08X  %04X       %s\n", a, op, mach.cpu.disasm(a, op, 0).c_str());
            a += is32 ? 4 : 2;
        }
        return 0;
    }

    if (logUnmapped) {
        mach.bus.onUnmapped = [&](uint32_t addr, int size, bool write, uint32_t val) {
            static int n = 0;
            if (n++ >= 200) return;
            if (write) fprintf(stderr, "[bus] unmapped write %08X size %d val %08X at PC=%08X PR=%08X\n", addr, size, val, mach.cpu.PC, mach.cpu.PR);
            else       fprintf(stderr, "[bus] unmapped read  %08X size %d at PC=%08X PR=%08X\n", addr, size, mach.cpu.PC, mach.cpu.PR);
            { fprintf(stderr, "      stack:"); for (int k = 0; k < 10; k++) fprintf(stderr, " %08X", mach.bus.read32((mach.cpu.R[15] + 4 * k) & ~3u)); fprintf(stderr, "\n"); }
        };
    }

    // console: SCIF0 <-> stdin/stdout
    FILE* tf = stdout;
    if (!traceFile.empty()) tf = fopen(traceFile.c_str(), "w");
    mach.cpu.traceOut = tf;
    mach.scif[0]->onTx = [](uint8_t b) { fputc(b, stdout); fflush(stdout); };
    platformInit();
    signal(SIGINT, onSig);
    bool haveTty = stdinIsTty() && rawTty;
#ifndef _WIN32
    struct termios oldt{};
    if (haveTty) { tcgetattr(0, &oldt); struct termios t = oldt; t.c_lflag &= ~(ICANON | ECHO); tcsetattr(0, TCSANOW, &t); }
    std::thread stdinThread([&] {
        int fl = fcntl(0, F_GETFL); fcntl(0, F_SETFL, fl | O_NONBLOCK);
        while (!g_quit) {
            uint8_t c; ssize_t r = ::read(0, &c, 1);
            if (r == 1) { if (c == 0x1D) { g_quit = true; break; } mach.scif[0]->rxByte(c == '\n' ? '\r' : c); }
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
#else
    std::thread stdinThread([&] {                       // Windows: poll the console keyboard; a redirected stdin is not read (use --console-script)
        while (!g_quit) {
            if (haveTty && _kbhit()) { int c = _getch(); if (c == 0x1D) { g_quit = true; break; } mach.scif[0]->rxByte((uint8_t)(c == '\n' ? '\r' : c)); }
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
#endif

#ifdef _WIN32
    if (realtime) { timeBeginPeriod(1); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }   // 1 ms scheduler granularity; the emulation competes with everything else on the host
#endif
    fprintf(stderr, "[emu] starting at PC=%08X SP=%08X  (Ctrl-] to quit)\n", mach.cpu.PC, mach.cpu.R[15]);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t lastCycles = 0;
    std::map<uint32_t, uint64_t> hist;
    while (!g_quit && !mach.powerLost) {
        if (runMs && mach.mainFpga->elapsedCycles / mach.mainFpga->cyclesPerMs >= runMs) { fprintf(stderr, "[emu] --run-ms %llu reached\n", runMs); break; }
        if (trace && mach.cpu.insns >= traceFrom) mach.cpu.trace = true;
        if (haveStop) {
            for (int i = 0; i < 2000 && !g_quit; i++) {
                if (Bus::phys(mach.cpu.PC) == Bus::phys(stopAt)) {
                    fprintf(stderr, "[emu] stop-at %08X reached after %llu insns\n", stopAt, (unsigned long long)mach.cpu.insns);
                    mach.cpu.dumpRegs(); if (ringDump) mach.cpu.dumpRing(ringDump);
                    g_quit = true; break;
                }
                mach.cpu.step();
            }
        } else {
            for (int i = 0; i < 2000 && !g_quit; i++) { mach.cpu.step(); if (mach.cpu.idleHit) { mach.cpu.idleHit = false; break; } }   // keep board timing fine-grained while idling
        }
        if (pcHist) hist[Bus::phys(mach.cpu.PC) & ~0x3Fu]++;
        uint64_t dc = mach.cpu.cycles - lastCycles; lastCycles = mach.cpu.cycles;
        mach.tick(dc);
        if (realtime) {                                                                  // pace emulated time to the wall clock
            // sleep for the bulk of the lead, then spin (yield) for the last millisecond: the OS timer alone overshoots by
            // up to 15 ms on Windows, which the Dante endpoint's packet timing cannot absorb
            double emuMs = mach.mainFpga->elapsedCycles / mach.mainFpga->cyclesPerMs;
            double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (wallMs > emuMs + 50.0) {                                                     // fell behind (boot, a stall): do not race to catch up -
                t0 = std::chrono::steady_clock::now() - std::chrono::microseconds((long long)(emuMs * 1000.0));   // re-base so the clock runs at 1x from here on
                wallMs = emuMs;
            }
            if (emuMs > wallMs + 2.5) std::this_thread::sleep_for(std::chrono::microseconds((long long)((emuMs - wallMs - 2.0) * 1000.0)));
            for (int spins = 0; spins < 200000; spins++) {
                wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (emuMs <= wallMs + 0.25) break;
                std::this_thread::yield();
            }
        }
        if (maxInsns && mach.cpu.insns >= maxInsns) break;
    }
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "\n[emu] stopped: %llu insns, %.1f Minsn/s, PC=%08X, unmapped accesses=%llu, idle iterations skipped %llu\n",
            (unsigned long long)mach.cpu.insns, mach.cpu.insns / dt / 1e6, mach.cpu.PC, (unsigned long long)mach.bus.unmappedCount, (unsigned long long)mach.cpu.idleSkipped);
    if (pcHist) {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto& kv : hist) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        fprintf(stderr, "[emu] PC histogram (64-byte buckets, sampled every 2000 insns):\n");
        for (size_t i = 0; i < v.size() && i < 15; i++)
            fprintf(stderr, "   %08X  %6.2f%%\n", v[i].second, 100.0 * v[i].first / (mach.cpu.insns / 2000.0));
    }
    if (!dumpAt.empty()) {               // --dump ADDR,LEN : hex dump of emulated memory at exit
        uint32_t a = strtoul(dumpAt.c_str(), 0, 16); uint32_t len = 256;
        if (auto c = dumpAt.find(','); c != std::string::npos) len = strtoul(dumpAt.c_str() + c + 1, 0, 16);
        for (uint32_t o = 0; o < len; o += 16) {
            fprintf(stderr, "%08X:", a + o);
            for (int k = 0; k < 16; k += 4) fprintf(stderr, " %08X", mach.bus.read32(a + o + k));
            fprintf(stderr, "\n");
        }
    }
    if (!ramDumpPath.empty()) { FILE* rf = fopen(ramDumpPath.c_str(), "wb"); if (rf) { fwrite(mach.bus.ramPtr(Machine::kSdramBase), 1, Machine::kSdramSize, rf); fclose(rf); fprintf(stderr, "[emu] SDRAM dumped to %s\n", ramDumpPath.c_str()); } }
    mach.shutdown();
#ifndef _WIN32
#ifndef _WIN32
    if (haveTty) tcsetattr(0, TCSANOW, &oldt);
#endif
#endif
    g_quit = true; stdinThread.join();
    if (tf != stdout) fclose(tf);
    return 0;
}
