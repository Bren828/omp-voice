// ============================================================
//  VoiceChat core — Audio DSP implementation
//  File: core/src/dsp.cpp
// ============================================================
#include "../include/dsp.h"
#include <cmath>
#include <algorithm>

namespace vc::core::dsp {

static constexpr float kPi = 3.14159265358979323846f;

void Biquad::setLowpass(float fs, float fc, float q)
{
    fc = std::clamp(fc, 20.f, fs * 0.49f);
    float w0 = 2.f * kPi * fc / fs;
    float cw = std::cos(w0), sw = std::sin(w0);
    float alpha = sw / (2.f * q);
    float a0 =  1.f + alpha;
    b0 = ((1.f - cw) * 0.5f) / a0;
    b1 =  (1.f - cw) / a0;
    b2 = ((1.f - cw) * 0.5f) / a0;
    a1 = (-2.f * cw) / a0;
    a2 =  (1.f - alpha) / a0;
}

void Biquad::setHighpass(float fs, float fc, float q)
{
    fc = std::clamp(fc, 20.f, fs * 0.49f);
    float w0 = 2.f * kPi * fc / fs;
    float cw = std::cos(w0), sw = std::sin(w0);
    float alpha = sw / (2.f * q);
    float a0 =  1.f + alpha;
    b0 = ((1.f + cw) * 0.5f) / a0;
    b1 = -(1.f + cw) / a0;
    b2 = ((1.f + cw) * 0.5f) / a0;
    a1 = (-2.f * cw) / a0;
    a2 =  (1.f - alpha) / a0;
}

void BandpassChain::configure(float fs, float lowHz, float highHz, float q)
{
    if (highHz > fs * 0.49f) highHz = fs * 0.49f;
    if (lowHz < 20.f) lowHz = 20.f;
    if (lowHz >= highHz) lowHz = highHz * 0.5f;
    hp.setHighpass(fs, lowHz, q);
    lp.setLowpass(fs, highHz, q);
}

void drive(float* x, int n, float amount)
{
    if (amount <= 0.f) return;
    float g = 1.f + amount * 4.f;           // pre-gain
    float norm = std::tanh(g);              // keep unity-ish output level
    for (int i = 0; i < n; ++i)
        x[i] = std::tanh(x[i] * g) / norm;
}

void addStatic(float* x, int n, float level, uint32_t& rng)
{
    if (level <= 0.f) return;
    for (int i = 0; i < n; ++i)
        x[i] += level * noise(rng);
}

void garble(float* x, int n, float fs, float& phase, uint32_t& rng)
{
    const float carrier = 110.f;            // low ring-mod => warbling "jam"
    const float dphi = 2.f * kPi * carrier / fs;
    for (int i = 0; i < n; ++i) {
        float ring = x[i] * std::sin(phase);
        // bit-crush to 6-ish bits for the broken-radio texture.
        float crushed = std::round(ring * 32.f) / 32.f;
        x[i] = 0.55f * crushed + 0.45f * noise(rng) * 0.5f;
        phase += dphi;
        if (phase > 2.f * kPi) phase -= 2.f * kPi;
    }
}

void squelchBeep(float* out, int n, float fs, float freqHz, float amp, float& phase)
{
    const float dphi = 2.f * kPi * freqHz / fs;
    for (int i = 0; i < n; ++i) {
        // short attack/decay envelope across the frame so it doesn't click.
        float t = (float)i / (float)n;
        float env = std::sin(kPi * t);      // 0 -> 1 -> 0 over the frame
        out[i] = amp * env * std::sin(phase);
        phase += dphi;
        if (phase > 2.f * kPi) phase -= 2.f * kPi;
    }
}

void limiter(float* x, int n, float ceiling)
{
    if (ceiling <= 0.f) ceiling = 0.95f;
    for (int i = 0; i < n; ++i) {
        float v = x[i];
        if (v > ceiling || v < -ceiling)
            v = ceiling * std::tanh(v / ceiling);   // smooth knee
        x[i] = v;
    }
}

} // namespace vc::core::dsp
