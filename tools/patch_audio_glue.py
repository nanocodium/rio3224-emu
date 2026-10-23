#!/usr/bin/env python3
"""patch_audio_glue.py : wire the audio engine into the board (machine, DSP meter port, head-amp link, main options)."""
import os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
nl = chr(92) + 'n'

def rw(path, pairs):
    p = os.path.join(root, path); s = open(p, encoding='utf-8', newline='').read()
    for old, new in pairs:
        assert old in s, (path, old[:70])
        s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)

# --- machine.h: include + member -------------------------------------------------------------------------------
rw('emu/src/board/machine.h', [
("""#include <vector>

struct Machine {""",
 """#include <vector>
#include "audio_glue.h"

struct Machine {"""),
("""    HaLink haLink;                           // SCIF1: head-amp FPGA command link""",
 """    HaLink haLink;                           // SCIF1: head-amp FPGA command link
    std::unique_ptr<AudioGlue> audio;        // the PCM engine (head amps, DSP, Dante TX/RX, outputs) fed by the firmware's writes"""),
])

# --- machine.cpp: tick + shutdown ----------------------------------------------------------------------------------
rw('emu/src/board/machine.cpp', [
("""    if (dante) { dante->tick((uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs)); mainFpga->src[8].present = dante->running; }   // module up -> word clock + heartbeats""",
 """    if (dante) { dante->tick((uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs)); mainFpga->src[8].present = dante->running; }   // module up -> word clock + heartbeats
    if (audio) audio->tick((uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs));"""),
("""void Machine::shutdown() {""",
 """void Machine::shutdown() {
    if (audio) audio->shutdown();"""),
])

# --- dspif.h: meter port -------------------------------------------------------------------------------------------
rw('emu/src/board/dspif.h', [
("""    std::function<void(uint32_t word)> onSerial;""",
 """    std::function<void(uint32_t word)> onSerial;
    std::function<uint16_t(int index)> meterRead;                 // +0x8E meter port: write index, read level (engine)
    uint8_t meterIndex = 0;"""),
("""        case 0x94: v = ctrl; break;
        default: v = 0; for (int i = 0; i < size; i++) v = (v << 8) | at(o + i); break;
        }
        if (size == 2) v = (o & 2) ? (v & 0xFFFF) : (v >> 16);""",
 """        case 0x94: v = ctrl; break;
        case 0x8E: v = meterRead ? meterRead(meterIndex) : 0; break;
        default: v = 0; for (int i = 0; i < size; i++) v = (v << 8) | at(o + i); break;
        }
        if (o != 0x8E) { if (size == 2) v = (o & 2) ? (v & 0xFFFF) : (v >> 16);"""),
("""        else if (size == 1) v = (v >> (8 * (3 - (o & 3)))) & 0xFF;
        if (log) fprintf(stderr, "[CS4] rd %08X -> %0*X""" + nl + """", addr, size * 2, v);""",
 """        else if (size == 1) v = (v >> (8 * (3 - (o & 3)))) & 0xFF; }
        if (log) fprintf(stderr, "[CS4] rd %08X -> %0*X""" + nl + """", addr, size * 2, v);"""),
("""        case 0x94: ctrl = val; break;
        case 0xA8: {""",
 """        case 0x94: ctrl = val; break;
        case 0x8E: meterIndex = (uint8_t)(val & 0xFF); break;      // bit 15 = "muted" flag from the firmware, index in the low byte
        case 0xA8: {"""),
])

# --- halink.h: which head-amp sheets the firmware addressed --------------------------------------------------------
rw('emu/src/board/halink.h', [
("""    uint8_t reg[4][8] = {};             // last value per FPGA and command""",
 """    uint8_t reg[4][8] = {};             // last value per FPGA and command
    bool seen[4] = {};                  // a frame was sent to this FPGA (sheet present as far as the firmware is concerned)"""),
("""        reg[fpga][cmd] = b; frames++; haveHdr = false;""",
 """        reg[fpga][cmd] = b; frames++; haveHdr = false; seen[fpga] = true;"""),
])

# --- dante_transport.h: TX tap on the loopback transport ------------------------------------------------------------
rw('emu/src/audio/dante_transport.h', [
("""    void sendTx(const float* const* tx, int nch, size_t frames, uint64_t sampleTime) override {
        st.txBlocks++;""",
 """    std::function<void(const float* const* tx, int nch, size_t frames)> txTap;   // e.g. a WAV recorder = virtual Dante receiver of all TX channels
    void sendTx(const float* const* tx, int nch, size_t frames, uint64_t sampleTime) override {
        st.txBlocks++;
        if (txTap) txTap(tx, nch, frames);"""),
])

# --- main.cpp: options -----------------------------------------------------------------------------------------------
rw('emu/src/main.cpp', [
("""        else if (a == "--dante-fresh") danteFresh = true;""",
 """        else if (a == "--dante-fresh") danteFresh = true;
        else if (a == "--audio-in" && i + 1 < argc) { std::string v = argv[++i]; size_t c = v.find(",loop"); audioIn = v.substr(0, c); audioLoop = c != std::string::npos; audioOn = true; }
        else if (a == "--audio-out" && i + 1 < argc) { audioOut = argv[++i]; audioOn = true; }
        else if (a == "--aes-out" && i + 1 < argc) { aesOut = argv[++i]; audioOn = true; }
        else if (a == "--tx-out" && i + 1 < argc) { txOut = argv[++i]; audioOn = true; }
        else if (a == "--tone" && i + 1 < argc) { tones.push_back(argv[++i]); audioOn = true; }
        else if (a == "--dante-patch" && i + 1 < argc) { dantePatch = argv[++i]; audioOn = true; }
        else if (a == "--dante-latency-ms" && i + 1 < argc) danteLatencyMs = atof(argv[++i]);
        else if (a == "--dante-drop" && i + 1 < argc) danteDrop = atof(argv[++i]);
        else if (a == "--host-in-dbu" && i + 1 < argc) hostInDbu = atof(argv[++i]);
        else if (a == "--host-out-dbu" && i + 1 < argc) hostOutDbu = atof(argv[++i]);
        else if (a == "--audio-map" && i + 1 < argc) audioMap = argv[++i];
        else if (a == "--trace-input" && i + 1 < argc) { traceInput = atoi(argv[++i]); audioOn = true; }
        else if (a == "--meter-print" && i + 1 < argc) { meterPrintMs = atoi(argv[++i]); audioOn = true; }
        else if (a == "--audio") audioOn = true;"""),
("""int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath, ramDumpPath;""",
 """int danteSwMode = -1; bool danteFresh = false; std::string consoleScriptPath, ramDumpPath;
    bool audioOn = false, audioLoop = false; std::string audioIn, audioOut, aesOut, txOut, dantePatch, audioMap; std::vector<std::string> tones;
    double danteLatencyMs = 1.0, danteDrop = 0.0, hostInDbu = 24.0, hostOutDbu = 24.0; int traceInput = 0, meterPrintMs = 0;"""),
("""    mach.cpu.ringEnable = ringDump > 0;""",
 """    mach.cpu.ringEnable = ringDump > 0;
    if (audioOn) {
        mach.audio = std::make_unique<AudioGlue>(mach);
        auto& g = *mach.audio;
        g.inPath = audioIn; g.inLoop = audioLoop; g.outPath = audioOut; g.aesPath = aesOut; g.txPath = txOut; g.tones = tones;
        g.patchSpec = dantePatch; g.mapSpec = audioMap; g.latencyMs = danteLatencyMs; g.dropPercent = danteDrop;
        g.hostInDbu = hostInDbu; g.hostOutDbu = hostOutDbu; g.traceInput = traceInput; g.meterPrintMs = meterPrintMs;
        if (!g.start()) return 1;
    }"""),
])
print('audio glue wired')
