// ============================================================
//  VoiceChat core — Proximity acoustics (req. D, J2-J5)
//  File: core/include/proximity.h
//
//  Pure functions: no state, fully unit-testable. Given two players and
//  tuning params, decide whether and how A hears B by proximity.
// ============================================================
#pragma once

#include <cstdint>
#include "registry.h"

namespace vc::core {

// Tunables (mirrors voice.ini [proximity]). Filled authoritatively by the
// relay from CtrlConfig; defaults here are the sane zero-config values.
struct ProximityTuning {
    float whisper   = 8.f;
    float normal    = 18.f;
    float shout     = 40.f;
    float falloffExp= 2.0f;   // >1 = fast mid drop, gentle tail (log-ish)
    bool  occlusion = true;
    float occludeAtten   = 0.45f;  // closed-vehicle / wall attenuation factor
    float panStrength    = 0.8f;   // 0 = no stereo, 1 = full L/R
};

// Result of evaluating speaker->listener proximity.
struct ProximityResult {
    bool  audible = false;
    float volume  = 0.f;   // 0..1 after log falloff + occlusion
    float pan     = 0.f;   // -1..+1 relative to listener heading-agnostic X
    bool  occluded= false;
};

// Effective audible radius for a speaker given their range preset/override.
float effectiveRange(const Player& speaker, const ProximityTuning& t);

// Squared 3D distance (cheap pre-filter before sqrt).
float dist2(const Vec3& a, const Vec3& b);

// The full gate + falloff. Enforces (in order, req. J2/J3):
//   1. same interior AND same virtual world          -> else inaudible
//   2. 3D distance <= effective range                -> else inaudible
//   3. logarithmic falloff curve                      (req. D1)
//   4. occlusion if either party is in a closed vehicle (req. J4)
//   5. stereo pan from relative X offset              (req. D3)
ProximityResult evaluate(const Player& speaker, const Player& listener,
                         const ProximityTuning& t);

} // namespace vc::core
