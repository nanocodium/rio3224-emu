#include "wasapi_backend.h"
#include "resampler.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#endif

namespace rio {

// ---------------------------------------------------------------------------------------------------------------------
// lock-free single-producer / single-consumer ring of interleaved float samples
struct FloatRing {
    std::vector<float> buf; size_t cap = 0; int ch = 1;
    std::atomic<size_t> head{0}, tail{0};              // head = write index (samples), tail = read index
    void init(size_t frames, int channels) { ch = channels; cap = frames * channels; buf.assign(cap, 0.f); head = tail = 0; }
    size_t readable() const { size_t h = head.load(std::memory_order_acquire), t = tail.load(std::memory_order_acquire); return h >= t ? h - t : cap - t + h; }
    size_t writable() const { return cap - 1 - readable(); }
    size_t write(const float* x, size_t n) {
        n = std::min(n, writable()); size_t h = head.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; i++) { buf[h] = x[i]; if (++h == cap) h = 0; }
        head.store(h, std::memory_order_release); return n;
    }
    size_t read(float* y, size_t n) {
        n = std::min(n, readable()); size_t t = tail.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; i++) { y[i] = buf[t]; if (++t == cap) t = 0; }
        tail.store(t, std::memory_order_release); return n;
    }
};

static std::vector<std::string> splitSpecs(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

#ifdef _WIN32

namespace {
std::string narrow(const wchar_t* w) { if (!w) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr); std::string s(n > 0 ? n - 1 : 0, 0); if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr); return s; }
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string hr2s(HRESULT hr) { char b[32]; snprintf(b, sizeof b, "HRESULT 0x%08lX", (unsigned long)hr); return b; }
struct ComInit { HRESULT hr; ComInit() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); } ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); } };
template <class T> struct Com { T* p = nullptr; ~Com() { if (p) p->Release(); } T** operator&() { return &p; } T* operator->() const { return p; } operator bool() const { return p != nullptr; } };

std::string deviceName(IMMDevice* dev) {
    Com<IPropertyStore> ps; if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) return "?";
    PROPVARIANT v; PropVariantInit(&v); std::string s = "?";
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) s = narrow(v.pwszVal);
    PropVariantClear(&v); return s;
}

// sample format of a shared-mode mix format
struct Fmt { int channels = 0; uint32_t rate = 0; int bits = 0; int validBits = 0; bool isFloat = false; int frameBytes = 0; };
Fmt describeFmt(const WAVEFORMATEX* wf) {
    Fmt f; f.channels = wf->nChannels; f.rate = wf->nSamplesPerSec; f.bits = wf->wBitsPerSample; f.validBits = f.bits; f.frameBytes = wf->nBlockAlign;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.isFloat = IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0; f.validBits = ex->Samples.wValidBitsPerSample ? ex->Samples.wValidBitsPerSample : f.bits;
    } else f.isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    return f;
}
void toFloat(const Fmt& f, const uint8_t* src, float* dst, size_t samples) {
    if (f.isFloat && f.bits == 32) { memcpy(dst, src, samples * 4); return; }
    for (size_t i = 0; i < samples; i++) {
        const uint8_t* p = src + i * (f.bits / 8); int32_t v = 0;
        switch (f.bits) {
        case 16: v = (int16_t)(p[0] | p[1] << 8) << 16; break;
        case 24: v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); break;
        case 32: v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); break;
        default: v = 0;
        }
        dst[i] = (float)(v / 2147483648.0);
    }
}
void fromFloat(const Fmt& f, const float* src, uint8_t* dst, size_t samples) {
    if (f.isFloat && f.bits == 32) { memcpy(dst, src, samples * 4); return; }
    for (size_t i = 0; i < samples; i++) {
        double x = src[i]; if (x > 1.0) x = 1.0; if (x < -1.0) x = -1.0;
        int32_t v = (int32_t)std::lrint(x * 2147483647.0); uint8_t* p = dst + i * (f.bits / 8);
        switch (f.bits) {
        case 16: { int16_t s = (int16_t)(v >> 16); p[0] = s & 0xFF; p[1] = (s >> 8) & 0xFF; break; }
        case 24: p[0] = (v >> 8) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 24) & 0xFF; break;
        case 32: { if (f.validBits < 32) v &= ~((1 << (32 - f.validBits)) - 1); p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; break; }
        default: break;
        }
    }
}
} // namespace

std::vector<HostDeviceInfo> wasapiListDevices(std::string* err) {
    std::vector<HostDeviceInfo> out; ComInit com;
    Com<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) { if (err) *err = "MMDeviceEnumerator: " + hr2s(hr); return out; }
    for (int dir = 0; dir < 2; dir++) {
        EDataFlow flow = dir == 0 ? eCapture : eRender;
        std::string defId; { Com<IMMDevice> d; if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) { LPWSTR id = nullptr; if (SUCCEEDED(d->GetId(&id))) { defId = narrow(id); CoTaskMemFree(id); } } }
        Com<IMMDeviceCollection> col; if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            Com<IMMDevice> d; if (FAILED(col->Item(i, &d))) continue;
            HostDeviceInfo info; info.capture = dir == 0; LPWSTR id = nullptr; if (SUCCEEDED(d->GetId(&id))) { info.id = narrow(id); CoTaskMemFree(id); }
            info.name = deviceName(d.p); info.isDefault = info.id == defId;
            Com<IAudioClient> ac; if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
                WAVEFORMATEX* wf = nullptr; if (SUCCEEDED(ac->GetMixFormat(&wf)) && wf) { info.channels = wf->nChannels; info.rate = wf->nSamplesPerSec; CoTaskMemFree(wf); }
            }
            out.push_back(info);
        }
    }
    return out;
}

struct WasapiBackend::Impl {
    struct Dir {
        bool capture = true, loopback = false; std::string name; int firstHost = 0;
        Com<IMMDevice> dev; Com<IAudioClient> client; Fmt fmt; UINT32 bufferFrames = 0; HANDLE evt = nullptr;
        Com<IAudioCaptureClient> cap; Com<IAudioRenderClient> ren;
        FloatRing ring; std::vector<Resampler> rs; std::thread thread; std::atomic<bool> run{false};
        std::vector<float> tmp, tmp2, zeros; std::atomic<uint64_t> glitches{0}; std::atomic<double> fillMs{0};
        double targetFrames = 0; double scale = 1.0; uint64_t frames = 0, starved = 0;
    };
    std::vector<std::unique_ptr<Dir>> ins; std::unique_ptr<Dir> out;
    uint32_t rioFs = 48000; double latencyMs = 30; ComInit com;

    static bool pick(IMMDeviceEnumerator* en, EDataFlow flow, const std::string& spec, Com<IMMDevice>& dev, std::string& name, std::string& err) {
        if (spec == "default" || spec.empty()) {
            if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, &dev))) { err = "no default " + std::string(flow == eCapture ? "capture" : "render") + " device"; return false; }
            name = deviceName(dev.p); return true;
        }
        Com<IMMDeviceCollection> col; if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) { err = "EnumAudioEndpoints failed"; return false; }
        UINT n = 0; col->GetCount(&n); std::string want = lower(spec);
        for (UINT i = 0; i < n; i++) { Com<IMMDevice> d; if (FAILED(col->Item(i, &d))) continue; std::string nm = deviceName(d.p); if (lower(nm).find(want) != std::string::npos) { dev.p = d.p; d.p = nullptr; name = nm; return true; } }
        err = "no " + std::string(flow == eCapture ? "capture" : "render") + " device matching '" + spec + "'"; return false;
    }

    bool openDir(Dir& d, bool capture, const std::string& spec, std::string& err) {
        d.capture = capture;
        Com<IMMDeviceEnumerator> en;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (FAILED(hr)) { err = "MMDeviceEnumerator: " + hr2s(hr); return false; }
        std::string s = spec; d.loopback = false;
        if (capture && (s == "loopback" || s.rfind("loopback:", 0) == 0)) { d.loopback = true; s = s.size() > 9 ? s.substr(9) : "default"; }
        if (!pick(en.p, (capture && !d.loopback) ? eCapture : eRender, s, d.dev, d.name, err)) return false;
        if (FAILED(hr = d.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&d.client))) { err = "Activate: " + hr2s(hr); return false; }
        WAVEFORMATEX* wf = nullptr; if (FAILED(hr = d.client->GetMixFormat(&wf)) || !wf) { err = "GetMixFormat: " + hr2s(hr); return false; }
        d.fmt = describeFmt(wf);
        DWORD flags = d.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : (capture ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK);
        REFERENCE_TIME dur = (REFERENCE_TIME)(latencyMs * 10000.0 * (capture ? 2 : 1));
        hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, wf, nullptr);
        CoTaskMemFree(wf);
        if (FAILED(hr)) { err = d.name + ": " + std::string(capture ? "capture" : "render") + " Initialize: " + hr2s(hr); return false; }
        d.client->GetBufferSize(&d.bufferFrames);
        if (!capture) { d.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr); d.client->SetEventHandle(d.evt); }
        if (capture) { if (FAILED(hr = d.client->GetService(__uuidof(IAudioCaptureClient), (void**)&d.cap))) { err = "IAudioCaptureClient: " + hr2s(hr); return false; } }
        else if (FAILED(hr = d.client->GetService(__uuidof(IAudioRenderClient), (void**)&d.ren))) { err = "IAudioRenderClient: " + hr2s(hr); return false; }
        // rings hold ~1 s at the device rate; the servo targets latencyMs of buffering
        d.ring.init(d.fmt.rate, d.fmt.channels); d.rs.resize(d.fmt.channels);
        for (auto& r : d.rs) r.reset(capture ? (double)d.fmt.rate : (double)rioFs, capture ? (double)rioFs : (double)d.fmt.rate);
        d.targetFrames = latencyMs * (capture ? rioFs : d.fmt.rate) / 1000.0;
        d.tmp.resize(4096 * d.fmt.channels); d.tmp2.resize(4096); d.zeros.assign(4096, 0.f);
        // start with the target amount of silence buffered so neither side glitches while the other spins up
        size_t pre = (size_t)d.targetFrames;
        if (capture) { std::vector<float> z((size_t)(pre * (double)d.fmt.rate / rioFs) + 1, 0.f); for (auto& r : d.rs) r.push(z.data(), z.size()); }
        else { std::vector<float> z(pre * d.fmt.channels, 0.f); d.ring.write(z.data(), z.size()); }
        return true;
    }

    void captureThread(Dir& in) {
        ComInit com; DWORD task = 0; HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        in.client->Start();
        while (in.run) {
            UINT32 pkt = 0;
            while (in.run && SUCCEEDED(in.cap->GetNextPacketSize(&pkt)) && pkt) {
                BYTE* data = nullptr; UINT32 n = 0; DWORD flags = 0;
                if (FAILED(in.cap->GetBuffer(&data, &n, &flags, nullptr, nullptr))) break;
                size_t samples = (size_t)n * in.fmt.channels; if (in.tmp.size() < samples) in.tmp.resize(samples);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(in.tmp.begin(), in.tmp.begin() + samples, 0.f); else toFloat(in.fmt, data, in.tmp.data(), samples);
                size_t w = in.ring.write(in.tmp.data(), samples); if (w < samples) in.glitches += (samples - w) / in.fmt.channels;
                in.cap->ReleaseBuffer(n);
            }
            Sleep(2);
        }
        in.client->Stop(); if (h) AvRevertMmThreadCharacteristics(h);
    }
    void renderThread(Dir& o) {
        ComInit com; DWORD task = 0; HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        { BYTE* data = nullptr; if (SUCCEEDED(o.ren->GetBuffer(o.bufferFrames, &data))) o.ren->ReleaseBuffer(o.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT); }   // start with silence
        o.client->Start();
        while (o.run) {
            if (WaitForSingleObject(o.evt, 200) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0; if (FAILED(o.client->GetCurrentPadding(&pad))) continue;
            UINT32 want = o.bufferFrames - pad; if (!want) continue;
            BYTE* data = nullptr; if (FAILED(o.ren->GetBuffer(want, &data))) continue;
            size_t samples = (size_t)want * o.fmt.channels; if (o.tmp.size() < samples) o.tmp.resize(samples);
            size_t got = o.ring.read(o.tmp.data(), samples);
            if (got < samples) { o.glitches += (samples - got) / o.fmt.channels; std::fill(o.tmp.begin() + got, o.tmp.begin() + samples, 0.f); }
            fromFloat(o.fmt, o.tmp.data(), data, samples);
            o.ren->ReleaseBuffer(want, 0);
            o.fillMs = o.ring.readable() / (double)o.fmt.channels * 1000.0 / o.fmt.rate;
        }
        o.client->Stop(); if (h) AvRevertMmThreadCharacteristics(h);
    }
    static void stopDir(Dir& d) { if (d.run) { d.run = false; if (d.thread.joinable()) d.thread.join(); } if (d.evt) { CloseHandle(d.evt); d.evt = nullptr; } }
    static DirStats statsOf(const Dir& d) {
        DirStats s; s.name = d.name; s.loopback = d.loopback; s.channels = d.fmt.channels; s.rate = d.fmt.rate; s.firstHostChannel = d.firstHost;
        s.frames = d.frames; s.engineStarved = d.starved; s.deviceGlitches = d.glitches; s.ratioScale = d.scale; s.fillMs = d.fillMs; return s;
    }
};

WasapiBackend::WasapiBackend() : impl(new Impl) {}
WasapiBackend::~WasapiBackend() { close(); delete impl; }

bool WasapiBackend::open(const std::string& inSpec, const std::string& outSpec, uint32_t rioFs, double targetLatencyMs, std::string& err) {
    impl->rioFs = rioFs; impl->latencyMs = targetLatencyMs > 0 ? targetLatencyMs : 30.0;
    inCh = 0;
    for (const std::string& spec : splitSpecs(inSpec)) {
        auto d = std::make_unique<Impl::Dir>();
        if (!impl->openDir(*d, true, spec, err)) return false;
        d->firstHost = inCh; inCh += d->fmt.channels; impl->ins.push_back(std::move(d));
    }
    if (!outSpec.empty()) {
        auto d = std::make_unique<Impl::Dir>();
        if (!impl->openDir(*d, false, outSpec, err)) return false;
        outCh = d->fmt.channels; impl->out = std::move(d);
    }
    for (auto& d : impl->ins) { Impl::Dir* p = d.get(); p->run = true; p->thread = std::thread([this, p] { impl->captureThread(*p); }); }
    if (impl->out) { Impl::Dir* p = impl->out.get(); p->run = true; p->thread = std::thread([this, p] { impl->renderThread(*p); }); }
    return true;
}

void WasapiBackend::close() {
    if (!impl) return;
    for (auto& d : impl->ins) Impl::stopDir(*d);
    if (impl->out) Impl::stopDir(*impl->out);
}

void WasapiBackend::pullInputs(float* const* ch, int nch, size_t frames) {
    for (int c = 0; c < nch; c++) std::fill(ch[c], ch[c] + frames, 0.f);
    for (auto& dp : impl->ins) {
        Impl::Dir& d = *dp;
        if (!d.run) continue;
        // move everything the device delivered into the per-channel resamplers
        size_t avail = d.ring.readable(); if (d.tmp2.size() < avail) d.tmp2.resize(avail);
        size_t got = d.ring.read(d.tmp2.data(), avail), fr = got / d.fmt.channels;
        if (fr) {
            std::vector<float>& de = d.tmp; if (de.size() < fr) de.resize(fr);
            for (int c = 0; c < d.fmt.channels; c++) { for (size_t i = 0; i < fr; i++) de[i] = d.tmp2[i * d.fmt.channels + c]; d.rs[c].push(de.data(), fr); }
        }
        // drift servo: keep ~targetFrames (Rio-rate) of input queued; consume faster when the queue grows
        double queuedRio = d.rs.empty() ? 0 : d.rs[0].queued() * (double)impl->rioFs / d.fmt.rate;
        double e = (queuedRio - d.targetFrames) / std::max(1.0, d.targetFrames);
        d.scale = 1.0 + std::clamp(e * 0.002, -0.003, 0.003);
        for (auto& r : d.rs) r.setRatioScale(d.scale);
        d.fillMs = queuedRio * 1000.0 / impl->rioFs;
        bool short_ = false;
        for (int c = 0; c < d.fmt.channels; c++) {
            int hc = d.firstHost + c; if (hc >= nch) break;
            size_t n = d.rs[c].pull(ch[hc], frames);
            if (n < frames) { std::fill(ch[hc] + n, ch[hc] + frames, 0.f); short_ = true; }
        }
        if (short_) d.starved++;
        d.frames += frames;
    }
}

void WasapiBackend::pushOutputs(const float* const* analog, int nAnalog, const float* const* aes, int nAes, size_t frames) {
    if (!impl->out || !impl->out->run) return;
    Impl::Dir& d = *impl->out;
    if (d.zeros.size() < frames) d.zeros.assign(frames, 0.f);
    // Rio analog outputs 1..N -> device channels 1..N (AES outputs follow when the device has more channels)
    for (int c = 0; c < d.fmt.channels; c++) {
        const float* src = c < nAnalog ? analog[c] : (c - nAnalog < nAes ? aes[c - nAnalog] : d.zeros.data());
        d.rs[c].push(src, frames);
    }
    // servo on the render ring: more buffered than the target -> produce fewer device samples per Rio frame
    double fillDev = d.ring.readable() / (double)d.fmt.channels;
    double e = (fillDev - d.targetFrames) / std::max(1.0, d.targetFrames);
    d.scale = 1.0 + std::clamp(e * 0.002, -0.003, 0.003);
    for (auto& r : d.rs) r.setRatioScale(d.scale);
    size_t n = d.rs.empty() ? 0 : d.rs[0].available();
    if (n) {
        if (d.tmp2.size() < n) d.tmp2.resize(n);
        std::vector<float>& il = d.tmp; if (il.size() < n * d.fmt.channels) il.resize(n * d.fmt.channels);
        size_t made = n;
        for (int c = 0; c < d.fmt.channels; c++) { size_t m = d.rs[c].pull(d.tmp2.data(), n); made = std::min(made, m); for (size_t i = 0; i < m; i++) il[i * d.fmt.channels + c] = d.tmp2[i]; }
        size_t w = d.ring.write(il.data(), made * d.fmt.channels); if (w < made * d.fmt.channels) d.starved++;   // ring full = overrun
    }
    d.frames += frames;
}

std::vector<WasapiBackend::DirStats> WasapiBackend::inputStats() const { std::vector<DirStats> v; for (auto& d : impl->ins) v.push_back(Impl::statsOf(*d)); return v; }
WasapiBackend::DirStats WasapiBackend::outputStats() const { return impl->out ? Impl::statsOf(*impl->out) : DirStats{}; }

std::string WasapiBackend::describe() const {
    std::string s = "wasapi in:";
    if (impl->ins.empty()) s += " -";
    for (auto& d : impl->ins) { char b[256]; snprintf(b, sizeof b, " [%s%s: %d ch @ %u Hz -> host %d-%d]", d->loopback ? "loopback " : "", d->name.c_str(), d->fmt.channels, d->fmt.rate, d->firstHost + 1, d->firstHost + d->fmt.channels); s += b; }
    s += " | out:";
    if (impl->out) { char b[256]; snprintf(b, sizeof b, " [%s: %d ch @ %u Hz]", impl->out->name.c_str(), impl->out->fmt.channels, impl->out->fmt.rate); s += b; } else s += " -";
    return s;
}

#else   // ---------------------------------------------------------------- not Windows: stubs

std::vector<HostDeviceInfo> wasapiListDevices(std::string* err) { if (err) *err = "WASAPI is only available in the Windows build"; return {}; }
struct WasapiBackend::Impl {};
WasapiBackend::WasapiBackend() : impl(nullptr) {}
WasapiBackend::~WasapiBackend() {}
bool WasapiBackend::open(const std::string&, const std::string&, uint32_t, double, std::string& err) { err = "WASAPI is only available in the Windows build"; return false; }
void WasapiBackend::close() {}
void WasapiBackend::pullInputs(float* const* ch, int nch, size_t frames) { for (int c = 0; c < nch; c++) for (size_t i = 0; i < frames; i++) ch[c][i] = 0.f; }
void WasapiBackend::pushOutputs(const float* const*, int, const float* const*, int, size_t) {}
std::vector<WasapiBackend::DirStats> WasapiBackend::inputStats() const { return {}; }
WasapiBackend::DirStats WasapiBackend::outputStats() const { return {}; }
std::string WasapiBackend::describe() const { return "wasapi (unavailable)"; }

#endif

} // namespace rio
