// ============================================================
//  VoiceChat — Relay restart policy (req. B1)
//  File: shared/restart_policy.h
//
//  Pure decision logic for the watchdog: given crash timestamps, decide
//  whether to respawn now, wait (exponential backoff), or give up (the
//  relay is crash-looping and respawning only makes it worse). Separated
//  from the actual process spawn so it is unit-testable with a fake clock.
// ============================================================
#pragma once

#include <cstdint>
#include <vector>

namespace vc {

struct RestartTuning {
    uint32_t baseDelayMs   = 500;     // first backoff
    uint32_t maxDelayMs    = 30000;   // cap (30s)
    uint32_t windowMs      = 60000;   // crash-loop window
    uint32_t maxInWindow   = 6;       // > this many crashes in window => give up
    uint32_t healthyAfterMs= 15000;   // alive this long => reset backoff
};

enum class RestartAction { Respawn, Wait, GiveUp };

class RestartPolicy {
public:
    explicit RestartPolicy(RestartTuning t = {}) : m_t(t) {}

    // Call when the process is observed dead at time `nowMs`.
    void onCrash(uint64_t nowMs)
    {
        // Reset backoff if the previous run was healthy long enough.
        if (m_lastSpawnMs && nowMs - m_lastSpawnMs >= m_t.healthyAfterMs)
            m_consecutive = 0;
        m_crashes.push_back(nowMs);
        ++m_consecutive;
        m_nextAllowedMs = nowMs + backoffMs();
    }

    // Call after a successful spawn so health/backoff can be tracked.
    void onSpawn(uint64_t nowMs) { m_lastSpawnMs = nowMs; }

    // What should the watchdog do at `nowMs`?
    RestartAction decide(uint64_t nowMs)
    {
        pruneWindow(nowMs);
        if (m_crashes.size() > m_t.maxInWindow) return RestartAction::GiveUp;
        if (m_nextAllowedMs && nowMs < m_nextAllowedMs) return RestartAction::Wait;
        return RestartAction::Respawn;
    }

    uint32_t backoffMs() const
    {
        // base * 2^(consecutive-1), capped.
        uint64_t d = m_t.baseDelayMs;
        for (uint32_t i = 1; i < m_consecutive && d < m_t.maxDelayMs; ++i) d *= 2;
        return (uint32_t)(d > m_t.maxDelayMs ? m_t.maxDelayMs : d);
    }

    uint32_t crashesInWindow() const { return (uint32_t)m_crashes.size(); }

private:
    void pruneWindow(uint64_t nowMs)
    {
        size_t keep = 0;
        for (uint64_t c : m_crashes)
            if (nowMs - c <= m_t.windowMs) m_crashes[keep++] = c;
        m_crashes.resize(keep);
    }

    RestartTuning         m_t;
    std::vector<uint64_t> m_crashes;
    uint32_t              m_consecutive   = 0;
    uint64_t              m_lastSpawnMs   = 0;
    uint64_t              m_nextAllowedMs = 0;
};

} // namespace vc
