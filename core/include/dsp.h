// ============================================================
//  VoiceChat core — Audio DSP primitives (req. C, E1, E2, E4, J12)
//  File: core/include/dsp.h
//
//  Pure float math on 48 kHz mono PCM frames. No Opus, no sockets — fully
//  unit-testable. The relay applies these to the FILTERED-channel buses
//  (radio/phone) before re-encoding; proximity voice is never filtered.
//
//  Pieces:
//   * Biquad / BandpassChain  -> radio & phone colouring (E1)
//   * drive / addStatic       -> radio "edge" + faint static (E1/E2)
//   * garble                  -> half-duplex collision "jamming" (E4)
//   * squelchBeep             -> PTT key up/down roger-beep (E2)
//   * limiter                 -> peak ceiling, anti-earrape (J12)
// ============================================================
#pragma once

#include <cstdint>

namespace vc::core::dsp {

// Tiny xorshift PRNG so noise is deterministic & dependency-free.
inline uint32_t xorshift(uint32_t& s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
// White noise in [-1, 1).
inline float noise(uint32_t& s)
{
    return (xorshift(s) / 2147483648.0f) - 1.0f;
}

// One RBJ-cookbook biquad, transposed direct-form II (state persists).
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;   // normalised (a0 = 1)
    float z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    void setLowpass(float fs, float fc, float q = 0.707f);
    void setHighpass(float fs, float fc, float q = 0.707f);
    inline float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Bandpass = highpass(low) -> lowpass(high). State persists across frames, so
// one instance per (speaker, channel) stream. Radio = 300-3000, phone narrower.
struct BandpassChain {
    Biquad hp, lp;
    void configure(float fs, float lowHz, float highHz, float q = 0.707f);
    void reset() { hp.reset(); lp.reset(); }
    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) x[i] = lp.process(hp.process(x[i]));
    }
};

// Soft saturation for radio "edge". amount 0..1 (0 = clean).
void drive(float* x, int n, float amount);

// Add faint static hiss (E2). level 0..1 (0.05 ~ subtle). rng persists.
void addStatic(float* x, int n, float level, uint32_t& rng);

// Half-duplex garble (E4): two+ speakers on one freq => "jammed". Ring-mod +
// noise + bit-crush makes the collision unintelligible on purpose. `phase`
// and `rng` persist per channel so it sounds continuous.
void garble(float* x, int n, float fs, float& phase, uint32_t& rng);

// Squelch / roger beep on PTT key transition (E2). Writes a sine burst with a
// short attack/decay envelope into `out`. `phase` persists across beep frames.
void squelchBeep(float* out, int n, float fs, float freqHz, float amp, float& phase);

// Soft peak limiter to +/- ceiling (J12 anti-earrape / soundboard). Smooth
// (tanh) so it limits loud sources without hard clipping artefacts.
void limiter(float* x, int n, float ceiling);

} // namespace vc::core::dsp
