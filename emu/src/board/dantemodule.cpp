// Behavioural Dante module (see dantemodule.h).  Payload layouts follow dac's dacCmdParamConv* serialisers:
// every parameter starts with the device id (big-endian 16 bit, 0 = the local module) followed by the fields.
#include "dantemodule.h"
#include <cstring>
#include <algorithm>
#include <map>

using namespace bklyn;

enum Cmd : uint8_t {
    BASE_INFO = 0, BASE_RESTART = 1, BASE_MODE = 2, BASE_CAPABILITY = 3, BASE_MANUFACTURER = 4, BASE_RESOURCE = 5,
    BASE_LICENSE = 7, BASE_KEEPALIVE = 8, BASE_FORCE_INIT = 9, BASE_LOCK = 10, BASE_AES67_ENABLE = 11,
    DEVICE_LATENCY = 16, DEVICE_FS = 17, DEVICE_BIT_DEPTH = 18, DEVICE_PREFERRED = 19, DEVICE_SLAVE_TO_EXT = 20,
    DEVICE_MONITOR = 21, DEVICE_CLOCK_MASTER = 22, DEVICE_CLOCK_SYNC = 23, DEVICE_LINK = 24, DEVICE_CLOCK_STATUS = 25,
    BROWSE_DEVICE_LIST = 32, BROWSE_DEVICE_LABEL = 33, BROWSE_CHANNEL_LABEL = 34, BROWSE_CHANNEL_LABELS = 35,
    BROWSE_RESTART = 36, BROWSE_RESTART_ALL = 37, BROWSE_COMPLETE = 38, BROWSE_COMPLETE_ALL = 39, BROWSE_IDENTIFY = 40,
    BROWSE_DIV_DEVICE_LIST = 44, BROWSE_START_BROWSING = 45, BROWSE_REBUILD_DEV_LIST = 46,
    ROUTING_PATCH_ON = 48, ROUTING_PATCHES = 49, ROUTING_PATCH_OFF = 50, ROUTING_AUDIO_RECV_DEVICE = 52, ROUTING_FLOW_USAGE = 53,
    NETWORK_IP_ADDRESS = 64, NETWORK_MAC_ADDRESS = 65, NETWORK_MDNS_SERVICE = 66, NETWORK_DNS_SD = 67, NETWORK_DNS_SPEC = 68,
    NETWORK_SWMODE = 69, NETWORK_ACCESS = 70, CONNECT_AUDIO_TX = 80, CONNECT_AUDIO_RX = 81, CONNECT_DEVICE = 82,
    L2SW_WRITE = 96, L2SW_READ = 97, GPIO = 98, DDM_CONNECT_STATE = 104, DDM_DOMAIN = 105, DDM_CONTROL_POLICY = 106,
    HAREMOTE_BRIDGE_MODE = 144, CMD_ERROR = 240, DACLOG = 241, DACEXIT = 242, CPU_LOADAVG = 243, FREEMEM = 244,
    FREEMEM_THRESHOLD = 245, TMP_TEXT = 246, SYSLOG = 247
};

static void be16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
static void be32(std::vector<uint8_t>& v, uint32_t x) { be16(v, x >> 16); be16(v, x & 0xFFFF); }
static void bytes(std::vector<uint8_t>& v, const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; v.insert(v.end(), b, b + n); }
static void fixedStr(std::vector<uint8_t>& v, const std::string& s, size_t n) { for (size_t i = 0; i < n; i++) v.push_back(i < s.size() ? (uint8_t)s[i] : 0); }
// dac's fs2Type: sample rate in the high nibble (44.1k=0x00, 48k=0x10, 88.2k=0x20, 96k=0x30, 176.4k=0x40, 192k=0x50), pull-up code in the low nibble
static uint8_t fsType(uint32_t fs) { switch (fs) { case 44100: return 0x00; case 48000: return 0x10; case 88200: return 0x20; case 96000: return 0x30; case 176400: return 0x40; case 192000: return 0x50; default: return 0x10; } }

DanteModule::DanteModule() {
    dec.onFrame = [this](const Frame& f) { onFrame(f); };
    dec.onError = [this](const char* why) { if (log) fprintf(stderr, "[DANTE] bad frame from host: %s\n", why); };
}

void DanteModule::send(uint8_t category, uint8_t cmd, uint8_t seqNo, uint8_t result, uint8_t origin, const std::vector<uint8_t>& payload, int delayMs) {
    Header h; h.category = category; h.cmd = cmd; h.ver = 1; h.seq = seqNo; h.result = result; h.origin = origin;
    auto bytesOut = encode(h, payload.data(), payload.size());
    framesOut++;
    if (delayMs < 0) delayMs = respDelayMs;
    if (log) {
        fprintf(stderr, "[DANTE] %7llu ms -> cat %02X cmd %3u seq %3u res %u len %zu:", (unsigned long long)nowMs, category, cmd, seqNo, result, payload.size());
        for (size_t i = 0; i < payload.size() && i < 160; i++) fprintf(stderr, " %02X", payload[i]);
        fprintf(stderr, "%s\n", payload.size() > 160 ? " ..." : "");
    }
    if (delayMs > 0 || !outQ.empty()) { outQ.push_back({ nowMs + (uint64_t)delayMs, std::move(bytesOut) }); return; }
    if (toHost) toHost(bytesOut.data(), bytesOut.size());
}

void DanteModule::flushOut() {
    while (!outQ.empty() && outQ.front().atMs <= nowMs) {
        if (toHost) toHost(outQ.front().bytes.data(), outQ.front().bytes.size());
        outQ.pop_front();
    }
}

// ---- payloads -----------------------------------------------------------------------------------
std::vector<uint8_t> DanteModule::baseInfo() const {       // 28 bytes
    std::vector<uint8_t> v; be16(v, 0);
    v.push_back(3);                                        // hard_id: board type derived from the model id (Brooklyn III)
    bytes(v, audFirm, 4); bytes(v, audSoft, 4); bytes(v, ymhFirm, 4);
    v.push_back(1);                                        // protocol version
    be16(v, txChannels); be16(v, rxChannels);
    bytes(v, modelId, 8);
    return v;
}
std::vector<uint8_t> DanteModule::dacMode() const {        // 4 bytes: dev, mode code | flags, error
    std::vector<uint8_t> v; be16(v, 0); v.push_back(mode); v.push_back(0); return v;
}
std::vector<uint8_t> DanteModule::capability() const {     // 18 bytes
    std::vector<uint8_t> v; be16(v, 0);
    be32(v, 0x0000FFFF);                                   // capability flags
    be16(v, 0x000F);                                       // srate_avail: 44.1/48/88.2/96
    be16(v, 0x001F);                                       // pullup_avail
    be16(v, 0x0007);                                       // enc_avail: 16/24/32
    be16(v, 32); be16(v, 32);                              // max tx/rx flows
    v.push_back(1); v.push_back(1);                        // can_preferred, can_rename
    return v;
}
std::vector<uint8_t> DanteModule::manufacturer() const {   // dev + DAC_PARAM_BASEMANF (128)
    std::vector<uint8_t> v; be16(v, 0);
    bytes(v, manfId, 8); fixedStr(v, "Yamaha Corporation", 32); bytes(v, modelId, 8); fixedStr(v, modelName, 32);
    be32(v, modelVer);                                     // model_ver
    const uint8_t soft[8] = { 4, 2, 5, 3, 0, 0, 0, 0 }, firm[8] = { 4, 2, 5, 3, 0, 0, 0, 0 };
    bytes(v, soft, 8); bytes(v, firm, 8);
    be32(v, manfCaps);
    const uint8_t serial[8] = { 0, 0x1D, 0xC1, 0x12, 0x34, 0x56, 0, 1 };
    bytes(v, serial, 8); fixedStr(v, modelVerStr, 16);
    return v;
}
std::vector<uint8_t> DanteModule::license() const {        // 7 bytes
    std::vector<uint8_t> v; be16(v, 0); be16(v, txChannels); be16(v, rxChannels); v.push_back(1); return v;
}
std::vector<uint8_t> DanteModule::resource() const {       // dev + 8 x u16
    std::vector<uint8_t> v; be16(v, 0);
    be16(v, 128); be16(v, 1); be16(v, 512); be16(v, txChannels); be16(v, 512); be16(v, rxChannels); be16(v, 128); be16(v, 0);
    return v;
}
std::vector<uint8_t> DanteModule::fsParam() const {        // dev, current fs type, pending fs type
    std::vector<uint8_t> v; be16(v, 0); v.push_back(fsType(fs)); v.push_back(fsType(fs)); return v;
}
std::vector<uint8_t> DanteModule::clockSync() const {      // dev, locked, external
    std::vector<uint8_t> v; be16(v, 0); v.push_back(1); v.push_back(slaveToExt ? 1 : 0); return v;
}
std::vector<uint8_t> DanteModule::clockStatus() const {    // 22 bytes, dacCmdParamConvClockStatus(DAC_PARAM_CLOCK_STATUS)
    std::vector<uint8_t> v; be16(v, 0);
    be32(v, 0);                                            // drift (ppm)
    be16(v, 0);                                            // mute_flags
    v.push_back(slaveToExt ? 1 : 0);                       // source != internal
    v.push_back(0);                                        // ext_state
    v.push_back(3);                                        // clock state (CONMON_AUDINATE_CLOCK_STATE_LOCKED)
    v.push_back(2);                                        // servo state (locked)
    v.push_back(1); v.push_back(0);                        // port[0] (primary: slave/master), port[1] (secondary: none)
    v.push_back(0);                                        // stratum
    v.push_back(clockMaster ? 1 : 0);                      // is_grandmaster
    be16(v, 0x0003);                                       // clock capabilities
    v.push_back(0); v.push_back(0);                        // aes67 port states
    v.push_back(0); v.push_back(0);                        // ddm port states
    return v;
}
std::vector<uint8_t> DanteModule::linkSpeed() const {      // dev, primary code, secondary code (2 = 100M, 3 = 1G)
    std::vector<uint8_t> v; be16(v, 0); v.push_back(linkUp ? 3 : 0); v.push_back(0); return v;
}
std::vector<uint8_t> DanteModule::ipAddress() const {      // 18 bytes
    std::vector<uint8_t> v; be16(v, 0); v.push_back(0); v.push_back(0);   // interface, type (current)
    v.push_back(1);                                        // ip_type: link-local / dhcp
    be32(v, ip); v.push_back((uint8_t)netmask); be32(v, gateway); be32(v, 0);
    return v;
}
std::vector<uint8_t> DanteModule::macAddress() const {     // 9 bytes: dev, mac[6], interface (dacCmdParamConvMacAddress)
    std::vector<uint8_t> v; be16(v, 0); bytes(v, mac, 6); v.push_back(0); return v;
}
std::string DanteModule::freshName() const {                // Brooklyn factory default: model name + last three MAC bytes
    char b[40]; snprintf(b, sizeof b, "Rio3224-D2-%02x%02x%02x", mac[3], mac[4], mac[5]); return b;
}
std::string DanteModule::configuredName(int unitId) const { // what the Rio host programs: "Y0" + unit id (2 hex digits) + "-" + last 12 chars of the current name
    std::string fresh = freshName(); char b[8]; snprintf(b, sizeof b, "Y0%02X-", unitId & 0xFF);
    return std::string(b) + fresh.substr(fresh.size() - 12);
}
std::vector<uint8_t> DanteModule::deviceLabel() const {    // dev + 32 chars
    std::vector<uint8_t> v; be16(v, 0); fixedStr(v, deviceName, 32); return v;
}

std::string DanteModule::channelLabel(int txrx, uint16_t ch) const {
    auto it = chLabel[txrx & 1].find(ch);
    if (it != chLabel[txrx & 1].end()) return it->second;
    char b[8]; snprintf(b, sizeof b, "%02u", (unsigned)ch); return b;   // Dante factory default channel names
}

// ---- behaviour ----------------------------------------------------------------------------------
void DanteModule::pumpNotify() {
    if (notifyBusy || notifyQ.empty()) return;
    auto& n = notifyQ.front();
    n.seqNo = seq++ & 0x7F; n.sentMs = nowMs; n.tries++;
    notifyBusy = true;
    send(NOTIFY, n.cmd, n.seqNo, 0, 0, n.payload);
}

void DanteModule::tick(uint64_t ms) {
    nowMs = ms;
    flushOut();
    if (!running && !startAtMs && !downAtMs && ms < (uint64_t)bootMs) startAtMs = bootMs;   // first boot
    if (downAtMs && ms >= downAtMs) {                                   // SET BASE_RESTART: module goes away
        downAtMs = 0; running = false; notifyQ.clear(); notifyBusy = false; outQ.clear(); browseCompleteAtMs = renameDoneAtMs = manfNotifyAtMs = 0;
        startAtMs = ms + rebootMs;
        if (log) fprintf(stderr, "[DANTE] module down (reboot) at %llu ms, back in %d ms%s", (unsigned long long)ms, rebootMs, "" + 0);
        if (log) fputc(10, stderr);
    }
    if (startAtMs && ms >= startAtMs) {
        startAtMs = 0; running = true; seq = 0; announceAtMs = ms + announceDelayMs; swMode = swModeConfigured;
        if (log) { fprintf(stderr, "[DANTE] module up at %llu ms", (unsigned long long)ms); fputc(10, stderr); }
    }
    if (running && announceAtMs && ms >= announceAtMs) {                // dac announces itself a few seconds after the GPIO heartbeat starts
        announceAtMs = 0;
        notify(BASE_INFO, baseInfo());                     // dacCmdSendStartNotify
        { std::vector<uint8_t> v; be16(v, 0); v.push_back(1); v.push_back(0); notify(CONNECT_DEVICE, v); }   // dacDeviceNotifyDeviceConnect
        notify(BROWSE_COMPLETE, browseComplete());
        notify(BASE_MODE, dacMode());                      // dacModeNotify: STARTUP -> MAIN
        { std::vector<uint8_t> v; be16(v, 0); v.push_back(0); v.push_back(swMode); notify(NETWORK_SWMODE, v); }   // updateConMonChange: the switch mode in
    }                                                      // effect once ConMon reports the interface; without it the host's settings sync sees "unknown" and rewrites it (+ module restart)
    if (notifyBusy && !notifyQ.empty() && ms >= notifyQ.front().sentMs + (uint64_t)ackTimeoutMs) {   // no ack from the host
        auto& n = notifyQ.front();
        notifyTimeouts++;
        if (log) fprintf(stderr, "[DANTE] notify cmd %u seq %u not acknowledged (try %d)\n", n.cmd, n.seqNo, n.tries);
        if (n.tries >= notifyRetries) notifyQ.pop_front();
        notifyBusy = false;
        pumpNotify();
    }
    if (browseCompleteAtMs && ms >= browseCompleteAtMs) { browseCompleteAtMs = 0; notify(BROWSE_COMPLETE, browseComplete()); }
    if (renameDoneAtMs && ms >= renameDoneAtMs) {            // dacWatchRenameProcTask: name applied -> notifyRename, restart monitoring, browse again
        renameDoneAtMs = 0;
        notify(BROWSE_DEVICE_LABEL, deviceLabel());
        { std::vector<uint8_t> v; be16(v, 0); notify(BROWSE_RESTART, v); }   // notifyRestart
        browseCompleteAtMs = ms + 600;
    }
    if (manfNotifyAtMs && ms >= manfNotifyAtMs) { manfNotifyAtMs = 0; notify(BASE_MANUFACTURER, manufacturer()); }   // updateNotifyManf -> dacCmdSendManfNotify
}

std::vector<uint8_t> DanteModule::browseComplete() const { // dev, state code, flags (local|network|all complete), changed
    std::vector<uint8_t> v; be16(v, 0); v.push_back(browseCode); v.push_back(browseFlags); v.push_back(browseChanged); return v;
}

void DanteModule::onFrame(const Frame& f) {
    if (log) {
        fprintf(stderr, "[DANTE] %7llu ms <- cat %02X cmd %3u seq %3u res %u origin %u len %zu:", (unsigned long long)nowMs, f.h.category, f.h.cmd, f.h.seq, f.h.result, f.h.origin, f.payload.size());
        for (size_t i = 0; i < f.payload.size() && i < 160; i++) fprintf(stderr, " %02X", f.payload[i]);
        fprintf(stderr, "%s\n", f.payload.size() > 160 ? " ..." : "");
    }
    if (f.h.category & RES_BIT) {                          // the host acknowledging one of our notifications
        if (notifyBusy && !notifyQ.empty() && (f.h.category & 0x7F) == NOTIFY && f.h.cmd == notifyQ.front().cmd && f.h.seq == notifyQ.front().seqNo) {
            notifyQ.pop_front(); notifyBusy = false; pumpNotify();
        }
        return;
    }
    if (f.h.category == GET) {
        switch (f.h.cmd) {
        case BASE_INFO:          respond(f, 0, baseInfo()); return;
        case BASE_MODE:          respond(f, 0, dacMode()); return;
        case BASE_CAPABILITY:    respond(f, 0, capability()); return;
        case BASE_MANUFACTURER:  respond(f, 0, manufacturer()); return;
        case BASE_RESOURCE:      respond(f, 0, resource()); return;
        case BASE_LICENSE:       respond(f, 0, license()); return;
        case BASE_LOCK:          { std::vector<uint8_t> v; be16(v, 0); v.push_back(0); respond(f, 0, v); return; }
        case BASE_AES67_ENABLE:  { std::vector<uint8_t> v; be16(v, 0); v.push_back(0); v.push_back(0); respond(f, 0, v); return; }   // enabled, state (size 4)
        case DEVICE_MONITOR:     { std::vector<uint8_t> v; be16(v, 0); v.push_back(0); respond(f, 0, v); return; }
        case NETWORK_SWMODE: {                                 // dacCmdParamConvSwMode: dev, element, value. element 0 = mode in effect, 1 = configured
            uint8_t el = f.payload.size() >= 3 ? f.payload[2] : 0;   // (takes effect after a restart; the host asks the module to restart after writing it)
            std::vector<uint8_t> v; be16(v, 0); v.push_back(el); v.push_back(el ? swModeConfigured : swMode); respond(f, 0, v); return;
        }
        case DDM_DOMAIN:         { std::vector<uint8_t> v; be16(v, 0); for (int i = 0; i < 16; i++) v.push_back(0); respond(f, 0, v); return; }   // domain UUID (none)
        case DDM_CONTROL_POLICY: { std::vector<uint8_t> v; be16(v, 0); v.push_back(0); v.push_back(0); respond(f, 0, v); return; }   // local, remote access
        case BROWSE_DIV_DEVICE_LIST: {                         // get_BROWSE_DIV_DEVICE_LIST returns 2 ("completed by notify", empty response) and starts
            respond(f, 2, {});                                 // dacDeviceStartDeviceListSequentialNotify: one NOTIFY per device (dev, total, index, id,
            // connect type, label[32]), or just (dev, total 0). The host's conversion table only knows the NOTIFY form
            // (0x218; index is 1-based, index == total ends the list); a direct GET response is "BCA_CALLBACK UNSUPPORTED".
            // The list is this device followed by the other devices on the (virtual) network.
            std::vector<std::string> names; if (listSelf) names.push_back(deviceName); for (auto& p : peers) names.push_back(p);
            if (names.empty()) { std::vector<uint8_t> v; be16(v, 0); be16(v, 0); notify(BROWSE_DIV_DEVICE_LIST, v); return; }
            for (size_t i = 0; i < names.size(); i++) {
                std::vector<uint8_t> v; be16(v, 0); be16(v, (uint16_t)names.size()); be16(v, (uint16_t)(i + 1)); be16(v, (uint16_t)i); v.push_back(1); fixedStr(v, names[i], 32);
                notify(BROWSE_DIV_DEVICE_LIST, v);
            }
            return;
        }
        case L2SW_READ:          { std::vector<uint8_t> v; be16(v, 0); uint32_t key = f.payload.size() >= 5 ? ((uint32_t)f.payload[2] << 16) | ((uint32_t)f.payload[3] << 8) | f.payload[4] : 0;
                                   v.push_back(key >> 16); v.push_back((key >> 8) & 0xFF); v.push_back(key & 0xFF); auto it = l2sw.find(key); be16(v, it == l2sw.end() ? 0 : it->second); respond(f, 0, v); return; }
        case DEVICE_LATENCY:     { std::vector<uint8_t> v; be16(v, 0); v.push_back(latency); respond(f, 0, v); return; }
        case DEVICE_FS:          respond(f, 0, fsParam()); return;
        case DEVICE_BIT_DEPTH:   { std::vector<uint8_t> v; be16(v, 0); v.push_back(bitDepth); v.push_back(0); respond(f, 0, v); return; }   // dacCmdParamConvEncoding: bits (0x18/0x20), pull-up; the host rejects anything else (event 0x210 error)
        case DEVICE_PREFERRED:   { std::vector<uint8_t> v; be16(v, 0); v.push_back(preferredMaster); respond(f, 0, v); return; }
        case DEVICE_SLAVE_TO_EXT:{ std::vector<uint8_t> v; be16(v, 0); v.push_back(slaveToExt); respond(f, 0, v); return; }
        case DEVICE_CLOCK_MASTER:{ std::vector<uint8_t> v; be16(v, 0); v.push_back(clockMaster); v.push_back(clockMaster); v.push_back(0); respond(f, 0, v); return; }   // size 5
        case DEVICE_CLOCK_SYNC:  respond(f, 0, clockSync()); return;
        case DEVICE_LINK:        respond(f, 0, linkSpeed()); return;
        case DEVICE_CLOCK_STATUS:                          // dac defers this to a ConMon round trip and reports the result as a
            respond(f, 0, clockStatus(), true);            // NOTIFY CLOCK_STATUS (the host's request table marks 0x19 "completed by notify")
            notify(DEVICE_CLOCK_STATUS, clockStatus());
            return;
        case BROWSE_DEVICE_LABEL:respond(f, 0, deviceLabel()); return;
        case BROWSE_COMPLETE: case BROWSE_COMPLETE_ALL: { std::vector<uint8_t> v; be16(v, 0); v.push_back(0); v.push_back(7); v.push_back(1); respond(f, 0, v); return; }
        case BROWSE_DEVICE_LIST: { std::vector<uint8_t> v; be16(v, 0); be16(v, 0); be16(v, 0); respond(f, 0, v); return; }   // no other devices
        case BROWSE_CHANNEL_LABEL: {                           // dev, txrx, channel id -> dev, txrx, channel id, label[32]
            if (f.payload.size() < 5) break;
            std::vector<uint8_t> v(f.payload.begin(), f.payload.begin() + 5);
            fixedStr(v, channelLabel(f.payload[2], (f.payload[3] << 8) | f.payload[4]), 32);
            respond(f, 0, v); return; }
        case BROWSE_CHANNEL_LABELS: {                          // dev, txrx, count, count x {channel id, label[32], result}
            if (f.payload.size() < 5) break;
            int txrx = f.payload[2]; size_t n = (f.payload[3] << 8) | f.payload[4];
            if (5 + n * 35 > f.payload.size()) n = (f.payload.size() - 5) / 35;
            std::vector<uint8_t> v(f.payload.begin(), f.payload.begin() + 3); be16(v, (uint16_t)n);
            for (size_t i = 0; i < n; i++) {
                const uint8_t* e = &f.payload[5 + i * 35];
                v.push_back(e[0]); v.push_back(e[1]);
                fixedStr(v, channelLabel(txrx, (e[0] << 8) | e[1]), 32);
                v.push_back(0);
            }
            respond(f, 0, v); return; }
        case ROUTING_PATCHES: {                                // request: dev, count, count x {rx channel, device, channel, flag}; reply with the current patches
            std::vector<uint8_t> v; be16(v, 0);
            size_t n = f.payload.size() >= 4 ? ((f.payload[2] << 8) | f.payload[3]) : 0;
            if (4 + n * 7 > f.payload.size()) n = (f.payload.size() - 4) / 7;
            be16(v, (uint16_t)n);
            for (size_t i = 0; i < n; i++) {
                const uint8_t* e = &f.payload[4 + i * 7];
                uint16_t rx = (e[0] << 8) | e[1];
                auto it = rxPatch.find(rx);
                v.push_back(e[0]); v.push_back(e[1]);
                if (it == rxPatch.end()) { be16(v, 0xFFFF); be16(v, 0); }          // unpatched
                else { be16(v, it->second.first); be16(v, it->second.second); }
                v.push_back(0);                                                   // result
            }
            respond(f, 0, v); return; }
        case NETWORK_IP_ADDRESS: respond(f, 0, ipAddress()); return;
        case NETWORK_MAC_ADDRESS:respond(f, 0, macAddress()); return;
        case CONNECT_DEVICE:     { std::vector<uint8_t> v; be16(v, 0); v.push_back(1); v.push_back(0); respond(f, 0, v); return; }
        case HAREMOTE_BRIDGE_MODE:{ std::vector<uint8_t> v; be16(v, 0); v.push_back(1); v.push_back(0); respond(f, 0, v); return; }
        case DDM_CONNECT_STATE:  { std::vector<uint8_t> v; be16(v, 0); v.push_back(0); respond(f, 0, v); return; }
        default: break;
        }
    } else if (f.h.category == SET) {
        switch (f.h.cmd) {
        case DEVICE_FS:          if (f.payload.size() >= 3) { static const uint32_t fsTab[] = { 44100, 48000, 88200, 96000, 176400, 192000 }; unsigned i = f.payload[2] >> 4; if (i < 6) fs = fsTab[i]; } respond(f, 0, fsParam()); return;
        case DEVICE_LATENCY:     if (f.payload.size() >= 3) latency = f.payload[2]; respond(f, 0, f.payload); return;
        case DEVICE_PREFERRED:   if (f.payload.size() >= 3) preferredMaster = f.payload[2]; respond(f, 0, f.payload); return;
        case DEVICE_SLAVE_TO_EXT:if (f.payload.size() >= 3) slaveToExt = f.payload[2]; respond(f, 0, f.payload); return;
        case BROWSE_DEVICE_LABEL:if (f.payload.size() >= 34) deviceName.assign((const char*)&f.payload[2], strnlen((const char*)&f.payload[2], 32));
                                 respond(f, 0, deviceLabel()); renameDoneAtMs = nowMs + 400; return;   // dr_device_rename, watched by dacWatchRenameProcTask
        case BASE_MANUFACTURER:  if (f.payload.size() >= 70) {            // dev, model id[8], model name[32], model version[4], device id[8], version string[16]
                                     memcpy(modelId, &f.payload[2], 8); modelName.assign((const char*)&f.payload[10], strnlen((const char*)&f.payload[10], 32));
                                     modelVer = ((uint32_t)f.payload[42] << 24) | (f.payload[43] << 16) | (f.payload[44] << 8) | f.payload[45];
                                     modelVerStr.assign((const char*)&f.payload[54], strnlen((const char*)&f.payload[54], 16)); }
                                 respond(f, 0, manufacturer(), true); manfNotifyAtMs = nowMs + 500; return;
        case BASE_RESTART:                                 // dev, reset type (0 normal, 1 factory): dac reboots the module
            if (f.payload.size() >= 3 && f.payload[2] == 1) { deviceName = freshName(); chLabel[0].clear(); chLabel[1].clear(); rxPatch.clear(); }
            respond(f, 0, f.payload); downAtMs = nowMs + 500; return;
        case L2SW_WRITE:         if (f.payload.size() >= 7) l2sw[((uint32_t)f.payload[2] << 16) | ((uint32_t)f.payload[3] << 8) | f.payload[4]] = (uint16_t)((f.payload[5] << 8) | f.payload[6]);
                                 respond(f, 0, f.payload); return;      // dev, type, phy_id, reg_no, value (Marvell 88E6350R switch via dac)
        case BROWSE_RESTART: case BROWSE_RESTART_ALL: case BROWSE_START_BROWSING: case BROWSE_REBUILD_DEV_LIST:
            browseCompleteAtMs = nowMs + 400;              // a real module re-browses and then notifies completion
            respond(f, 0, f.payload); return;
        case ROUTING_PATCH_ON:   if (f.payload.size() >= 8) rxPatch[(f.payload[2] << 8) | f.payload[3]] = { (uint16_t)((f.payload[4] << 8) | f.payload[5]), (uint16_t)((f.payload[6] << 8) | f.payload[7]) };
                                 respond(f, 0, f.payload); return;      // dev, rx channel, tx device, tx channel
        case ROUTING_PATCH_OFF:  if (f.payload.size() >= 4) rxPatch.erase((f.payload[2] << 8) | f.payload[3]); respond(f, 0, f.payload); return;
        case DEVICE_BIT_DEPTH:   if (f.payload.size() >= 3 && (f.payload[2] == 24 || f.payload[2] == 32)) bitDepth = f.payload[2]; respond(f, 0, f.payload); return;
        case BASE_KEEPALIVE: case BASE_MODE: case BASE_LOCK: case DEVICE_MONITOR:
        case BROWSE_IDENTIFY:
        case BROWSE_CHANNEL_LABEL:                             // dev, txrx, channel id, label[32]
            if (f.payload.size() >= 37) chLabel[f.payload[2] & 1][(f.payload[3] << 8) | f.payload[4]].assign((const char*)&f.payload[5], strnlen((const char*)&f.payload[5], 32));
            respond(f, 0, f.payload); return;
        case BROWSE_CHANNEL_LABELS: {                          // dev, txrx, count, count x {channel id, label[32]} -> same with a result byte per entry
            if (f.payload.size() < 5) break;
            int txrx = f.payload[2]; size_t n = (f.payload[3] << 8) | f.payload[4];
            if (5 + n * 34 > f.payload.size()) n = (f.payload.size() - 5) / 34;
            std::vector<uint8_t> v(f.payload.begin(), f.payload.begin() + 3); be16(v, (uint16_t)n);
            for (size_t i = 0; i < n; i++) {
                const uint8_t* e = &f.payload[5 + i * 34];
                chLabel[txrx & 1][(e[0] << 8) | e[1]].assign((const char*)&e[2], strnlen((const char*)&e[2], 32));
                v.insert(v.end(), e, e + 34); v.push_back(0);
            }
            respond(f, 0, v); return; }
        case ROUTING_PATCHES:
        case NETWORK_SWMODE:     if (f.payload.size() >= 4 && f.payload[2] == 1) swModeConfigured = f.payload[3]; respond(f, 0, f.payload); return;   // set_NETWORK_SWMODE: element must be 1
        case NETWORK_IP_ADDRESS: case NETWORK_MDNS_SERVICE: case NETWORK_DNS_SPEC: case NETWORK_ACCESS:
        case HAREMOTE_BRIDGE_MODE: case DACLOG: case SYSLOG: case TMP_TEXT: case FREEMEM_THRESHOLD: case GPIO:
            respond(f, 0, f.payload); return;
        default: break;
        }
    }
    unknownCmds++;
    if (log) fprintf(stderr, "[DANTE] unhandled cat %02X cmd %u\n", f.h.category, f.h.cmd);
    // Fallback: dac's per-command parameter sizes (getCmdParamSize table). A GET of a known command gets a
    // zero-filled parameter of the right size (so the host's fixed-size parser stays in bounds); a SET is echoed.
    static const std::map<uint8_t, uint16_t> kParamSize = { {0x00,28}, {0x01,3}, {0x02,4}, {0x03,18}, {0x04,130}, {0x05,18}, {0x07,7}, {0x0A,3}, {0x0B,4}, {0x10,3}, {0x11,4}, {0x12,4}, {0x13,3}, {0x14,3}, {0x15,3}, {0x16,5}, {0x17,4}, {0x18,4}, {0x19,22}, {0x21,34}, {0x22,37}, {0x24,2}, {0x25,2}, {0x26,3}, {0x27,3}, {0x28,2}, {0x29,4}, {0x2A,4}, {0x2B,42}, {0x2C,41}, {0x30,40}, {0x31,256}, {0x32,4}, {0x33,2}, {0x34,60}, {0x35,14}, {0x38,518}, {0x39,6}, {0x3A,4}, {0x3B,6}, {0x3C,138}, {0x40,18}, {0x41,9}, {0x42,37}, {0x43,48}, {0x44,35}, {0x45,4}, {0x50,5}, {0x51,6}, {0x52,4}, {0x60,7}, {0x61,7}, {0x62,23}, {0x68,3}, {0x69,18}, {0x6A,4}, {0x70,3}, {0x71,8}, {0x72,8}, {0x73,4}, {0x74,4}, {0x80,7}, {0x81,2}, {0x82,3}, {0x83,6}, {0x84,10}, {0x90,4}, {0xF0,7}, {0xF1,4}, {0xF2,2}, {0xF3,8}, {0xF4,4}, {0xF5,5}, {0xF6,82}, {0xF7,7} };
    if (f.h.category == GET) {
        auto it = kParamSize.find(f.h.cmd);
        if (it != kParamSize.end()) { std::vector<uint8_t> v(f.payload.begin(), f.payload.begin() + std::min<size_t>(f.payload.size(), it->second)); v.resize(it->second, 0); respond(f, 0, v); return; }
    }
    respond(f, f.h.category == SET ? 0 : 1, f.payload);   // echo (errors keep the request's size so the host's parser stays in bounds)
}

// dacCmdParamConvHaremoteMode packs (be16 device, u8 mode, u8 state); dac's dac_ipc_haremote notifies it when the
// conmon HA-remote bridge state changes (a console connects / disconnects).
void DanteModule::notifyHaRemote(bool active) {
    std::vector<uint8_t> v; be16(v, 0); v.push_back(1); v.push_back(active ? 1 : 0);
    notify(HAREMOTE_BRIDGE_MODE, v);
    if (log) fprintf(stderr, "[DANTE] notify HAREMOTE_BRIDGE_MODE active=%d\n", active ? 1 : 0);
}

// ---- events that originate on the Dante network (fed by the Controller-interoperable endpoint) ----------------------------
void DanteModule::networkRenamed(const std::string& name) {
    if (name.empty() || name == deviceName) return;
    deviceName = name.size() > 31 ? name.substr(0, 31) : name;
    renameDoneAtMs = nowMs + 200;                         // dacWatchRenameProcTask: label applied -> notifyRename + restart browsing
    if (log) { fprintf(stderr, "[DANTE] device renamed on the network: %s", deviceName.c_str()); fputc(10, stderr); }
}
void DanteModule::networkChannelLabel(int txrx, uint16_t ch, const std::string& label) {
    std::string cur = channelLabel(txrx, ch); if (cur == label) return;
    chLabel[txrx & 1][ch] = label;
    std::vector<uint8_t> v; be16(v, 0); v.push_back((uint8_t)txrx); be16(v, ch); fixedStr(v, label, 32);
    if (running) notify(BROWSE_CHANNEL_LABEL, v);
}
void DanteModule::networkClockChanged() { if (running) notify(DEVICE_CLOCK_STATUS, clockStatus()); }
void DanteModule::networkLinkChanged() { if (running) notify(DEVICE_LINK, linkSpeed()); }
void DanteModule::networkDeviceListChanged() {
    if (!running) return;
    std::vector<std::string> names; if (listSelf) names.push_back(deviceName); for (auto& p : peers) names.push_back(p);
    std::vector<uint8_t> v; be16(v, 0); be16(v, (uint16_t)names.size()); for (auto& n : names) fixedStr(v, n, 32);
    notify(BROWSE_DIV_DEVICE_LIST, v);
}

void DanteModule::networkFsChanged(uint32_t newFs) {
    if (newFs == fs) return;
    fs = newFs;
    if (running) notify(DEVICE_FS, fsParam());
    if (log) { fprintf(stderr, "[DANTE] sample rate changed on the network: %u Hz", fs); fputc(10, stderr); }
}
void DanteModule::networkIdentify() {
    std::vector<uint8_t> v; be16(v, 0); v.push_back(1);
    if (running) notify(BROWSE_IDENTIFY, v);
}
