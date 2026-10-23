#include "machine.h"
#include "net/dante_page.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>

RegBlock* Machine::addBlock(const std::string& name, uint32_t base, uint32_t size) {
    blocks.push_back(std::make_unique<RegBlock>(name, base, size));
    bus.addDevice(base, size, blocks.back().get());
    return blocks.back().get();
}

Machine::Machine() {
    // memories
    flash = std::make_unique<NorFlash>(kFlashSize, 128u << 10);
    flash->nowMs = [this] { return mainFpga ? (uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs) : 0; };
    bus.addDevice(kFlashBase, kFlashSize, flash.get());
    bus.addRam(kSdramBase, kSdramSize, "SDRAM (CS3)");
    bus.addRam(kRamBase, kRamSize, "on-chip RAM");
    // external devices
    pld = std::make_unique<Pld>();
    bus.addDevice(0x40001400, 0x100, pld.get());
    fpga = addBlock("FPGA(CS1)", 0x04000000, 0x04000000);
    addBlock("CS2", 0x08000000, 0x04000000);
    dspIf = std::make_unique<DspIf>();
    bus.addDevice(0x10000000, 0x04000000, dspIf.get());
    mainFpga = std::make_unique<MainFpga>();
    mainFpga->oledDc = [this] { return (pld->reg6 & 0x80) != 0; };   // OLED D/C# comes from the PLD
    mainFpga->oledSel = [this] { return (uint8_t)(((pld->reg6 & 0x100) ? 0 : 1) | ((pld->reg6 & 0x200) ? 0 : 2)); };   // nCS lines, PLD +6 bits 8/9
    bus.addDevice(0x14000000, 0x04000000, mainFpga.get());
    pld->irqInputs = [this] { return (uint16_t)(mainFpga->irqActive ? 0x10 : 0); };
    pld->gpiInputs = [this] { return mainFpga->gpiBits(); };
    pld->ledExtra = [this](int id) { return id >= 120 && id < 124 ? (int)bus.read8(0x0C764C81 + id) : -1; };   // panel object LED state array (sheet 9 firmware)
    enet = std::make_unique<Dm9000>();
    bus.addDevice(0x18000000, 0x04000000, enet.get());
    addBlock("CS7", 0x1C000000, 0x04000000);
    // on-chip peripherals
    intc = std::make_unique<Intc>(cpu);
    bus.addDevice(0xFFFE0800, 0x0400, intc.get());        // ICR/IRQRR/IPR01-05/IBCR/IBNR
    bus.addDevice(0xFFFE0C00, 0x0400, intc.get());        // IPR06-17
    mainFpga->setIrq = [this](bool a) { intc->setLine(65, a); };   // main FPGA -> /IRQ_PLD -> IRQ1 (level)
    enet->setIrq = [this](bool a) { intc->setLine(64, a); };       // DM9000 -> IRQ0 (level)
    cmt = std::make_unique<Cmt>(*intc, 16000000.0);   // Pφ = 16 MHz (SCIF 38400 baud with SCBRR=12, 10 ms OS tick, 500 us MTU0)
    bus.addDevice(0xFFFEC000, 0x0010, cmt.get());
    for (int i = 0; i < 4; i++) {
        scif[i] = std::make_unique<Scif>(i, *intc);
        snprintf(scif[i]->nm, sizeof scif[i]->nm, "SCIF%d", i);
        bus.addDevice(0xFFFE8000 + i * 0x800, 0x30, scif[i].get());
    }
    scif[1]->onTx = [this](uint8_t b) { haLink.rxByteFromHost(b); };
    bsc = addBlock("BSC", 0xFFFC0000, 0x0100);
    addBlock("UBC", 0xFFFC0400, 0x0100);
    addBlock("CACHE", 0xFFFC1000, 0x0010);
    addBlock("USB", 0xFFFC1C00, 0x0100);
    addBlock("SDRAM-mode", 0xFFFC4000, 0x2000);
    cpg = addBlock("WDT/CPG", 0xFFFE0000, 0x0020);
    addBlock("STBY/SYSCR", 0xFFFE0400, 0x0020);
    addBlock("DMAC", 0xFFFE1000, 0x0400);
    addBlock("FLCTL", 0xFFFE2000, 0x0100);
    pfc = addBlock("PFC", 0xFFFE3800, 0x0300);
    mtu2 = std::make_unique<Mtu2>(*intc, 16000000.0);
    bus.addDevice(0xFFFE4000, 0x0400, mtu2.get());
    adc = std::make_unique<Adc>(*intc);
    bus.addDevice(0xFFFE5800, 0x0040, adc.get());
    addBlock("SSU0", 0xFFFE7000, 0x0020);
    addBlock("SSU1", 0xFFFE7800, 0x0020);
    addBlock("IIC3-0", 0xFFFEE000, 0x0010);
    addBlock("IIC3-1", 0xFFFEE400, 0x0010);
    addBlock("IIC3-2", 0xFFFEE800, 0x0010);
    addBlock("IIC3-3", 0xFFFEEC00, 0x0010);
    addBlock("RSPI?", 0xFFFF0000, 0x0200);
    addBlock("RSPI1?", 0xFFFF0800, 0x0200);
    addBlock("RTC", 0xFFFF2000, 0x0040);
    addBlock("DEEPSTBY", 0xFFFF2800, 0x0010);
    addBlock("SSI0", 0xFFFFC000, 0x0010);
    addBlock("SSI1", 0xFFFFC800, 0x0010);
    addBlock("SSI2", 0xFFFFD000, 0x0010);
    addBlock("SSI3", 0xFFFFD800, 0x0010);
    addBlock("RSPI2?", 0xFFFFF000, 0x0080);
    addBlock("LCDC", 0xFFFFFC00, 0x0100);

    // CPG reset values (SH7203): FRQCR = 0x0003? ; WDT: WTCSR 0x18, WRCSR 0x1F
    cpg->set16(0xFFFE0010, 0x1003);
    cpg->set8(0xFFFE0000, 0x18);
    cpg->set8(0xFFFE0004, 0x1F);
    // BSC CMNCR reset
    bsc->set32(0xFFFC0000, 0x00001010);
}

bool Machine::loadFirmware(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open firmware %s\n", path.c_str()); return false; }
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
    if (img.size() < 0x20 || memcmp(img.data(), "V", 1) != 0) { fprintf(stderr, "unexpected firmware header\n"); }
    uint8_t* dst = bus.ramPtr(kLoadAddr - 0x10);          // boot loader copies the bank file incl. its 16-byte header
    memcpy(dst, img.data(), img.size());
    if (!flashPath.empty() && !flash->load(flashPath)) fprintf(stderr, "[flash] no image at %s, starting blank (erased)\n", flashPath.c_str());
    // The firmware also expects to see itself in flash at 0x000F1000 (its SDRAM load address minus 0x0C000000):
    // part of its constant data is addressed 0x00-based. Keep the bank image there (header at 0xF0FF0).
    {
        const uint32_t bankOff = kLoadAddr - kSdramBase - 0x10;
        if (memcmp(flash->data() + bankOff, img.data(), img.size()) != 0) {
            memcpy(flash->data() + bankOff, img.data(), img.size());
            flash->dirty = true;
            fprintf(stderr, "[flash] placed firmware bank image at flash offset %06X\n", bankOff);
        }
    }
    fprintf(stderr, "[emu] loaded %zu bytes of %s at %08X (header '%.5s')\n", img.size() - 0x10, path.c_str(), kLoadAddr, img.data());
    // boot loader bank (flash 0x000000..0x0F0FFF, mirrored to SDRAM 0x0C000000) is not in the update package:
    // provide its version header "Vxxx" at 0x0C000FF0 / flash 0x000FF0 so the monitor shows something sane
    { static const char bootHdr[16] = { 'V','1','0','0',' ',0, (char)0xFF,(char)0xFF,(char)0xFF,(char)0xFF,(char)0xFF,(char)0xFF,(char)0xFF,(char)0xFF,(char)0xFF,(char)0xFF };
      memcpy(bus.ramPtr(kSdramBase + 0xFF0), bootHdr, 16);
      if (memcmp(flash->data() + 0xFF0, bootHdr, 16) != 0) { memcpy(flash->data() + 0xFF0, bootHdr, 16); flash->dirty = true; } }
    initBoardInfo();
    cpu.reset();
    cpu.fetchPtr = bus.ramPtr(kSdramBase); cpu.fetchBase = kSdramBase; cpu.fetchSize = kSdramSize;   // code runs from SDRAM
    cpu.PC = kLoadAddr;
    cpu.R[15] = kRamBase + kRamSize - 16;
    cpu.VBR = 0;
    return true;
}

void Machine::tick(uint64_t cycles) {
    cmt->tick(cycles);
    mtu2->tick(cycles);
    for (auto& s : scif) s->tick(cycles);
    mainFpga->tick(cycles);
    if (tapFd >= 0) { netPollAcc += cycles; if (netPollAcc >= (uint64_t)mainFpga->cyclesPerMs) { netPollAcc = 0; pollNetwork(); } }
    if (consolePos < consoleScript.size() || !consolePending.empty()) {   // scripted debug-console input, one byte per 2 ms
        uint64_t ms = mainFpga->elapsedCycles / mainFpga->cyclesPerMs;
        if (consolePos < consoleScript.size() && ms >= consoleScript[consolePos].ms) { consolePending += consoleScript[consolePos].text; consolePending += '\r'; fprintf(stderr, "[console] %llu ms > %s\n", (unsigned long long)ms, consoleScript[consolePos].text.c_str()); consolePos++; }
        if (!consolePending.empty() && ms >= consoleNextByteMs) { scif[0]->rxByte((uint8_t)consolePending[0]); consolePending.erase(0, 1); consoleNextByteMs = ms + 2; }
    }
    {   // board inputs that depend on time: fan tacho square wave, scheduled switch presses
        const double ms = mainFpga->elapsedCycles / mainFpga->cyclesPerMs;
        pld->fanOk = fanHz > 0;                                                                  // --fan-hz 0 simulates a stopped fan
        uint8_t b = 0;
        for (auto& pr : presses) if (ms >= pr.fromMs && ms < pr.toMs && pr.sw >= 0 && pr.sw < 8) b |= 1 << pr.sw;
        for (auto& ev : pldEvents) if (!ev.done && ms >= ev.ms) { ev.done = true; switch (ev.kind) { case 0: pld->uvlo = ev.on; if (ev.on) powerLossMs = ms + powerHoldMs; else powerLossMs = 0; break; case 1: pld->poffA = ev.on; break; case 2: pld->poffB = ev.on; break; case 3: pld->master48 = ev.on; break; } fprintf(stderr, "[pld] %llu ms: %s %s\n", (unsigned long long)ms, ev.kind == 0 ? "/UVLO" : ev.kind == 1 ? "PSU A fail" : ev.kind == 2 ? "PSU B fail" : "+48V MASTER", ev.on ? "asserted" : "released"); }
        pld->buttons = b;
        for (auto& inj : injects) if (!inj.done && ms >= inj.ms) { inj.done = true; mainFpga->moduleToHost(inj.ch, inj.bytes.data(), inj.bytes.size()); fprintf(stderr, "[DNT%c] %llu ms: injected %zu bytes from the module side\n", 'A' + inj.ch, (unsigned long long)ms, inj.bytes.size()); }
        if (powerLossMs && ms >= powerLossMs && !powerLost) { powerLost = true; fprintf(stderr, "[pld] %llu ms: supply rails down (%llu ms after /UVLO) - power lost\n", (unsigned long long)ms, (unsigned long long)powerHoldMs); }
        if (!rotations.empty()) {                                                                // rotary encoder position = base + steps done so far
            int pos = 0; bool gray = false;
            for (auto& r : rotations) {
                if (ms < r.fromMs) continue;
                int done = (int)((ms - r.fromMs) / (r.msPerStep > 0 ? r.msPerStep : 60)) + 1;
                int n = r.steps < 0 ? -r.steps : r.steps; if (done > n) done = n;
                pos += r.steps < 0 ? -done : done; gray = r.gray;
            }
            rotaryCode = pos;
            pld->encPhase = (uint8_t)(pos & 3);                                              // quadrature phase on PLD +0 bits 13/14
        }
    }
    if (dante) {
        uint64_t ms = (uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs);
        dante->tick(ms); mainFpga->src[8].present = dante->running;                         // module up -> word clock + heartbeats
        if (!dante->running) danteRunningSinceMs = 0; else if (!danteRunningSinceMs) danteRunningSinceMs = ms;
        for (auto& e : haRemoteEvents) if (e.first && ms >= e.first && dante->running) { dante->notifyHaRemote(e.second); fprintf(stderr, "[DANTE] %llu ms: HA remote console %s\n", (unsigned long long)ms, e.second ? "connected" : "disconnected"); e.first = 0; }
        mainFpga->dntMute = !(dante->running && danteRunningSinceMs && ms >= danteRunningSinceMs + (uint64_t)dntMuteReleaseMs);
    }
    if (audio) audio->tick((uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs));
    if (panelHttp) panelHttp->poll();
    for (auto& o : oledAt) if (!o.done && mainFpga->elapsedCycles / mainFpga->cyclesPerMs >= o.ms) { o.done = true; writeOled(o.path); }
    enet->tick((uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs));
}

// Board-info records in the boot bank at flash 0xE0000 (descriptor table at 0x0C2C8F90): each record is a
// big-endian magic 0x00001E61 followed by the data at +4; record 0 = MAC (6), 1 = 17-byte string (serial),
// 2 = 33-byte string (device label), 3 = 40 bytes. A blank record makes the host show "ILLEGAL MAC ADDRESS".
void Machine::initBoardInfo() {
    struct Rec { uint32_t off; size_t len; } recs[4] = { { 0xE0000, 6 }, { 0xE0040, 17 }, { 0xE0080, 33 }, { 0xE00C0, 40 } };
    uint8_t* f = flash->data();
    bool blank = true;
    for (int i = 0; i < 4; i++) if (f[recs[i].off] != 0xFF) blank = false;
    if (!blank) return;
    auto put = [&](uint32_t off, const uint8_t* p, size_t n) {
        f[off] = 0; f[off + 1] = 0; f[off + 2] = 0x1E; f[off + 3] = 0x61;
        for (size_t i = 0; i < n; i++) f[off + 4 + i] = p[i];
    };
    std::vector<uint8_t> s1(17, 0), s2(33, 0), s3(40, 0);
    memcpy(s1.data(), serial.c_str(), std::min<size_t>(serial.size(), 16));
    memcpy(s2.data(), label.c_str(), std::min<size_t>(label.size(), 32));
    put(recs[0].off, mac, 6); put(recs[1].off, s1.data(), 17); put(recs[2].off, s2.data(), 33); put(recs[3].off, s3.data(), 40);
    flash->dirty = true;
    fprintf(stderr, "[flash] wrote board-info records at 0xE0000 (MAC %02X:%02X:%02X:%02X:%02X:%02X)\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void Machine::attachDanteModel() {
    dante = std::make_unique<DanteModule>();
    dante->toHost = [this](const uint8_t* p, size_t n) { mainFpga->moduleToHost(0, p, n); };
    mainFpga->onHostTx = [this](int ch, uint8_t b) { if (ch == 0) dante->hostByte(b); };
    mainFpga->danteClockToggleMs.clear();                     // the model decides when its clock appears
}

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#endif

void Machine::openNetwork() {
    if (!pcapPath.empty()) {
        pcap = fopen(pcapPath.c_str(), "wb");
        if (pcap) { uint32_t hdr[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 }; fwrite(hdr, 4, 6, pcap); fflush(pcap); }
        else fprintf(stderr, "[enet] cannot open pcap %s%c", pcapPath.c_str(), 10);
    }
#ifdef __linux__
    if (!tapName.empty()) {
        tapFd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
        if (tapFd >= 0) {
            struct ifreq ifr; memset(&ifr, 0, sizeof ifr); ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
            strncpy(ifr.ifr_name, tapName.c_str(), IFNAMSIZ - 1);
            if (ioctl(tapFd, TUNSETIFF, &ifr) < 0) { perror("[enet] TUNSETIFF"); close(tapFd); tapFd = -1; }
            else fprintf(stderr, "[enet] attached to TAP %s%c", ifr.ifr_name, 10);
        } else perror("[enet] /dev/net/tun");
    }
#endif
    enet->txBackend = [this](const uint8_t* p, size_t n) {
        if (pcap) { double ms = mainFpga->elapsedCycles / mainFpga->cyclesPerMs; uint32_t rec[4] = { (uint32_t)(ms / 1000), (uint32_t)((uint64_t)(ms * 1000) % 1000000), (uint32_t)n, (uint32_t)n }; fwrite(rec, 4, 4, pcap); fwrite(p, 1, n, pcap); fflush(pcap); }
#ifdef __linux__
        if (tapFd >= 0) { ssize_t w = ::write(tapFd, p, n); (void)w; }
#endif
    };
}

void Machine::pollNetwork() {
#ifdef __linux__
    if (tapFd < 0) return;
    uint8_t buf[2048];
    for (int k = 0; k < 8; k++) {
        ssize_t r = ::read(tapFd, buf, sizeof buf);
        if (r <= 0) break;
        enet->receive(buf, (size_t)r);
        if (pcap) { double ms = mainFpga->elapsedCycles / mainFpga->cyclesPerMs; uint32_t rec[4] = { (uint32_t)(ms / 1000), (uint32_t)((uint64_t)(ms * 1000) % 1000000), (uint32_t)r, (uint32_t)r }; fwrite(rec, 4, 4, pcap); fwrite(buf, 1, (size_t)r, pcap); }
    }
#endif
}

void Machine::shutdown() {
    if (audio) audio->shutdown();
    if (pcap) { fclose(pcap); pcap = nullptr; }
    fprintf(stderr, "[enet] frames tx %llu rx %llu%c", (unsigned long long)enet->txFrames, (unsigned long long)enet->rxFrames, 10);
    if (!flashPath.empty() && flash->dirty) {
        if (flash->save(flashPath)) fprintf(stderr, "[flash] saved %s (%llu erases, %llu programs)\n", flashPath.c_str(), (unsigned long long)flash->erases, (unsigned long long)flash->programs);
    }
    // Two OLED panels, each showing columns 0x17..0x60 (x 92..387) and rows 0..47 -> 296x48 pixels. Panel 1 (PLD +6
    // bit 9) is the left one (UI canvas columns 0..295), panel 0 (bit 8) the right one (296..588).
    const int ox = 0x17 * 4, oy = 0, ow = (0x60 - 0x17 + 1) * 4, oh = 48;
    const Ssd1322* left = &mainFpga->oled[1]; const Ssd1322* right = &mainFpga->oled[0];
    if (!oledDumpPath.empty()) writeOled(oledDumpPath);
    if (!oledFullPath.empty()) {                      // both controllers' whole RAM, left panel above right panel
        FILE* f = fopen(oledFullPath.c_str(), "wb");
        if (f) { fprintf(f, "P5%c480 256%c255%c", 10, 10, 10); for (const Ssd1322* p : { left, right }) for (int y = 0; y < 128; y++) for (int x = 0; x < 480; x++) fputc(p->pixel(x, y) * 17, f); fclose(f); }
    }
    if (oledAscii) {
        fprintf(stderr, "[oled] left panel (%u frames, display %s):%c%s", left->frames, left->on ? "on" : "off", 10, left->ascii(ox, oy, ow, oh, false, false).c_str());
        fprintf(stderr, "[oled] right panel (%u frames, display %s):%c%s", right->frames, right->on ? "on" : "off", 10, right->ascii(ox, oy, ow, oh, false, false).c_str());
    }
    if (panelAscii) fprintf(stderr, "[panel] LED chains latched %u times, HA frames %llu:\n%s", pld->ledLatches, (unsigned long long)haLink.frames, pld->ledAscii().c_str());
}

void Machine::enableLog(const std::string& dev) {
    bool any = false;
    for (auto& b : blocks) if (dev == "all" || b->nm.find(dev) != std::string::npos) { b->log = true; any = true; }
    if (dev == "INTC" || dev == "all") { intc->logAccess = true; any = true; }
    if (dev == "MTU2" || dev == "all") { mtu2->log = true; any = true; }
    if (dev == "PLD" || dev == "all") { pld->log = true; any = true; }
    if (dev == "PLDS") { pld->logSerial = true; any = true; }
    if (dev == "AES" || dev == "all") { pld->aes.log = true; any = true; }
    if (dev == "DM9000" || dev == "CS6" || dev == "all") { enet->log = true; enet->logFrames = true; any = true; }
    if (dev == "ENET") { enet->logFrames = true; any = true; }
    if (dev == "CS5" || dev == "all") { mainFpga->log = true; any = true; }
    if (dev == "CS4" || dev == "all") { dspIf->log = true; any = true; }
    if (dev == "DSPIF") { dspIf->logSerial = true; any = true; }
    if (dev == "HA" || dev == "all") { haLink.log = true; any = true; }
    if (dev == "OLED") { mainFpga->oled[0].log = true; any = true; }
    if (dev == "DNTUART") { mainFpga->logUart = true; any = true; }
    if ((dev == "DANTE" || dev == "all") && dante) { dante->log = true; any = true; }
    if (dev == "ADC" || dev == "all") { adc->log = true; any = true; }
    if (dev == "flash" || dev == "all") { flash->log = true; any = true; }
    for (auto& s : scif) if (dev == s->nm || dev == "SCIF" || dev == "all") { s->logAccess = true; any = true; }
    if (!any) fprintf(stderr, "[emu] --log: no device matches '%s'\n", dev.c_str());
}

// Both OLED panels side by side as one PGM (left panel = UI columns 0..295, 8 px gap, right panel).
std::string Machine::oledPgmBytes() const {
    const int ox = 0x17 * 4, oy = 0, ow = (0x60 - 0x17 + 1) * 4, oh = 48, gap = 8;
    const Ssd1322* left = &mainFpga->oled[1]; const Ssd1322* right = &mainFpga->oled[0];
    std::string s; char hdr[48]; snprintf(hdr, sizeof hdr, "P5%c%d %d%c255%c", 10, ow * 2 + gap, oh, 10, 10); s += hdr;
    s.reserve(s.size() + (size_t)(ow * 2 + gap) * oh);
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) s.push_back((char)(left->on ? left->pixel(ox + x, oy + y) * 17 : 0));
        for (int x = 0; x < gap; x++) s.push_back(0);
        for (int x = 0; x < ow; x++) s.push_back((char)(right->on ? right->pixel(ox + x, oy + y) * 17 : 0));
    }
    return s;
}

void Machine::writeOled(const std::string& path) {
    const Ssd1322* left = &mainFpga->oled[1]; const Ssd1322* right = &mainFpga->oled[0];
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    std::string s = oledPgmBytes(); fwrite(s.data(), 1, s.size(), f); fclose(f);
    fprintf(stderr, "[oled] wrote %s at %llu ms (600x48: left panel %u frames %s, right panel %u frames %s)%c", path.c_str(),
            (unsigned long long)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs), left->frames, left->on ? "on" : "off", right->frames, right->on ? "on" : "off", 10);
}

// ---------------------------------------------------------------------------------------------------------------------
// live panel page (--panel-http)
bool Machine::startPanelHttp(uint16_t port) {
    panelHttp = std::make_unique<PanelServer>(); std::string err;
    if (!panelHttp->open(port, err)) { fprintf(stderr, "[panel] cannot serve the panel page: %s%c", err.c_str(), 10); panelHttp.reset(); return false; }
    panelHttp->stateJson = [this] { return panelStateJson(); };
    panelHttp->oledPgm = [this] { return oledPgmBytes(); };
    panelHttp->action = [this](const std::string& p, const std::map<std::string, std::string>& q) { panelAction(p, q); };
    panelHttp->extraRoute = [this](const std::string& p, const std::map<std::string, std::string>& q, std::string& type, std::string& body) {
        rio::DanteEndpoint* ep = audio ? audio->endpoint.get() : nullptr;
        if (p == "/dante") { type = "text/html; charset=utf-8"; body = kDantePageHtml; return true; }
        if (p == "/dante.json") { type = "application/json"; body = ep ? ep->statusJson() : "{\"running\":false}"; return true; }
        if (p == "/dante.log") { type = "text/plain; charset=utf-8"; body = ep ? ep->logText(300) : "endpoint not running (start with --dante-ctl IFADDR)\n"; return true; }
        if (p == "/dante/subscribe" && ep) {   // ?rx=N&tx=NAME&host=DEVICE (empty tx = unsubscribe): the receiver-side path a controller takes, for tests
            auto g = [&](const char* k) { auto it = q.find(k); return it == q.end() ? std::string() : it->second; };
            int rx = atoi(g("rx").c_str()); if (rx >= 1 && rx <= rio::kDanteRx) ep->subscribeNamed(rx - 1, g("tx"), g("host"));
            type = "application/json"; body = "{\"ok\":true}"; return true;
        }
        return false;
    };
    fprintf(stderr, "[panel] live panel page at http://127.0.0.1:%u/%c", port, 10);
    return true;
}

std::string Machine::panelStateJson() {
    uint64_t ms = (uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs);
    std::string s = "{\"ms\":" + std::to_string(ms);
    {   // LED states by logical id (-1 = unknown)
        std::string l = "{";
        auto add = [&](const char* key, int base, int n) { l += "\""; l += key; l += "\":["; for (int i = 0; i < n; i++) { l += std::to_string(pld->ledById(base + i)); if (i + 1 < n) l += ','; } l += "],"; };
        add("p48", 0, 32); add("sig", 32, 32); add("peak", 64, 32); add("sigOut", 96, 16); add("sigAes", 112, 8); add("status", 120, 4);
        l.pop_back(); l += "}"; s += ",\"leds\":" + l;
    }
    s += ",\"meters\":{\"in\":[";
    for (int c = 0; c < 32; c++) { s += audio ? std::to_string((int)std::lround(audio->engine.inputMeter(c).peakDbfs)) : "-200"; if (c < 31) s += ','; }
    s += "],\"out\":[";
    for (int o = 0; o < 24; o++) { s += audio ? std::to_string((int)std::lround(audio->engine.outputMeter(o).peakDbfs)) : "-200"; if (o < 23) s += ','; }
    s += "]}";
    s += ",\"rear\":{\"master48\":" + std::string(pld->master48 ? "true" : "false") + ",\"psuAFail\":" + (pld->poffA ? "true" : "false") + ",\"psuBFail\":" + (pld->poffB ? "true" : "false") + ",\"uvlo\":" + (pld->uvlo ? "true" : "false") + "}";
    s += ",\"realtime\":" + std::string(realtimeFlag ? "true" : "false") + ",\"danteRunning\":" + (dante && dante->running ? "true" : "false") + ",\"clockLocked\":" + (mainFpga->src[8].present ? "true" : "false");
    s += ",\"fs\":" + std::to_string(mainFpga->src[8].fs) + ",\"audio\":" + (audio ? "true" : "false") + ",\"powerLost\":" + (powerLost ? "true" : "false");
    s += ",\"danteNet\":" + std::string(audio && audio->endpoint ? "true" : "false") + "}";
    return s;
}

void Machine::panelAction(const std::string& path, const std::map<std::string, std::string>& q) {
    uint64_t ms = (uint64_t)(mainFpga->elapsedCycles / mainFpga->cyclesPerMs);
    auto num = [&](const char* k, long def) { auto it = q.find(k); return it == q.end() || it->second.empty() ? def : atol(it->second.c_str()); };
    if (path == "/press") { int sw = (int)num("sw", 5); long dur = num("ms", 150); if (sw >= 0 && sw < 8) presses.push_back({ sw, ms, ms + (uint64_t)(dur > 0 ? dur : 150) }); }
    else if (path == "/rotate") { int steps = (int)num("steps", 1); if (steps) rotations.push_back({ steps, ms, 20, true }); }
    else if (path == "/master") pld->master48 = num("on", 1) != 0;
    else if (path == "/psu") { if (q.count("a")) pld->poffA = num("a", 1) == 0; if (q.count("b")) pld->poffB = num("b", 1) == 0; }
    else if (path == "/poweroff") { pld->uvlo = true; powerLossMs = ms + powerHoldMs; fprintf(stderr, "[pld] %llu ms: /UVLO asserted from the panel page%c", (unsigned long long)ms, 10); }
}
