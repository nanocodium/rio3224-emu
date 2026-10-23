// Windowed-sinc sample-rate converter for the host audio connection (the "proper SRC" between a host interface
// running at its own clock/rate and the Rio's 44.1/48 kHz).  One instance per channel; the conversion ratio can be
// nudged at run time (a few hundred ppm) so a fill-level servo can absorb the drift between the two clocks.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace rio {

class SincTable {
public:
    static constexpr int kHalfTaps = 16;              // 32-tap kernel
    static constexpr int kPhases = 512;               // sub-sample phases (linear interpolation between them)
    static const SincTable& get() { static SincTable t; return t; }
    // kernel value at (tap index i in [0, 2*kHalfTaps), fractional phase p in [0,1))
    float at(int i, float p) const {
        float fp = p * kPhases; int k = (int)fp; float f = fp - k;
        const float* a = &tab[k * kTaps + i]; const float* b = &tab[(k + 1) * kTaps + i];
        return *a + (*b - *a) * f;
    }
    static constexpr int kTaps = 2 * kHalfTaps;
private:
    std::vector<float> tab;
    SincTable() : tab((kPhases + 1) * kTaps) {
        // Kaiser-windowed sinc, cutoff slightly below Nyquist (0.9) so that aliasing stays below ~-80 dB
        const double beta = 8.6, cutoff = 0.9;
        auto bessel0 = [](double x) { double s = 1, t = 1, y = x * x / 4; for (int k = 1; k < 50; k++) { t *= y / (k * k); s += t; if (t < 1e-12 * s) break; } return s; };
        double ib = bessel0(beta);
        for (int ph = 0; ph <= kPhases; ph++) {
            double p = (double)ph / kPhases, sum = 0;
            for (int i = 0; i < kTaps; i++) {
                double x = (i - kHalfTaps + 1) - p;                   // distance from the output sample position
                double w = x / kHalfTaps; w = std::fabs(w) < 1.0 ? bessel0(beta * std::sqrt(1 - w * w)) / ib : 0.0;
                double s = x == 0.0 ? cutoff : std::sin(M_PI * cutoff * x) / (M_PI * x);
                tab[ph * kTaps + i] = (float)(s * w); sum += s * w;
            }
            for (int i = 0; i < kTaps; i++) tab[ph * kTaps + i] = (float)(tab[ph * kTaps + i] / sum);   // unity DC gain per phase
        }
    }
};

// Converts a stream at rate `in` to rate `out`.  push() input samples, pull() output samples; the history buffer keeps
// the kernel's past so blocks of any size can be used.
class Resampler {
public:
    void reset(double inRate, double outRate) { ratio = inRate / outRate; base = ratio; hist.assign(SincTable::kTaps, 0.f); pos = 0.0; pending.clear(); }
    void setRatioScale(double scale) { ratio = base * scale; }           // drift servo: 1.0 = nominal
    double ratioScale() const { return base > 0 ? ratio / base : 1.0; }
    void push(const float* x, size_t n) { pending.insert(pending.end(), x, x + n); }
    size_t available() const {                                           // output samples that can be produced now
        double have = (double)pending.size() - pos - (SincTable::kHalfTaps + 1);
        return have <= 0 ? 0 : (size_t)(have / ratio) + 1;
    }
    // produce up to n output samples; returns how many were produced (fewer when input runs short)
    size_t pull(float* y, size_t n) {
        const SincTable& T = SincTable::get(); size_t made = 0;
        while (made < n) {
            // output sample at input position pos (relative to pending[0]); need taps pos-15 .. pos+16
            long ip = (long)std::floor(pos); float frac = (float)(pos - ip);
            if (ip + SincTable::kHalfTaps + 1 > (long)pending.size()) break;
            float acc = 0.f;
            for (int i = 0; i < SincTable::kTaps; i++) {
                long idx = ip + (i - SincTable::kHalfTaps + 1);
                float s = idx < 0 ? hist[(size_t)(idx + SincTable::kTaps)] : pending[(size_t)idx];
                acc += s * T.at(i, frac);
            }
            y[made++] = acc; pos += ratio;
        }
        // drop consumed input, keeping kTaps samples of history
        long drop = (long)std::floor(pos) - SincTable::kHalfTaps;
        if (drop > 0) {
            size_t d = (size_t)drop;
            if (d > pending.size()) d = pending.size();
            // history = the last kTaps samples before the new pending[0]
            for (int i = 0; i < SincTable::kTaps; i++) {
                long src = (long)d - SincTable::kTaps + i;
                hist[i] = src < 0 ? hist[(size_t)(src + SincTable::kTaps)] : pending[(size_t)src];
            }
            pending.erase(pending.begin(), pending.begin() + d); pos -= d;
        }
        return made;
    }
    size_t queued() const { return pending.size(); }
private:
    double ratio = 1.0, base = 1.0, pos = 0.0;
    std::vector<float> hist = std::vector<float>(SincTable::kTaps, 0.f), pending;
};

} // namespace rio
