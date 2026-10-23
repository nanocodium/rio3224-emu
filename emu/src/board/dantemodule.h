// Behavioural model of the Dante module (Brooklyn III running Yamaha's `dac -t`) as seen by the Rio host:
// it answers the BCA command protocol on DNT_UART_A, drives the GPI heartbeat lines and the word clock.
// Protocol details come from dac's debug symbols (fw/d2/bk3/qemu/dac_*.txt); see bklyn_frame.h for framing.
#pragma once
#include "bklyn_frame.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <deque>
#include <map>

class DanteModule {
public:
    void notifyHaRemote(bool active);        // NOTIFY HAREMOTE_BRIDGE_MODE (dev, bridge mode 1, active): a console connected to the HA remote channel
    // network-side events (the Controller-interoperable endpoint feeds them in): dac would learn these from ConMon / mDNS
    void networkRenamed(const std::string& name);            // a controller renamed the device -> label applied, NOTIFY BROWSE_DEVICE_LABEL + BROWSE_RESTART
    void networkChannelLabel(int txrx, uint16_t ch, const std::string& label);   // channel label changed on the network -> NOTIFY BROWSE_CHANNEL_LABEL
    void networkClockChanged();                              // grandmaster / sync change -> NOTIFY DEVICE_CLOCK_STATUS
    void networkLinkChanged();                               // link state change -> NOTIFY DEVICE_LINK
    void networkDeviceListChanged();                         // devices appeared / vanished -> NOTIFY BROWSE_DIV_DEVICE_LIST (as dac does after a browse update)
    void networkFsChanged(uint32_t newFs);                   // a controller changed the sample rate -> NOTIFY DEVICE_FS
    void networkIdentify();                                  // a controller asked the unit to identify itself -> NOTIFY BROWSE_IDENTIFY
    // wiring
    std::function<void(const uint8_t* p, size_t n)> toHost;     // bytes towards the host (UART A RX)
    bool log = false;

    // identity / state (defaults resemble a factory Rio3224-D2 module)
    std::string deviceName = "Y001-24-D2-123456";   // configuredName(1) for the default MAC; Machine sets it (--dante-fresh -> freshName())
    std::string freshName() const;
    std::string configuredName(int unitId) const;
    uint8_t mac[6] = { 0x00, 0x1D, 0xC1, 0x12, 0x34, 0x56 };
    uint32_t ip = 0xA9FE0A0A, netmask = 16, gateway = 0;          // 169.254.10.10/16 (link-local like a real box)
    uint32_t fs = 48000;
    uint16_t txChannels = 32, rxChannels = 24;
    uint8_t audFirm[4] = { 4, 2, 5, 3 }, audSoft[4] = { 4, 2, 5, 3 }, ymhFirm[4] = { 2, 1, 8, 0 };
    uint8_t modelId[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };          // SET BASE_MANUFACTURER fields as the Rio host writes them (dacCmdParamGetManufacturer:
    std::string modelName = "Rio3224-D2";                      // model id, model name, model version, device id, version string); a configured unit
    uint32_t modelVer = 0x015B0000;                            // already holds them, so the host does not rewrite them at boot
    std::string modelVerStr = "V1.91";
    uint8_t manfId[8] = { 'Y', 'A', 'M', 'A', 'H', 'A', 0, 0 };
    uint32_t manfCaps = 0x1801;
    bool clockMaster = true, linkUp = true, preferredMaster = false, slaveToExt = false;
    uint8_t latency = 2, bitDepth = 24, mode = 1;                 // DAC_MODE_MAIN
    std::map<uint32_t, uint16_t> l2sw;
    std::map<uint16_t, std::pair<uint16_t, uint16_t>> rxPatch;   // rx channel -> (tx device id, tx channel id)
    std::map<uint16_t, std::string> chLabel[2];                   // [txrx][channel id] -> label (default "01".."32")
    std::string channelLabel(int txrx, uint16_t ch) const;                            // Ethernet switch registers written by the host (type<<16 | phy<<8 | reg)
    uint8_t browseCode = 1, browseFlags = 7, browseChanged = 0;     // BROWSE_COMPLETE: monitor state code (1 = completed, 2/3/0x80 = errors -> host re-browses), status flags, dnssd!=keepalive

    // timing: the module "boots" and announces itself (NOTIFY BASE_INFO) after bootMs
    int bootMs = 12000;                 // real module: 40-60 s; must be after the host DSP task finished its init
    bool listSelf = true;                   // BROWSE_DIV_DEVICE_LIST includes this device
    std::vector<std::string> peers;         // other devices on the (virtual) network, listed after this device (fed by the transport)
    uint8_t swMode = 0, swModeConfigured = 0;   // NETWORK_SWMODE in effect / configured (stored in the module's flash; the Rio host writes its rear DIP setting, 0 = switched, and restarts the module when it had to change it)
    int rebootMs = 10000;               // time the module is away after SET BASE_RESTART (real: ~40 s)
    int announceDelayMs = 5000;         // dacCmdSendStartNotify comes this long after the GPI heartbeat starts
    bool running = false;
    uint64_t startAtMs = 0, announceAtMs = 0, downAtMs = 0;    // schedule (0 = none)
    // response latency: dac is a Linux process behind a 115200 baud UART; immediate answers take a few ms, commands
    // that dac defers to a ConMon round trip (clock status) take longer
    int respDelayMs = 3, deferredDelayMs = 30;
    struct Outgoing { uint64_t atMs; std::vector<uint8_t> bytes; };
    std::deque<Outgoing> outQ;

    DanteModule();
    void hostByte(uint8_t b) { dec.feed(b); }                    // byte from the host (UART A TX)
    void tick(uint64_t ms);                                      // called with elapsed emulated milliseconds
    uint64_t framesIn() const { return dec.frames; }
    uint64_t framesOut = 0, unknownCmds = 0, notifyTimeouts = 0;

private:
    bklyn::Decoder dec;
    uint8_t seq = 0;
    uint64_t nowMs = 0;
    uint64_t browseCompleteAtMs = 0;                             // pending BROWSE_COMPLETE notification (after a restart request)
    uint64_t renameDoneAtMs = 0, manfNotifyAtMs = 0;             // dac completes a rename / manufacturer update asynchronously
    std::vector<uint8_t> browseComplete() const;
    void onFrame(const bklyn::Frame& f);
    void send(uint8_t category, uint8_t cmd, uint8_t seqNo, uint8_t result, uint8_t origin, const std::vector<uint8_t>& payload, int delayMs = -1);
    void flushOut();
    // Notifications go through dac's sendCommand path: one outstanding at a time, the next is sent once the host has
    // acknowledged (category | RES_BIT, same seq); unacknowledged ones are resent a few times and then dropped.
    struct PendingNotify { uint8_t cmd; std::vector<uint8_t> payload; uint8_t seqNo; uint64_t sentMs; int tries; };
    std::deque<PendingNotify> notifyQ;
    bool notifyBusy = false;
    int ackTimeoutMs = 1000, notifyRetries = 3;
    void notify(uint8_t cmd, const std::vector<uint8_t>& payload) { notifyQ.push_back({ cmd, payload, 0, 0, 0 }); pumpNotify(); }
    void pumpNotify();
    // dac's dacCmdParse answers with origin 0 (immediate) or 1 (deferred, e.g. after a ConMon round trip); the
    // request's origin is not echoed.
    uint8_t respOrigin = 0;
    void respond(const bklyn::Frame& req, uint8_t result, const std::vector<uint8_t>& payload, bool deferred = false) { send(req.h.category | bklyn::RES_BIT, req.h.cmd, req.h.seq, result, deferred ? 1 : respOrigin, payload, deferred ? deferredDelayMs : respDelayMs); }
    // payload builders (big-endian serialisations, as dacCmdParamConv* produce them)
    std::vector<uint8_t> baseInfo() const;
    std::vector<uint8_t> dacMode() const;
    std::vector<uint8_t> capability() const;
    std::vector<uint8_t> manufacturer() const;
    std::vector<uint8_t> license() const;
    std::vector<uint8_t> resource() const;
    std::vector<uint8_t> fsParam() const;
    std::vector<uint8_t> clockSync() const;
    std::vector<uint8_t> clockStatus() const;
    std::vector<uint8_t> linkSpeed() const;
    std::vector<uint8_t> ipAddress() const;
    std::vector<uint8_t> macAddress() const;
    std::vector<uint8_t> deviceLabel() const;
    std::vector<uint8_t> u8(uint8_t v) const { return { v }; }
};
