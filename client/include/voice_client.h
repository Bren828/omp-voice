// ============================================================
//  VoiceChat client (.asi) — connection manager (req. B2 reconnect)
//  File: client/include/voice_client.h
//
//  Ties the token receiver (TokenInbox) to the secure Session and keeps the
//  link alive: it (re)handshakes whenever the session is down, drives the
//  heartbeat while it is up, and retries with a FRESH token after a timeout.
//
//  Reconnect flow (req. B2/J7):
//    1. Session::tick() drops `established` when no datagram arrives for the
//       rx-timeout window (the relay died / network dropped).
//    2. The relay's own heartbeat timeout makes it emit SessionTimeout to the
//       .dll, which mints a NEW token and delivers it -> TokenInbox.
//    3. This manager notices the session is down, takes the freshest token
//       from the inbox and re-handshakes. Stale-token attempts get a fast NAK
//       and back off until the new token lands.
//
//  The audio layer (WASAPI capture/playback) is unchanged: it calls
//  sendAudio() and the PlaybackHooks fill its jitter buffers.
// ============================================================
#pragma once

#include <atomic>
#include <thread>
#include <string>
#include "net_session.h"

namespace vc::client {

class VoiceClient {
public:
    void setHooks(PlaybackHooks h) { m_session.setHooks(std::move(h)); }

    // Start the token listener + the connect/reconnect worker. Non-blocking;
    // the first connection is established on the worker thread.
    bool start(const char* relayIp,
               uint16_t audioPort = DEFAULT_PORT_AUDIO,
               uint16_t cmdPort   = DEFAULT_PORT_CMD);
    void stop();

    // --- audio passthrough (called by AudioCapture) ---
    void sendAudio(uint16_t seq, uint8_t flags, const uint8_t* opus, uint16_t len)
    { m_session.sendAudio(seq, flags, opus, len); }

    bool connected() const { return m_session.established(); }
    const EffectiveLimits& limits() const { return m_session.limits(); }

private:
    void loop();

    TokenInbox        m_inbox;
    Session           m_session;
    std::string       m_relayIp;
    uint16_t          m_audioPort = DEFAULT_PORT_AUDIO;
    std::thread       m_thr;
    std::atomic<bool> m_run{ false };
};

} // namespace vc::client
