// ============================================================
//  VoiceChat core — Proximity acoustics implementation
//  File: core/src/proximity.cpp
// ============================================================
#include "../include/proximity.h"
#include <cmath>
#include <algorithm>

namespace vc::core {

float effectiveRange(const Player& s, const ProximityTuning& t)
{
    if (s.rangeOverride > 0.f) return s.rangeOverride;
    switch (s.range) {
        case RangePreset::Whisper: return t.whisper;
        case RangePreset::Shout:   return t.shout;
        case RangePreset::Normal:
        default:                   return t.normal;
    }
}

float dist2(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx*dx + dy*dy + dz*dz;
}

ProximityResult evaluate(const Player& sp, const Player& li,
                         const ProximityTuning& t)
{
    ProximityResult r;

    // (1) Dimension gate FIRST — before any distance math (req. J2).
    if (sp.interior != li.interior || sp.vworld != li.vworld)
        return r;

    // (2) Range gate using real 3D distance (req. J3 — floors matter).
    float range = effectiveRange(sp, t);
    float d2 = dist2(sp.pos, li.pos);
    if (d2 >= range * range) return r;

    float d = std::sqrt(d2);

    // (3) Logarithmic-style falloff: 1 at the source, fading with an
    //     exponent so it's loud up close, drops fast through the middle,
    //     and tails off gently toward the edge (req. D1).
    //     vol = (1 - d/range) ^ falloffExp
    float lin = 1.f - (d / range);
    r.volume = std::pow(std::max(0.f, lin), t.falloffExp);

    // (4) Occlusion: a closed vehicle muffles voice in/out (req. J4).
    //     Convertibles / bikes (vehicle==1, "open") get no penalty.
    if (t.occlusion && (sp.vehicle == 2 || li.vehicle == 2)) {
        r.volume *= t.occludeAtten;
        r.occluded = true;
    }

    // (5) Stereo pan from the relative X axis (req. D3). A heading-aware
    //     version (using the listener's camera yaw) can replace this once
    //     the bridge forwards heading; X-relative is a decent default.
    float dx = sp.pos.x - li.pos.x;
    float norm = (range > 0.f) ? std::clamp(dx / range, -1.f, 1.f) : 0.f;
    r.pan = norm * t.panStrength;

    r.audible = r.volume > 0.001f;
    return r;
}

} // namespace vc::core
