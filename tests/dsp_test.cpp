// ============================================================
//  Stage-E verification: DSP filters + mix policy (no Opus needed)
//  File: tests/dsp_test.cpp
//
//  Exercises the building blocks of the radio/phone bus:
//   * bandpass passes 1 kHz, rejects 60 Hz and 8 kHz (radio colouring, E1)
//   * limiter holds the peak ceiling (J12 anti-earrape)
//   * garble materially changes the signal (half-duplex jam, E4)
//   * squelch beep produces tone energy (E2)
//   * BusMixer::policyGain implements mix / duck / exclusive (I4)
// ============================================================
#include "../core/include/dsp.h"
#include "../core/include/mixer.h"
#include <cmath>
#include <vector>
#include <cstdio>

using namespace vc;
using namespace vc::core;

static int g_fail = 0;
static void check(const char* n, bool ok){ printf("  [%s] %s\n", ok?"PASS":"FAIL", n); if(!ok) ++g_fail; }

static constexpr float FS = 48000.f;
static constexpr int   N  = 4800;          // 100 ms

static void sine(std::vector<float>& b, float freq)
{
    b.resize(N);
    for (int i = 0; i < N; ++i) b[i] = std::sin(2.f * 3.14159265f * freq * i / FS);
}
// RMS over the second half (skip filter transient).
static float rmsTail(const std::vector<float>& b)
{
    double s = 0; int c = 0;
    for (int i = N/2; i < N; ++i) { s += (double)b[i]*b[i]; ++c; }
    return (float)std::sqrt(s / c);
}
static float bandpassRms(float freq)
{
    std::vector<float> b; sine(b, freq);
    dsp::BandpassChain bp; bp.configure(FS, 300.f, 3000.f);
    bp.process(b.data(), N);
    return rmsTail(b);
}

int main()
{
    printf("Stage-E DSP + mix policy:\n");

    // Reference: an unfiltered sine RMS is ~0.707.
    float pass = bandpassRms(1000.f);    // in band
    float lowF = bandpassRms(60.f);      // below band
    float highF= bandpassRms(8000.f);    // above band
    check("bandpass passes 1kHz",        pass > 0.45f);
    check("bandpass rejects 60Hz",       lowF < 0.30f * 0.707f);
    check("bandpass rejects 8kHz",       highF < 0.30f * 0.707f);
    check("bandpass: 1kHz louder than 8kHz", pass > highF * 3.f);

    // Limiter: a hot 2.0 signal must come back under the ceiling.
    {
        std::vector<float> b(256, 2.0f);
        dsp::limiter(b.data(), (int)b.size(), 0.95f);
        float mx = 0; for (float v : b) mx = std::max(mx, std::fabs(v));
        check("limiter holds ceiling", mx <= 0.95f + 1e-4f && mx > 0.5f);
    }

    // Garble must change the signal a lot (it's meant to be unintelligible).
    {
        std::vector<float> a; sine(a, 1000.f);
        std::vector<float> b = a;
        float phase = 0; uint32_t rng = 12345;
        dsp::garble(b.data(), N, FS, phase, rng);
        double diff = 0; for (int i = 0; i < N; ++i) diff += std::fabs(a[i]-b[i]);
        check("garble alters signal", diff / N > 0.2);
    }

    // Squelch beep has real tone energy.
    {
        std::vector<float> b(960); float phase = 0;
        dsp::squelchBeep(b.data(), 960, FS, 1200.f, 0.3f, phase);
        double s = 0; for (float v : b) s += (double)v*v;
        check("squelch beep has energy", std::sqrt(s/960.0) > 0.05);
    }

    // Mix policy (I4). Listener has buses at priority 200 (top) and 100.
    {
        // Duck: the low bus drops to duckLevel, the top bus stays full.
        check("duck: top bus full",   BusMixer::policyGain(MixPolicy::Duck, 200, 200, 0.5f) == 1.f);
        check("duck: low bus ducked", BusMixer::policyGain(MixPolicy::Duck, 100, 200, 0.5f) == 0.5f);
        // Exclusive: only the top bus is heard.
        check("exclusive: top heard",  BusMixer::policyGain(MixPolicy::Exclusive, 200, 200, 0.5f) == 1.f);
        check("exclusive: low muted",  BusMixer::policyGain(MixPolicy::Exclusive, 100, 200, 0.5f) == 0.f);
        // Mix: everything full.
        check("mix: low bus full",     BusMixer::policyGain(MixPolicy::Mix, 100, 200, 0.5f) == 1.f);
    }

    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 2 : 0;
}
