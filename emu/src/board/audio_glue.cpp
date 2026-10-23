#include "audio_glue.h"
#include "machine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <chrono>
#ifdef _WIN32
#include <timeapi.h>
#include <avrt.h>
#endif

using namespace rio;

// DSP meter port index tables (FUN_0c160a54 reads these three banks: 32 input levels, 32 second input bank,
// 24 output levels), from the firmware's tables at 0x0C2A87F4 / 0x0C2A8834 / 0x0C2A8874.
static const uint8_t kBank0[32] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22 };
static const uint8_t kBank1[32] = { 0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2F,0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x40,0x41,0x47,0x48,0x49,0x4A,0x4B };
static const uint8_t kBank2[24] = { 0x4C,0x4D,0x4F,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x6D,0x6E,0x6F,0x70,0x71,0x72,0x73,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F,0x80 };

AudioGlue::AudioGlue(Machine& mach) : m(mach) {
    memset(meterBank, -1, sizeof meterBank); memset(meterPos, 0, sizeof meterPos);
    for (int i = 0; i < 32; i++) { meterBank[kBank0[i]] = 0; meterPos[kBank0[i]] = i; meterBank[kBank1[i]] = 1; meterPos[kBank1[i]] = i; }
    for (int i = 0; i < 24; i++) { meterBank[kBank2[i]] = 2; meterPos[kBank2[i]] = i; }
}

uint16_t AudioGlue::meterRead(int index) const {
    index &= 0xFF;
    if (meterBank[index] < 0) return 0;
    return engine.meterWord(meterBank[index], meterPos[index]);
}

bool AudioGlue::start() {
    enabled = true;
    engine.cfg.hostInFsDbu = hostInDbu; engine.cfg.hostOutFsDbu = hostOutDbu;
    engine.setSampleRate(m.mainFpga->src[8].fs > 0 ? (uint32_t)m.mainFpga->src[8].fs : 48000);
    // host backend
    if (!devIn.empty() || !devOut.empty()) {
        auto w = std::make_unique<WasapiBackend>(); std::string err;
        if (!w->open(devIn, devOut, engine.sampleRate(), devLatencyMs, err)) { fprintf(stderr, "[audio] host device: %s\n", err.c_str()); return false; }
        fprintf(stderr, "[audio] %s\n", w->describe().c_str());
        backend = std::move(w);
    } else if (!tones.empty()) {
        auto t = std::make_unique<ToneBackend>(); t->fs = engine.sampleRate(); t->hostInFsDbu = hostInDbu;
        for (auto& s : tones) if (!t->parse(s)) { fprintf(stderr, "[audio] bad --tone spec '%s'\n", s.c_str()); return false; }
        backend = std::move(t);
    } else {
        auto f = std::make_unique<FileBackend>();
        if (!inPath.empty() && !f->openInput(inPath, inLoop)) { fprintf(stderr, "[audio] cannot open %s\n", inPath.c_str()); return false; }
        if (!inPath.empty() && f->in.sampleRate != engine.sampleRate())
            fprintf(stderr, "[audio] warning: %s is %u Hz, the Rio runs at %u Hz (no host SRC yet: resample the file)\n", inPath.c_str(), f->in.sampleRate, engine.sampleRate());
        if (!outPath.empty()) { f->outAopen = f->openAnalogOut(outPath, engine.sampleRate()); if (!f->outAopen) { fprintf(stderr, "[audio] cannot create %s\n", outPath.c_str()); return false; } }
        if (!aesPath.empty()) { f->outAesOpen = f->openAesOut(aesPath, engine.sampleRate()); if (!f->outAesOpen) { fprintf(stderr, "[audio] cannot create %s\n", aesPath.c_str()); return false; } }
        backend = std::move(f);
    }
    engine.setBackend(backend.get());
    // virtual cables
    if (!mapSpec.empty()) {
        for (int i = 0; i < kInputs; i++) engine.mapInput(i, -1);
        std::string cur; auto apply = [&](const std::string& p) { size_t c = p.find(':'); if (c == std::string::npos) return; int rio = atoi(p.substr(0, c).c_str()), host = atoi(p.substr(c + 1).c_str()); if (rio >= 1 && rio <= kInputs) engine.mapInput(rio - 1, host - 1); };
        for (char c : mapSpec) { if (c == ',') { apply(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) apply(cur);
    }
    // Dante transport: UDP virtual network between processes, or the in-process loopback
    std::string selfName = !netName.empty() ? netName : (m.dante ? std::string(m.dante->deviceName) : "Rio3224-D2");
    uint32_t latencyFrames = (uint32_t)std::lround(latencyMs * engine.sampleRate() / 1000.0);
    if (!txPath.empty()) {
        txTap = std::make_unique<WavWriter>();
        if (!txTap->open(txPath, kDanteTx, engine.sampleRate(), 32)) { fprintf(stderr, "[audio] cannot create %s\n", txPath.c_str()); return false; }
    }
    auto tap = [this](const float* const* tx, int nch, size_t frames) { if (txTap) txTap->write(tx, nch, frames); };
    if (!ctlSpec.empty()) {
        // Controller-interoperable endpoint on the real network: identity comes from the module model (what the Rio host
        // programmed: device label, model id, manufacturer, MAC) so the Yamaha side and the network side agree
        DanteEndpointConfig c; std::string spec = ctlSpec, err; bool macOverride = false;
        size_t semi = spec.find(';'); c.ifAddr = spec.substr(0, semi); std::string rest = semi == std::string::npos ? "" : spec.substr(semi + 1);
        std::string cur; auto kv = [&](const std::string& item) {
            size_t e = item.find('='); std::string k = item.substr(0, e), v = e == std::string::npos ? "1" : item.substr(e + 1);
            if (k == "sec") c.ifAddr2 = v; else if (k == "trace") c.tracePath = v; else if (k == "latency") c.rxLatencyNs = (uint32_t)(atof(v.c_str()) * 1e6); else if (k == "prefer") c.preferredMaster = atoi(v.c_str()) != 0;
            else if (k == "persist") c.persistPath = v; else if (k == "verbose") endpointVerbose = atoi(v.c_str()) != 0; else if (k == "slaveonly") c.slaveOnly = atoi(v.c_str()) != 0; else if (k == "sync") c.ptpLogSyncInterval = (int8_t)atoi(v.c_str()); else if (k == "stratum") c.ptpStratum = (uint8_t)atoi(v.c_str());
            else if (k == "ports") { unsigned a, b2, i2, f2; if (sscanf(v.c_str(), "%u,%u,%u,%u", &a, &b2, &i2, &f2) == 4) { c.arcPort = (uint16_t)a; c.cmcPort = (uint16_t)b2; c.infoPort = (uint16_t)i2; c.flowsPort = (uint16_t)f2; c.allowAltPorts = false; } }
            else if (k == "name") c.name = v; else if (k == "ahead") c.txAheadNs = (int64_t)(atof(v.c_str()) * 1e6); else if (k == "board") c.boardName = v; else if (k == "mf") c.manufacturer = v; else if (k == "model") c.modelName = v; else if (k == "modelid") c.modelId = v;
            else if (k == "mac") {                                                             // module MAC (a second instance on the same host needs its own identity)
                unsigned b[6]; if (sscanf(v.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 || sscanf(v.c_str(), "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) { for (int i = 0; i < 6; i++) c.mac[i] = (uint8_t)b[i]; macOverride = true; }
            }
            else fprintf(stderr, "[dante-net] unknown --dante-ctl key '%s'\n", k.c_str());
        };
        for (char ch : rest) { if (ch == ';') { if (!cur.empty()) kv(cur); cur.clear(); } else cur += ch; } if (!cur.empty()) kv(cur);
        if (macOverride && m.dante) { bool wasDefault = m.dante->deviceName == m.dante->configuredName(1); memcpy(m.dante->mac, c.mac, 6); if (wasDefault) m.dante->deviceName = m.dante->configuredName(1); if (netName.empty()) selfName = m.dante->deviceName; }
        if (c.name.empty()) c.name = !netName.empty() ? netName : selfName;
        if (m.dante) {
            c.factoryName = m.dante->freshName(); memcpy(c.mac, m.dante->mac, 6); c.modelName = m.dante->modelName;
            bool anyId = false; for (int i = 0; i < 8; i++) if (m.dante->modelId[i]) anyId = true;
            if (anyId) { char b[24]; snprintf(b, sizeof b, "_%02x%02x%02x%02x%02x%02x%02x%02x", m.dante->modelId[0], m.dante->modelId[1], m.dante->modelId[2], m.dante->modelId[3], m.dante->modelId[4], m.dante->modelId[5], m.dante->modelId[6], m.dante->modelId[7]); c.modelId = b; }
            c.fwVersion[0] = m.dante->audFirm[0]; c.fwVersion[1] = m.dante->audFirm[1]; c.fwVersion[2] = 0; c.fwVersion[3] = m.dante->audFirm[2]; c.fwPatch = m.dante->audFirm[3]; c.productVersion[0] = (uint8_t)(m.dante->modelVer >> 24); c.productVersion[1] = (uint8_t)(m.dante->modelVer >> 16); c.productVersion[2] = (uint8_t)(m.dante->modelVer >> 8); c.productVersion[3] = (uint8_t)m.dante->modelVer;
            c.preferredMaster = c.preferredMaster || m.dante->preferredMaster; c.redundant = m.dante->swMode != 0;
            auto dflt = [](int i) { char b[8]; snprintf(b, sizeof b, "%02d", i + 1); return std::string(b); };
            for (int i = 0; i < kDanteTx; i++) { std::string l = m.dante->channelLabel(0, (uint16_t)(i + 1)); c.txLabels.push_back(l == dflt(i) ? "" : l); }
            for (int i = 0; i < kDanteRx; i++) { std::string l = m.dante->channelLabel(1, (uint16_t)(i + 1)); c.rxLabels.push_back(l == dflt(i) ? "" : l); }
        } else { c.factoryName = "Rio3224-D2-000000"; }
        // device id: EUI-64 of the module's MAC, like Dante hardware derives it
        c.deviceId[0] = c.mac[0]; c.deviceId[1] = c.mac[1]; c.deviceId[2] = c.mac[2]; c.deviceId[3] = 0xff; c.deviceId[4] = 0xfe; c.deviceId[5] = c.mac[3]; c.deviceId[6] = c.mac[4]; c.deviceId[7] = c.mac[5];
        c.fs = engine.sampleRate(); if (!c.rxLatencyNs) c.rxLatencyNs = 5000000;
        if (latencyMs != 1.0) c.rxLatencyNs = (uint32_t)(latencyMs * 1e6);                 // explicit --dante-latency-ms wins
        if (c.persistPath.empty()) c.persistPath = "dante-endpoint.cfg";
        endpoint = std::make_unique<DanteEndpoint>(); endpoint->setLog(endpointVerbose);
        if (txTap) endpoint->txTap = tap;
        if (!endpoint->start(c, err)) { fprintf(stderr, "[dante-net] %s\n", err.c_str()); return false; }
        if (!patchSpec.empty()) {                                                             // "rx@tx" (own channels) / "rx@Device:tx"
            std::string cur2; auto apply = [&](const std::string& p) { size_t at = p.find('@'); if (at == std::string::npos) return; int rx = atoi(p.substr(0, at).c_str()); std::string r2 = p.substr(at + 1); size_t col = r2.find(':'); std::string dev = col == std::string::npos ? c.name : r2.substr(0, col); int tx = atoi((col == std::string::npos ? r2 : r2.substr(col + 1)).c_str()); if (rx >= 1 && rx <= kDanteRx && tx >= 1) endpoint->subscribe(rx - 1, dev, tx - 1); };
            for (char ch : patchSpec) { if (ch == ',') { apply(cur2); cur2.clear(); } else cur2 += ch; } if (!cur2.empty()) apply(cur2);
        }
        transport = endpoint.get();
        if (m.dante) { auto snap = endpoint->snapshot(); if (snap.name != m.dante->deviceName) { fprintf(stderr, "[dante-net] module keeps the persisted device name '%s'\n", snap.name.c_str()); m.dante->deviceName = snap.name; } bridgeName = snap.name; }
        fprintf(stderr, "[dante-net] %s\n", endpoint->describe().c_str());
    } else if (!netPrimary.empty()) {
        vnet = std::make_unique<VirtualDanteTransport>(selfName); std::string err;
        if (!vnet->open(netPrimary == "on" ? "" : netPrimary, netSecondary, err)) { fprintf(stderr, "[dante] %s\n", err.c_str()); return false; }
        vnet->setFormat(engine.sampleRate(), 0); vnet->setLatencyFrames(latencyFrames); vnet->dropPercent = dropPercent; vnet->setClockLeader(netClockLeader);
        if (!patchSpec.empty()) vnet->patch(patchSpec);
        if (txTap) vnet->txTap = tap;
        transport = vnet.get();
        // console HA-remote frames from the virtual network reach the host on UART B (and the host's UART B frames go back)
        vnet->onHaRemoteFrame = [this](const uint8_t* f, size_t n) {
            std::vector<uint8_t> b; if (n && f[0] != n - 1) b.push_back((uint8_t)n);   // the UART carries a length byte in front of every frame
            b.insert(b.end(), f, f + n); m.mainFpga->moduleToHost(1, b.data(), b.size());
            fprintf(stderr, "[dante] HA remote frame from the network: %zu bytes -> UART B\n", n);
        };
        m.mainFpga->onHostTxB = [this](uint8_t byte) {
            haTx.push_back(byte);
            if (haTx.size() >= 1 && haTx.size() == (size_t)haTx[0] + 1) { vnet->sendHaRemote(haTx.data(), haTx.size()); haTx.clear(); }
            else if (haTx.size() > 300) haTx.clear();
        };
        fprintf(stderr, "[dante] %s\n", vnet->describe().c_str());
    } else {
        loop = std::make_unique<LoopbackTransport>(selfName);
        loop->setFormat(engine.sampleRate(), 0); loop->setLatencyFrames(latencyFrames); loop->dropPercent = dropPercent;
        if (!patchSpec.empty()) loop->patchSelf(patchSpec);
        if (txTap) loop->txTap = tap;
        transport = loop.get();
    }
    engine.setTransport(transport);
    m.dspIf->meterRead = [this](int idx) { return meterRead(idx); };
    fprintf(stderr, "[audio] engine on: %u Hz, backend %s, transport %s, latency %.2f ms, host in/out full scale %+.1f/%+.1f dBu\n",
            engine.sampleRate(), backend->name(), transport->name(), latencyMs, hostInDbu, hostOutDbu);
    if (endpoint) { rtQuit = false; rtThread = std::thread([this] { rtLoop(); }); fprintf(stderr, "[audio] engine clocked by the Dante media clock on a real-time thread\n"); }
    return true;
}

void AudioGlue::buildSnapshot(HwSnapshot& s) const {
    s.fs = engine.sampleRate();
    s.clockLocked = m.mainFpga->src[8].present;        // Dante word clock present -> DIO locks -> DSP alive
    s.outputMute = !s.clockLocked;
    s.phantomMaster = true;                             // the firmware already gates the HA phantom bit with the master switch
    for (int c = 0; c < kInputs; c++) {
        InputHw& p = s.in[c];
        int fpga = c >> 3, cmd = 7 - (c & 7);
        uint8_t v = m.haLink.reg[fpga][cmd];
        p.present = m.haLink.seen[fpga];
        p.gainIndex = (uint8_t)(v >> 1); p.phantomBit = (v & 1) != 0;
        int dev = (c & 8) ? 8 : 7, off = (c >= 16) ? 0xA8 : 0, k = c & 7;
        auto it = m.dspIf->regs.find(dev);
        auto rd = [&](uint32_t reg, uint32_t dflt) -> uint32_t {
            if (it == m.dspIf->regs.end()) return dflt;
            auto r = it->second.find(reg); return r == it->second.end() ? dflt : r->second;
        };
        p.gain1 = q30ToFloat(rd(0x4A + off + k, 0x40000000));
        p.gain2 = q30ToFloat(rd(0x52 + off + k, 0x40000000));
        p.gain3 = q30ToFloat(rd(0x5A + off + k, 0x40000000));
        uint32_t blkA = 0x0A + off + 6 * k, blkH = (k < 4 ? 0x62 + 6 * k : 0x8A + 6 * (k - 4)) + off;
        uint32_t pairA = 0x3A + off + 2 * k, pairH = (k < 4 ? 0x7A + 2 * k : 0xA2 + 2 * (k - 4)) + off;
        static const uint32_t passthru[5] = { 0, 0, 0x40000000, 0, 0 };
        for (int i = 0; i < 5; i++) { p.filtA[i] = q30ToFloat(rd(blkA + 1 + i, passthru[i])); p.hpf[i] = q30ToFloat(rd(blkH + 1 + i, passthru[i])); }
        p.filtADry = q30ToFloat(rd(pairA, 0x40000000)); p.filtAWet = q30ToFloat(rd(pairA + 1, 0));
        p.hpfDry = q30ToFloat(rd(pairH, 0x40000000));  p.hpfWet = q30ToFloat(rd(pairH + 1, 0));
        // gain compensation lives in the firmware's parameter tree (the real DSP gets the HA gain over the head-amp link
        // and the GC settings from the host; this build of the host firmware does not expose them in the register writes)
        p.gcOn = m.bus.read32(paramGcOn + paramStride * c) != 0;
        p.gcStartGainDb = (float)(int32_t)m.bus.read32(paramGcStart + paramStride * c);
    }
}

void AudioGlue::tick(uint64_t ms) {
    if (!enabled || ms <= lastMs) return;
    buildSnapshot(snap); snap.serial++;
    engine.publishHw(snap);
    // consistency check once: the firmware's own gain parameter must agree with the analog index it sent to the head amps
    if (!warnedGain && snap.in[0].present) {
        int32_t g = (int32_t)m.bus.read32(paramGain); int idx = snap.in[0].gainIndex;
        if (g != idx - 6 && m.haLink.frames > 0) { fprintf(stderr, "[audio] note: ch1 parameter gain %d dB vs head-amp index %d (%d dB) - transient or param address mismatch\n", g, idx, idx - 6); }
        warnedGain = true;
    }
    if (vnet) for (auto& f : netFails) if (!f.done && ms >= f.ms) { f.done = true; vnet->failNetwork(f.secondary, f.failed); fprintf(stderr, "[dante] %llu ms: %s network %s\n", (unsigned long long)ms, f.secondary ? "secondary" : "primary", f.failed ? "cable pulled" : "cable plugged back"); }
    if (endpoint) for (auto& f : netFails) if (!f.done && ms >= f.ms) { f.done = true; endpoint->failLink(f.secondary, f.failed); }
    if (endpoint && ms >= nextBridgeMs) { nextBridgeMs = ms + 200; bridgeTick(ms); }
    if (vnet && m.dante && ms >= nextPeerMs) {                 // the module model's device list follows the virtual network
        nextPeerMs = ms + 1000; std::vector<std::string> names; for (auto& [n, p] : vnet->peerList()) names.push_back(n);
        if (names != m.dante->peers) { m.dante->peers = names; fprintf(stderr, "[dante] %llu ms: %zu other device(s) on the network\n", (unsigned long long)ms, names.size()); }
    }
    if (rtThread.joinable()) { lastMs = ms; frameAcc = 0; }
    else {
        frameAcc += (double)(ms - lastMs) * engine.sampleRate() / 1000.0; lastMs = ms;
        while (frameAcc >= 1.0) {
            size_t n = (size_t)frameAcc; if (n > (size_t)kMaxBlock) n = kMaxBlock;
            engine.processBlock(n); frameAcc -= (double)n;
        }
    }
    if (traceInput >= 1 && traceInput <= kInputs && ms >= nextTraceMs) { nextTraceMs = ms + traceMs; fprintf(stderr, "[audio] %7llu ms %s\n", (unsigned long long)ms, engine.describeInput(traceInput - 1).c_str()); }
    if (meterPrintMs > 0 && ms >= nextMeterMs) {
        nextMeterMs = ms + meterPrintMs;
        fprintf(stderr, "[audio] %7llu ms meters in:", (unsigned long long)ms);
        for (int c = 0; c < kInputs; c++) { auto mt = engine.inputMeter(c); fprintf(stderr, " %5.1f%s%s", mt.peakDbfs, mt.peak ? "P" : "", mt.adcClip ? "!" : ""); }
        fprintf(stderr, " | out:"); for (int o = 0; o < kAnalogOuts + kAesOuts; o++) fprintf(stderr, " %5.1f", engine.outputMeter(o).peakDbfs);
        fprintf(stderr, "\n");
    }
}

void AudioGlue::shutdown() {
    if (!enabled) return;
    if (rtThread.joinable()) { rtQuit = true; rtThread.join(); fprintf(stderr, "[audio] media-clock engine thread: %llu blocks, %llu stalls\n", (unsigned long long)rtBlocks, (unsigned long long)rtStalls); }
    if (txTap) txTap->close();
    if (auto* f = dynamic_cast<FileBackend*>(backend.get())) f->close();
    if (auto* w = dynamic_cast<WasapiBackend*>(backend.get())) {
        for (auto& s : w->inputStats())
            fprintf(stderr, "[audio] host in  %s%s (host %d-%d): %llu frames, %llu engine underruns, %llu device drops, buffer %.1f ms, drift servo %+.4f%%\n",
                    s.loopback ? "loopback " : "", s.name.c_str(), s.firstHostChannel + 1, s.firstHostChannel + s.channels, (unsigned long long)s.frames, (unsigned long long)s.engineStarved, (unsigned long long)s.deviceGlitches, s.fillMs, (s.ratioScale - 1) * 100);
        if (auto s = w->outputStats(); s.channels)
            fprintf(stderr, "[audio] host out %s: %llu frames, %llu overruns, %llu device glitches (silence inserted), buffer %.1f ms, drift servo %+.4f%%\n",
                    s.name.c_str(), (unsigned long long)s.frames, (unsigned long long)s.engineStarved, (unsigned long long)s.deviceGlitches, s.fillMs, (s.ratioScale - 1) * 100);
        w->close();
    }
    const auto& st = transport->status();
    fprintf(stderr, "[audio] processed %llu frames (%.2f s), Dante tx blocks %llu rx blocks %llu lost %llu\n",
            (unsigned long long)engine.framesProcessed(), engine.framesProcessed() / (double)engine.sampleRate(),
            (unsigned long long)st.txBlocks, (unsigned long long)st.rxBlocks, (unsigned long long)st.rxLost);
    if (vnet) fprintf(stderr, "[dante] %s\n", vnet->statsText().c_str());
    if (endpoint) { auto sn = endpoint->snapshot(); fprintf(stderr, "[dante-net] controller requests %llu, audio packets sent %llu / received %llu (late %llu), re-anchors %llu, errors %llu\n", (unsigned long long)sn.controllerQueries, (unsigned long long)sn.txPackets, (unsigned long long)sn.rxPackets, (unsigned long long)sn.rxLate, (unsigned long long)sn.resyncs, (unsigned long long)sn.errors); endpoint->stop(); }
    for (int c = 0; c < kInputs; c++) if (engine.inputMapOf(c) >= 0 && engine.trace(c).hostPeakDbfs > -150.f) fprintf(stderr, "[audio] %s\n", engine.describeInput(c).c_str());
}

// Layer A (module model seen by the Rio host) <-> layer B (network endpoint).  The module is authoritative for what the
// host programs (device label, channel labels, sample rate, preferred master, switch mode); the network is authoritative
// for what happens on the wire (link, clock role and lock, other devices, controller-initiated renames / subscriptions).
void AudioGlue::bridgeTick(uint64_t ms) {
    if (!endpoint || !m.dante) return;
    auto& d = *m.dante; auto s = endpoint->snapshot();
    // host -> network
    if (d.deviceName != bridgeName && !s.nameChangedByController) { endpoint->setDeviceName(d.deviceName); bridgeName = d.deviceName; }
    if (s.nameChangedByController) { endpoint->clearNameChangedFlag(); bridgeName = s.name; d.networkRenamed(s.name); fprintf(stderr, "[dante-net] %llu ms: device renamed from the controller -> module notifies the host: %s\n", (unsigned long long)ms, s.name.c_str()); }
    if (s.fs != bridgeFs && s.fs != d.fs) {                 // controller changed the rate: module notifies the host, the DIO word clock follows, the engine re-rates
        bridgeFs = s.fs; d.networkFsChanged(s.fs); m.mainFpga->src[8].fs = (int)s.fs; if (rtThread.joinable()) rtPendingFs = s.fs; else engine.setSampleRate(s.fs);
        fprintf(stderr, "[dante-net] %llu ms: sample rate %u Hz from the controller -> module notifies DEVICE_FS, word clock re-rated\n", (unsigned long long)ms, s.fs);
    } else if (d.fs != bridgeFs) { bridgeFs = d.fs; endpoint->setSampleRate(d.fs); m.mainFpga->src[8].fs = (int)d.fs; if (rtThread.joinable()) rtPendingFs = d.fs; else engine.setSampleRate(d.fs); }
    if (s.identify) { endpoint->clearNameChangedFlag(); d.networkIdentify(); fprintf(stderr, "[dante-net] %llu ms: identify request from the controller -> module notifies BROWSE_IDENTIFY\n", (unsigned long long)ms); }
    if (d.preferredMaster != bridgePreferred) { bridgePreferred = d.preferredMaster; endpoint->setPreferredMaster(d.preferredMaster); }
    if ((int)d.swMode != bridgeSw) { bridgeSw = d.swMode; endpoint->setRedundant(d.swMode != 0); }
    bridgeTxLabels.resize(kDanteTx); bridgeRxLabels.resize(kDanteRx);
    for (int i = 0; i < kDanteTx; i++) { std::string host = d.channelLabel(0, (uint16_t)(i + 1)), net = s.txLabels[i]; if (host != bridgeTxLabels[i] && host != net) endpoint->setChannelLabel(true, i, host); else if (net != bridgeTxLabels[i] && net != host) d.networkChannelLabel(0, (uint16_t)(i + 1), net); bridgeTxLabels[i] = d.channelLabel(0, (uint16_t)(i + 1)); }
    for (int i = 0; i < kDanteRx; i++) { std::string host = d.channelLabel(1, (uint16_t)(i + 1)), net = s.rxLabels[i]; if (host != bridgeRxLabels[i] && host != net) endpoint->setChannelLabel(false, i, host); else if (net != bridgeRxLabels[i] && net != host) d.networkChannelLabel(1, (uint16_t)(i + 1), net); bridgeRxLabels[i] = d.channelLabel(1, (uint16_t)(i + 1)); }
    // network -> host
    bool link = s.linkPrimary; if (link != bridgeLink) { bridgeLink = link; d.linkUp = link; d.networkLinkChanged(); fprintf(stderr, "[dante-net] %llu ms: primary link %s -> module notifies DEVICE_LINK\n", (unsigned long long)ms, link ? "up" : "down"); }
    if (s.clockMaster != bridgeMaster || s.clockLocked != bridgeLocked) { bridgeMaster = s.clockMaster; bridgeLocked = s.clockLocked; d.clockMaster = s.clockMaster; d.networkClockChanged(); fprintf(stderr, "[dante-net] %llu ms: clock %s%s -> module notifies DEVICE_CLOCK_STATUS\n", (unsigned long long)ms, s.clockMaster ? "grandmaster" : ("follower of " + s.masterId).c_str(), s.clockLocked ? ", locked" : ", not locked"); }
    if (s.peers != d.peers) { d.peers = s.peers; d.networkDeviceListChanged(); fprintf(stderr, "[dante-net] %llu ms: %zu other device(s) on the network\n", (unsigned long long)ms, s.peers.size()); }
    // subscriptions -> the module's patch table (device index in the browse list, TX channel id)
    std::map<uint16_t, std::pair<uint16_t, uint16_t>> patches;
    for (int rx = 0; rx < kDanteRx; rx++) { const auto& sb = s.subs[rx]; if (sb.txChannel.empty()) continue; uint16_t dev = 0; if (sb.txHost != s.name) { dev = 0xFFFF; for (size_t k = 0; k < s.peers.size(); k++) if (s.peers[k] == sb.txHost) dev = (uint16_t)(k + 1); } uint16_t ch = (uint16_t)atoi(sb.txChannel.c_str()); patches[(uint16_t)(rx + 1)] = { dev, ch }; }
    if (patches != d.rxPatch) d.rxPatch = patches;
}

// The engine thread: process exactly the samples that are due on the media clock, in blocks of about a millisecond.
void AudioGlue::rtLoop() {
#ifdef _WIN32
    timeBeginPeriod(1); DWORD task = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (!mm) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    while (!rtQuit) {
        if (uint32_t fs = rtPendingFs.exchange(0)) { engine.setSampleRate(fs); rtAnchored = false; }
        uint32_t fs = engine.sampleRate();
        int64_t nowS = DanteEndpoint::ptpSampleCount(endpoint->mediaTimeNs(), fs);
        if (!rtAnchored) { rtBase = nowS - (int64_t)engine.framesProcessed(); rtAnchored = true; }
        int64_t due = nowS - rtBase - (int64_t)engine.framesProcessed();
        if (due > (int64_t)fs / 10 || due < -(int64_t)fs / 10) { rtStalls++; rtBase = nowS - (int64_t)engine.framesProcessed(); due = 0; }   // the thread was held for 100 ms (or the clock stepped): do not burst, skip
        while (due > 0) { int64_t n = due; if (n > (int64_t)fs / 1000 + 1) n = fs / 1000 + 1; if (n > kMaxBlock) n = kMaxBlock; engine.processBlock((size_t)n); due -= n; rtBlocks++; }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
#ifdef _WIN32
    if (mm) AvRevertMmThreadCharacteristics(mm);
    timeEndPeriod(1);
#endif
}
