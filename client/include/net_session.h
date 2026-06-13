// ============================================================
//  VoiceChat client (.asi) — secure network session (req. A, B2, B3)
//  File: client/include/net_session.h
//
//  Replaces the old IP-based NetworkClient. Flow:
//    1. The .dll mints a token and ships it over the game channel; a tiny
//       receiver (TokenInbox) listens on the cmd port for [ 'T','K' ] +
//       playerid + token.
//    2. connect(): generate an X25519 ephemeral keypair, send HandshakeReq
//       (token + clientPubKey) IN CLEARTEXT (the token is the secret), wait
//       for HandshakeAck, derive the AEAD session key.
//    3. All subsequent audio in/out is XChaCha20-Poly1305 sealed.
//    4. Heartbeat ping + auto-reconnect with a fresh token on timeout.
//
//  Pure transport/security: WASAPI capture and playback are unchanged and
//  call sendAudio() / drain the playback queues this fills.
// ============================================================
#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <cstring>

#include "../../shared/protocol.h"
#include "../../shared/udp.h"
#include "../../shared/crypto.h"

namespace vc::client {

// Decoded-stream callbacks into the audio playback layer.
struct PlaybackHooks {
    // proximity: client spatialises (volume+pan) and mixes locally.
    std::function<void(uint16_t speaker, uint16_t seq, uint8_t flags,
                       float volume, float pan,
                       const uint8_t* opus, uint16_t len)> onProximity;
    // bus: already filtered+mixed on the server; just play at category vol.
    std::function<void(uint16_t channel, uint8_t filter, uint16_t seq,
                       float volume, const uint8_t* opus, uint16_t len)> onBus;
    std::function<void(uint16_t speaker)> onGone;
    // roster: id -> display name for the overlay (G2). name is null-terminated.
    std::function<void(uint16_t playerId, const char* name)> onName;
};

// Effective authoritative limits handed down at handshake (req. H).
struct EffectiveLimits {
    uint16_t playerId   = INVALID_PLAYER;
    bool     vadAllowed = false;
    uint8_t  defaultMode= 0;     // 0 PTT / 1 VAD
    float    maxRange   = 40.f;
    uint16_t opusBitrate= 24000;
    uint8_t  maxStreams = 8;
};

class TokenInbox {
public:
    bool start(uint16_t cmdPort = DEFAULT_PORT_CMD);
    void stop();
    // Returns true and fills `out` if a token has been received.
    bool latest(crypto::Token& out, uint16_t& playerId);
private:
    UdpSocket m_sock;
    std::thread m_thr;
    std::atomic<bool> m_run{ false };
    std::mutex m_mtx;
    bool m_have = false;
    crypto::Token m_tok{};
    uint16_t m_pid = INVALID_PLAYER;
};

class Session {
public:
    void setHooks(PlaybackHooks h) { m_hooks = std::move(h); }
    const EffectiveLimits& limits() const { return m_limits; }
    bool established() const { return m_established.load(); }

    // Connect using a token already received via TokenInbox.
    bool connect(const char* relayIp, uint16_t audioPort,
                 const crypto::Token& token);
    void disconnect();

    // Called by AudioCapture per encoded Opus frame.
    void sendAudio(uint16_t seq, uint8_t flags, const uint8_t* opus, uint16_t len);

    void tick();   // heartbeat + reconnect bookkeeping (call from main loop)

private:
    void recvLoop();
    void sendSealed(const uint8_t* plain, size_t len);

    UdpSocket   m_sock;
    sockaddr_in m_server{};
    crypto::KeyPair     m_self{};
    crypto::SessionKeys m_keys{};
    uint64_t    m_tx = 1;
    uint64_t    m_rxLastSeen = 0;
    EffectiveLimits m_limits;
    PlaybackHooks   m_hooks;

    std::thread m_recv;
    std::atomic<bool> m_run{ false };
    std::atomic<bool> m_established{ false };
    std::atomic<bool> m_nak{ false };     // relay rejected the last handshake
    uint32_t m_lastPing = 0;
    uint32_t m_lastRx   = 0;
};

} // namespace vc::client
