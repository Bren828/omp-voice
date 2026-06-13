// ============================================================
//  VoiceChat — Relay process launcher + watchdog (req. B1)
//  File: shared/relay_launcher.h
//
//  Used by both bridges (/samp, /omp) to spawn the standalone relay
//  (.exe / ELF) as a child process and keep it alive. Header-only,
//  cross-platform. The watchdog thread (auto-restart on crash) is the
//  STAGE B deliverable; launch/kill are wired now.
// ============================================================
#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include "restart_policy.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <cstdio>
#endif

namespace vc::relay {

#ifdef _WIN32
inline PROCESS_INFORMATION g_proc{};
// Searched in order, relative to the game-server cwd. Covers the open.mp
// `components/` and `plugins/` layouts and a same-dir drop, so the relay is
// found wherever the host server keeps it.
inline const char* kRelayCandidates[] = {
    "components/voice_relay.exe",
    "plugins/voice_relay.exe",
    "voice_relay.exe",
};
#else
inline pid_t g_pid = 0;
inline const char* kRelayCandidates[] = {
    "components/voice_relay",
    "plugins/voice_relay",
    "./voice_relay",
};
#endif

inline std::atomic<bool> g_watchdogRun{ false };
inline std::thread       g_watchdog;

// First candidate that exists; falls back to [0] so a missing relay fails
// loudly (and degrades gracefully — the game runs without voice, req. J7).
inline const char* resolveRelayPath()
{
    for (const char* p : kRelayCandidates) {
#ifdef _WIN32
        DWORD a = GetFileAttributesA(p);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return p;
#else
        if (access(p, X_OK) == 0) return p;
#endif
    }
    return kRelayCandidates[0];
}

inline bool spawn()
{
    const char* path = resolveRelayPath();
#ifdef _WIN32
    STARTUPINFOA si{}; si.cb = sizeof(si);
    if (!CreateProcessA(path, nullptr, nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &g_proc))
        return false;
    return true;
#else
    g_pid = fork();
    if (g_pid < 0) return false;
    if (g_pid == 0) {
        FILE* log = fopen("voice_relay.log", "a");
        if (log) { dup2(fileno(log), STDOUT_FILENO); dup2(fileno(log), STDERR_FILENO); }
        execl(path, path, (char*)nullptr);
        _exit(127);
    }
    return true;
#endif
}

inline bool alive()
{
#ifdef _WIN32
    if (!g_proc.hProcess) return false;
    return WaitForSingleObject(g_proc.hProcess, 0) == WAIT_TIMEOUT;
#else
    if (g_pid <= 0) return false;
    int st; return waitpid(g_pid, &st, WNOHANG) == 0;
#endif
}

inline uint64_t monoMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

inline void launch(RestartTuning tuning = {})
{
    if (spawn()) { /* policy.onSpawn set below */ }

    // Watchdog auto-restart with exponential backoff + crash-loop cap (B1).
    g_watchdogRun = true;
    g_watchdog = std::thread([tuning] {
        using namespace std::chrono;
        RestartPolicy policy(tuning);
        policy.onSpawn(monoMs());
        bool gaveUp = false;
        while (g_watchdogRun) {
            std::this_thread::sleep_for(milliseconds(250));
            if (gaveUp || !g_watchdogRun) continue;
            if (alive()) continue;

            uint64_t now = monoMs();
            policy.onCrash(now);
            RestartAction act = policy.decide(now);
            if (act == RestartAction::GiveUp) {
#ifdef _WIN32
                OutputDebugStringA("[VoiceChat] relay crash-looping; watchdog gave up\n");
#else
                std::fprintf(stderr, "[VoiceChat] relay crash-looping; watchdog gave up\n");
#endif
                gaveUp = true;
                continue;
            }
            // Wait out the backoff, re-checking the shutdown flag.
            uint32_t wait = policy.backoffMs();
            for (uint32_t w = 0; w < wait && g_watchdogRun; w += 100)
                std::this_thread::sleep_for(milliseconds(100));
            if (g_watchdogRun && !alive()) {
                if (spawn()) policy.onSpawn(monoMs());
            }
        }
    });
}

inline void kill()
{
    g_watchdogRun = false;
    if (g_watchdog.joinable()) g_watchdog.join();
#ifdef _WIN32
    if (g_proc.hProcess) {
        TerminateProcess(g_proc.hProcess, 0);
        CloseHandle(g_proc.hProcess); CloseHandle(g_proc.hThread);
        g_proc = {};
    }
#else
    if (g_pid > 0) {
        ::kill(g_pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int st; if (waitpid(g_pid, &st, WNOHANG) == 0) ::kill(g_pid, SIGKILL);
        g_pid = 0;
    }
#endif
}

} // namespace vc::relay
