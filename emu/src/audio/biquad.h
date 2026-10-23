// Second-order IIR section in the Rio DSP's coefficient convention.
// The firmware writes five Q30 words per filter: { -a2, -a1, b0, b1, b2 }  (feedback terms stored negated, as
// the DSP multiplies and accumulates them directly).  Transposed direct form II, double precision state.
#pragma once
#include <cmath>
#include <cstdint>

namespace rio {

inline float q30ToFloat(uint32_t w) { return (float)((int32_t)w) / 1073741824.0f; }   // 2^30 = 1.0

struct Biquad {
    double s1 = 0.0, s2 = 0.0;
    // c = { na2, na1, b0, b1, b2 }
    inline float process(float x, const float* c) {
        double y = (double)c[2] * x + s1;
        s1 = (double)c[3] * x + (double)c[1] * y + s2;
        s2 = (double)c[4] * x + (double)c[0] * y;
        return (float)y;
    }
    void reset() { s1 = s2 = 0.0; }
    // magnitude response of a coefficient set at frequency f (Hz) for sample rate fs
    static double magnitudeDb(const float* c, double f, double fs) {
        double w = 2.0 * M_PI * f / fs, cr = std::cos(w), ci = -std::sin(w);
        double c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
        double nr = c[2] + c[3] * cr + c[4] * c2r, ni = c[3] * ci + c[4] * c2i;          // b0 + b1 z^-1 + b2 z^-2
        double dr = 1.0 - c[1] * cr - c[0] * c2r, di = -c[1] * ci - c[0] * c2i;         // 1 + a1 z^-1 + a2 z^-2
        double mag = std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
        return 20.0 * std::log10(mag + 1e-30);
    }
};

// one-pole parameter smoother (linear gain ramps without zipper noise)
struct Smooth {
    float cur = 1.f, target = 1.f, coef = 0.f;
    void setTime(double ms, double fs) { coef = (float)(1.0 - std::exp(-1.0 / (std::max(ms, 0.01) * 0.001 * fs))); }
    inline float next() { cur += (target - cur) * coef; return cur; }
    void jump(float v) { cur = target = v; }
};

} // namespace rio
