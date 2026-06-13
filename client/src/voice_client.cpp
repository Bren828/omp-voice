// ============================================================
//  VoiceChat client — connection manager implementation (req. B2)
//  File: client/src/voice_client.cpp
// ============================================================
#include "../include/voice_client.h"
#include <algorithm>
#include <chrono>

namespace vc::client {

bool VoiceClient::start(const char* relayIp, uint16_t audioPort, uint16_t cmdPort)
{
    m_relayIp   = relayIp;
    m_audioPort = audioPort;
    if (!m_inbox.start(cmdPort)) return false;
    m_run = true;
    m_thr = std::thread([this] { loop(); });
    return true;
}

void VoiceClient::stop()
{
    m_run = false;
    if (m_thr.joinable()) m_thr.join();
    m_session.disconnect();
    m_inbox.stop();
}

void VoiceClient::loop()
{
    using namespace std::chrono;
    const uint32_t baseBackoff = 250, maxBackoff = 3000;
    uint32_t backoff = baseBackoff;

    while (m_run) {
        if (m_session.established()) {
            m_session.tick();              // heartbeat + rx-timeout watchdog
            backoff = baseBackoff;         // reset for the next outage
            std::this_thread::sleep_for(milliseconds(200));
            continue;
        }

        // Session down: (re)connect using the freshest delivered token.
        crypto::Token tok; uint16_t pid;
        if (!m_inbox.latest(tok, pid)) {
            std::this_thread::sleep_for(milliseconds(200));  // token not here yet
            continue;
        }

        if (m_session.connect(m_relayIp.c_str(), m_audioPort, tok)) {
            backoff = baseBackoff;         // connected
            continue;
        }

        // Timed out or NAK (likely a stale single-use token mid-reconnect).
        // Back off, then retry; the server re-mints on its own timeout and the
        // fresh token arrives in the inbox for the next attempt.
        for (uint32_t w = 0; w < backoff && m_run; w += 50)
            std::this_thread::sleep_for(milliseconds(50));
        backoff = std::min<uint32_t>(backoff * 2, maxBackoff);
    }
}

} // namespace vc::client
