// ============================================================
//  VoiceChat client — connection manager implementation (req. B2)
//  File: client/src/voice_client.cpp
// ============================================================
#include "../include/voice_client.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace vc::client {

bool VoiceClient::start(const char* relayIp, uint16_t audioPort, uint16_t cmdPort)
{
    m_relayIp   = relayIp;
    m_audioPort = audioPort;

    std::printf("[voice] TokenInbox start begin cmd=%u\n", (unsigned)cmdPort);
    if (!m_inbox.start(cmdPort)) {
        std::printf("[voice] !!! TokenInbox start FAILED cmd=%u !!!\n", (unsigned)cmdPort);
        return false;
    }
    std::printf("[voice] TokenInbox start done cmd=%u\n", (unsigned)cmdPort);

    m_run = true;
    m_thr = std::thread([this] { loop(); });
    std::printf("[voice] connection thread started relay=%s audio=%u cmd=%u\n",
                m_relayIp.c_str(), (unsigned)m_audioPort, (unsigned)cmdPort);
    return true;
}

void VoiceClient::stop()
{
    std::printf("[voice] stop begin\n");
    m_run = false;
    if (m_thr.joinable()) m_thr.join();
    m_session.disconnect();
    m_inbox.stop();
    std::printf("[voice] stop done\n");
}

void VoiceClient::loop()
{
    using namespace std::chrono;
    const uint32_t baseBackoff = 250, maxBackoff = 3000;
    uint32_t backoff = baseBackoff;
    bool loggedWaitingToken = false;
    bool loggedDisconnected = false;

    std::printf("[voice] connection loop entered\n");

    while (m_run) {
        if (m_session.established()) {
            if (!loggedDisconnected) {
                std::printf("[voice] ONLINE\n");
                loggedDisconnected = false;
            }
            m_session.tick();
            backoff = baseBackoff;
            std::this_thread::sleep_for(milliseconds(200));
            continue;
        }

        if (!loggedDisconnected) {
            std::printf("[voice] OFFLINE\n");
            loggedDisconnected = true;
        }

        crypto::Token tok; uint16_t pid;
        if (!m_inbox.latest(tok, pid)) {
            if (!loggedWaitingToken) {
                std::printf("[voice] waiting for token from server plugin\n");
                loggedWaitingToken = true;
            }
            std::this_thread::sleep_for(milliseconds(200));
            continue;
        }
        loggedWaitingToken = false;

        std::printf("[voice] handshake attempt player=%u -> %s:%u\n",
                    (unsigned)pid, m_relayIp.c_str(), (unsigned)m_audioPort);

        if (m_session.connect(m_relayIp.c_str(), m_audioPort, tok)) {
            std::printf("[voice] handshake/connect SUCCESS player=%u\n", (unsigned)pid);
            backoff = baseBackoff;
            continue;
        }

        std::printf("[voice] handshake/connect FAILED; retry in %u ms\n", (unsigned)backoff);
        for (uint32_t w = 0; w < backoff && m_run; w += 50)
            std::this_thread::sleep_for(milliseconds(50));
        backoff = std::min<uint32_t>(backoff * 2, maxBackoff);
    }

    std::printf("[voice] connection loop exit\n");
}

} // namespace vc::client
