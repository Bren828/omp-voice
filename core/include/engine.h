// ============================================================
//  VoiceChat core — Relay engine / routing brain (platform-free)
//  File: core/include/engine.h
//
//  The relay (.exe) is a thin transport + crypto shell around this.
//  The engine is deliberately I/O-free and socket-free so it is unit
//  testable: it consumes already-decrypted control/audio events and
//  emits already-formed PLAINTEXT payloads through a sink; the relay
//  does the AEAD + sendto.
// ============================================================
#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include "registry.h"
#include "spatial_grid.h"
#include "proximity.h"
#include "bus_router.h"
#include "opus_codec.h"

namespace vc::core {

// Authoritative client policy limits handed down at handshake (stage H). The
// relay fills these from voice.ini [policy] at boot; a CtrlConfig push from the
// .dll overrides them at runtime. opusBitrate lives in BehaviorTuning::bitrate
// and maxRange is ProximityTuning::shout, so they are not duplicated here.
struct PolicyLimits {
    uint8_t vadAllowed  = 1;   // may the client use VAD vs PTT-only
    uint8_t defaultMode = 0;   // 0 = PTT, 1 = VAD (server default)
    uint8_t maxStreams  = 8;   // cap on simultaneous decoded streams advertised
};

// Engine -> relay: "send these bytes to this player". The relay looks up
// the player's session, seals (AEAD) and sends on the audio socket.
using PacketSink = std::function<void(uint16_t toPlayer,
                                      const uint8_t* data, size_t len)>;

// Engine -> bridge: a status event for the gamemode (talking/radio key).
using StatusSink = std::function<void(StatusType, uint16_t playerId,
                                      uint16_t channelId, uint8_t flag)>;

class Engine {
public:
    explicit Engine(PacketSink sink)
        : m_sink(std::move(sink)), m_bus(m_reg, m_tune)
    {
        m_bus.setSink(m_sink);            // filtered buses share the audio sink
        // J6: the bus router hands a speakerphone member's call audio back here
        // so we can spatialise it into their proximity (we own the grid/encoders).
        m_bus.setSpeakerphoneSink([this](uint16_t holder, uint16_t chan,
                                         const float* pcm, int n, uint32_t now) {
            routeSpeakerphone(holder, chan, pcm, n, now);
        });
    }

    Registry&         registry() { return m_reg; }
    ProximityTuning&  tuning()   { return m_tune; }
    BusRouter&        bus()      { return m_bus; }   // relay applies [radio]/[behavior]
    PolicyLimits&     policy()   { return m_policy; } // stage H: client limits in the Ack
    void setStatusSink(StatusSink s) { m_status = std::move(s); }

    // Stage F (J9): cap how many simultaneous proximity speakers any one
    // listener receives — only the N nearest/loudest get through, so a dense
    // crowd can't flood a client with streams to decode. 0 = unlimited.
    void setNearestN(int n) { m_nearestN = n < 0 ? 0 : n; }

    // --- control plane (already-decrypted Ctrl* packets from the .dll) ---
    void onControl(const uint8_t* data, size_t len);

    // --- audio plane: a speaker's decoded transmit intent + Opus frame ---
    // `opus`/`opusLen` is the speaker's encoded frame as received. The
    // engine forwards it for proximity (no re-encode) and, for filtered
    // channels, hands decoded PCM to the BusMixer (stage E).
    void onAudioFrame(uint16_t speakerId, uint16_t seq, uint8_t flags,
                      const uint8_t* opus, uint16_t opusLen);

    // periodic: rebuild grid, time out silent speakers, etc.
    void tick();

private:
    void routeProximity(uint16_t speakerId, uint16_t seq, uint8_t flags,
                        const uint8_t* opus, uint16_t opusLen);
    // STAGE E: routeBuses(...) decode + filter + mix + re-encode per listener.

    // J6: encode the holder's received call PCM and fan it out as a synthetic
    // proximity speaker (id = holder | SPEAKERPHONE_ID_BIT) to nearby NON-members.
    void routeSpeakerphone(uint16_t holderId, uint16_t channelId,
                           const float* pcm, int samples, uint32_t nowMs);

    void emitStatus(StatusType e, uint16_t pid, uint16_t cid = INVALID_CHANNEL, uint8_t flag = 0)
    { if (m_status) m_status(e, pid, cid, flag); }

    // Per-listener nearest-N bookkeeping (J9). Each listener keeps a small set
    // of the speakers it currently hears with their last volume + tick; stale
    // entries age out so stopped speakers free their slot.
    struct NearEntry { uint16_t speaker; float volume; uint32_t tick; };
    // Returns true if `speaker` (at `vol`) should be forwarded to `listener`
    // under the cap, updating the listener's near-set.
    bool nearestAdmit(uint16_t listener, uint16_t speaker, float vol);

    PacketSink   m_sink;
    StatusSink   m_status;
    Registry     m_reg;
    SpatialGrid  m_grid{ 50.f };
    ProximityTuning m_tune;
    PolicyLimits m_policy;              // stage H: handshake limits (voice.ini [policy])
    BusRouter    m_bus;                 // filtered radio/phone buses (stage E)
    uint32_t     m_tickCount = 0;
    uint32_t     m_silenceTicks = 8;   // ticks of no audio => SpeakerStop
    int          m_nearestN = 8;       // J9 cap; 0 = unlimited
    std::unordered_map<uint16_t, std::vector<NearEntry>> m_near;  // listener -> set

    // J6 speakerphone: per-holder Opus encoder + proximity seq for the synthetic
    // "phone on speaker" stream injected into the holder's proximity.
    std::unordered_map<uint16_t, std::unique_ptr<OpusEncoderWrap>> m_spkEnc;
    std::unordered_map<uint16_t, uint16_t> m_spkSeq;
};

} // namespace vc::core
