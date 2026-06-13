// ============================================================
//  VoiceChat — Relay (.exe) transport + crypto shell
//  File: relay/include/relay.h
//
//  Owns the two UDP sockets and per-client crypto sessions. All routing /
//  acoustics / mixing live in core::Engine; this class only moves bytes,
//  validates tokens, and seals/opens AEAD envelopes.
//
//  Identity = token, NOT IP (req. A). The .dll binds token->playerid on
//  the control socket; the client proves the token in the handshake; from
//  then on the player is identified by the established session, with IP as
//  a secondary sanity check only.
// ============================================================
#pragma once

#include <cstdint>
#include <unordered_map>
#include <array>
#include <vector>
#include <ctime>
#include <atomic>
#include <thread>
#include <mutex>

#include "../../shared/protocol.h"
#include "../../shared/udp.h"
#include "../../shared/crypto.h"
#include "../../core/include/engine.h"
#include "config.h"

namespace vc {

struct Session {
    uint16_t            playerId  = INVALID_PLAYER;
    sockaddr_in         addr{};
    crypto::SessionKeys keys{};
    crypto::KeyPair     self{};        // relay ephemeral keypair
    uint64_t           txCounter = 1; // outbound anti-replay counter
    uint64_t           rxLastSeen= 0; // inbound replay high-water mark
    time_t             lastSeen  = 0; // heartbeat timeout (req. B3)
    bool               established = false;
};

struct PendingToken {
    crypto::Token token;
    uint16_t      playerId;
    uint32_t      expiresUnix;
    uint32_t      expectedIp = 0;   // network-order; 0 = no IP check (req. A)
};

class Relay {
public:
    bool init(const RelayConfig& cfg = RelayConfig{});
    void run();      // blocks: spins control/audio/cleanup loops
    void stop();

private:
    void controlLoop();   // <- .dll (loopback). Drives engine + token table.
    void audioLoop();     // <- clients. Handshake + AEAD audio.
    void cleanupLoop();   // heartbeat timeouts (req. B3)

    void handleHandshake(const uint8_t* buf, int len, const sockaddr_in& from);
    void handleEncrypted(Session& s, const uint8_t* buf, int len);

    // engine sink: seal + send a plaintext payload to a player's session.
    void emitToPlayer(uint16_t playerId, const uint8_t* data, size_t len);
    // G2 overlay: fan a single id->name entry to one client (caller holds m_mtx).
    void sendName(uint16_t to, uint16_t whoId, const char* name);
    // relay -> bridge status event (loopback control plane, req. B).
    void emitStatus(StatusType ev, uint16_t playerId,
                    uint16_t channelId = INVALID_CHANNEL, uint8_t flag = 0);

    UdpSocket m_control, m_audio;
    core::Engine* m_engine = nullptr;
    RelayConfig m_cfg;
    sockaddr_in m_bridgeAddr{};       // learned from the .dll's control packets
    bool        m_haveBridge = false;

    std::mutex m_mtx;
    std::unordered_map<uint16_t, Session> m_sessions;             // by playerId
    std::unordered_map<uint64_t, uint16_t> m_addrIndex;           // packed addr -> playerId
    std::vector<PendingToken> m_pending;                          // token table
    std::unordered_map<uint16_t, std::array<char, MAX_NAME_LEN>> m_names; // G2 roster

    std::atomic<bool> m_run{ false };
};

} // namespace vc
