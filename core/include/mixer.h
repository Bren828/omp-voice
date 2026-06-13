// ============================================================
//  VoiceChat core — Mix policy (req. I4)
//  File: core/include/mixer.h
//
//  HYBRID model:
//   * Proximity voice is NOT mixed here — it is forwarded per-speaker to the
//     client (volume+pan from proximity.h) and the client mixes it locally.
//   * Filtered channels (radio/phone) are decoded, filtered (dsp.h) and mixed
//     PER-LISTENER on the relay in bus_router.cpp; this header is just the
//     pure mix-policy arithmetic that decides each bus's gain.
// ============================================================
#pragma once

#include <cstdint>
#include "registry.h"

namespace vc::core {

// Post-policy gain for ONE bus, given the listener's policy and the top
// active-bus priority among the listener's buses (req. I4):
//   Mix       -> every bus at full volume
//   Duck      -> a bus below the top priority drops to duckLevel
//   Exclusive -> only the top-priority bus is heard
// Proximity is never ducked here (it is client-mixed), satisfying the
// "always hear what's physically next to you" floor.
class BusMixer {
public:
    static float policyGain(MixPolicy policy, uint8_t busPriority,
                            uint8_t topActivePriority, float duckLevel)
    {
        switch (policy) {
            case MixPolicy::Duck:
                return (busPriority >= topActivePriority) ? 1.f : duckLevel;
            case MixPolicy::Exclusive:
                return (busPriority >= topActivePriority) ? 1.f : 0.f;
            case MixPolicy::Mix:
            default:
                return 1.f;
        }
    }
};

} // namespace vc::core
