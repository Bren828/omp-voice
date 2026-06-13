// ============================================================
//  VoiceChat core — Bridge-side control link (platform-free)
//  File: core/include/control_link.h
//
//  Used by BOTH /samp and /omp so the native handlers are thin: they
//  translate a Pawn call into a typed method here, which serialises the
//  matching Ctrl* packet and ships it to the relay over loopback UDP.
//
//  Also owns:
//   * ChannelAllocator — hands out channel ids (req. I1 dynamic alloc).
//   * TokenStore        — mints per-connect crypto tokens (req. A1),
//                         remembers token<->playerid, and asks the bridge
//                         to deliver the token to the client over the
//                         game's authenticated channel (delivery is
//                         platform-specific, injected as a callback).
// ============================================================
#pragma once

#include <cstdint>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include "registry.h"
#include "../../shared/udp.h"
#include "../../shared/crypto.h"

namespace vc::core {

// Deliver a freshly minted token to a player over the game channel
// (RakNet packet on open.mp / SA-MP, or the legacy cmd bridge). The bridge
// supplies the concrete implementation.
using TokenDeliverFn = std::function<void(uint16_t playerId,
                                          const crypto::Token& token)>;

class ChannelAllocator {
public:
    uint16_t alloc() { return m_next++; }   // 0 reserved for global proximity
    void     reset() { m_next = 1; }
private:
    uint16_t m_next = 1;
};

class ControlLink {
public:
    bool init(const char* relayIp, uint16_t controlPort);
    void shutdown();

    // --- token lifecycle (req. A) ---
    void setTokenDeliver(TokenDeliverFn fn) { m_deliver = std::move(fn); }
    // Mint a token for a connecting player, send the binding to the relay
    // (with the expected client IP for the secondary check), and deliver the
    // token to the client. Call from OnPlayerConnect. ip = network-order
    // IPv4, 0 to disable the IP check for this player.
    void onPlayerConnect(uint16_t playerId, uint32_t ip = 0, uint32_t ttlSeconds = 30);

    // --- authoritative state push (called from gamemode natives) ---
    void pushPosition(uint16_t playerId, float x, float y, float z,
                      uint16_t interior, uint16_t vworld, uint8_t vehicleState);
    void pushMeta(uint16_t playerId, uint8_t flags,
                  uint8_t rangePreset, float rangeOverride);
    void setTransmit(uint16_t playerId, bool on, uint32_t channelMask);
    void setMixPolicy(uint16_t playerId, MixPolicy p, float duck, float proxFloor);
    // Per-pair mute (req. J11/stage G): `listenerId` stops hearing `targetId`
    // by proximity, for themselves only. on=false to unmute.
    void mutePlayer(uint16_t listenerId, uint16_t targetId, bool on);
    // Speakerphone (req. J6): while on, `playerId`'s received call audio is also
    // played into their proximity so nearby players hear it. on=false to stop.
    void setSpeakerphone(uint16_t playerId, bool on);
    // Display name for the client overlay (G2). Forwarded to the relay, which
    // fans it out to clients. Display-only; identity is still the token.
    void setPlayerName(uint16_t playerId, const char* name);

    // Stage H: push the authoritative gameplay config (read from voice.ini by
    // the bridge) to the relay over CtrlConfig. Overrides the relay's boot-time
    // defaults and is reflected in every subsequent handshake Ack.
    void pushConfig(uint16_t opusBitrate, float whisper, float normal, float shout,
                    float falloffExp, bool occlusion, bool vadAllowed,
                    uint8_t defaultMode, uint8_t maxStreams);

    // --- channel ops (mirror to local Registry + relay) ---
    uint16_t createChannel(Filter f, bool positional, uint8_t priority);
    void     destroyChannel(uint16_t channelId);
    void     joinChannel(uint16_t playerId, uint16_t channelId);
    void     leaveChannel(uint16_t playerId, uint16_t channelId);
    void     setChannelFilter(uint16_t channelId, Filter f);
    void     setChannelPriority(uint16_t channelId, uint8_t priority);

    // --- disconnect cleanup (req. I1/J8) ---
    void onPlayerDisconnect(uint16_t playerId);

    // --- relay -> bridge status (req. B) ---------------------------------
    // The relay pushes StatusEvent packets back over the SAME loopback socket
    // (talking start/stop, radio-key, session timeout). A background thread
    // collects them into a queue; the bridge drains it on its OWN thread
    // (ProcessTick / open.mp tick) because AMX / pawn calls are not thread
    // safe. Returns false when the queue is empty.
    bool nextStatus(StatusEvent& out);

    Registry& registry() { return m_reg; }

private:
    template<typename T> void send(const T& pkt);
    void statusLoop();               // bg: recv StatusEvent -> m_statusQ

    UdpSocket        m_sock;
    sockaddr_in      m_relay{};
    Registry         m_reg;          // bridge-side bookkeeping
    ChannelAllocator m_chanAlloc;
    TokenDeliverFn   m_deliver;

    std::thread             m_statusThr;
    std::atomic<bool>       m_statusRun{ false };
    std::mutex              m_statusMtx;
    std::deque<StatusEvent> m_statusQ;
};

} // namespace vc::core
