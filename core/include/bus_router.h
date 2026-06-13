// ============================================================
//  VoiceChat core — Filtered-bus router (req. E, I4, J1, J5)
//  File: core/include/bus_router.h
//
//  The server half of the HYBRID model. Proximity voice is forwarded raw by
//  the engine; the FILTERED channels (radio/phone) are handled here:
//
//    speaker frame ──decode once──► per-(speaker,channel) filter (dsp.h)
//        │                                   │
//        │   per channel, ~once per 20 ms:   ▼
//        └─► for each listener-member: sum the OTHER members' filtered PCM
//            (minus anyone already audible in person, J5), garble on a
//            half-duplex collision (E4), limit (J12), apply duck/exclusive
//            policy (I4), re-encode, emit one BusDown.
//
//  Routing rules honoured:
//   * J1  voice goes to proximity (clean) AND to channels the speaker keys;
//         channel audio reaches ONLY channel members.
//   * J5  a source already audible by proximity is suppressed in the bus
//         (proximity wins — no echo when your call partner is next to you).
//   * I4  duck/exclusive across the listener's buses; proximity never ducked.
//
//  Degrades gracefully: without libopus (VC_WITH_OPUS off) decode/encode
//  return 0 and this becomes a no-op — proximity voice is unaffected (J7).
// ============================================================
#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

#include "registry.h"
#include "proximity.h"
#include "mixer.h"
#include "dsp.h"
#include "opus_codec.h"

namespace vc::core {

// Radio/phone colouring (mirrors voice.ini [radio]).
struct RadioTuning {
    float bandpassLowHz   = 300.f;
    float bandpassHighHz   = 3000.f;
    float phoneLowHz      = 300.f;
    float phoneHighHz      = 3400.f;
    float drive           = 0.35f;   // radio "edge" (soft saturation)
    bool  squelchBeep     = true;
    float staticLevel     = 0.05f;
    bool  halfDuplexGarble= true;
};

// Mixing/safety behaviour (mirrors voice.ini [behavior]/[audio]).
struct BehaviorTuning {
    bool  suppressDoublePlay = true;   // J5
    bool  speakerphone       = false;  // J6 (reserved)
    bool  normalize          = true;
    float limiterCeiling     = 0.95f;  // J12 anti-earrape
    int   bitrate            = 24000;  // bus encode bitrate
};

// Emit a finished BusDown plaintext payload to a listener (relay seals + sends).
using BusSink = std::function<void(uint16_t toPlayer, const uint8_t* data, size_t len)>;

// J6 speakerphone: hand the call audio a member HEARS (filtered, post-mix) back
// to the engine so it can inject it into that member's proximity. The engine
// owns the spatial grid + proximity encode, so the router just produces the PCM.
using SpeakerphoneSink = std::function<void(uint16_t holderId, uint16_t channelId,
                                            const float* pcm, int samples, uint32_t nowMs)>;

class BusRouter {
public:
    BusRouter(Registry& reg, ProximityTuning& prox) : m_reg(reg), m_prox(prox) {}

    void setSink(BusSink s) { m_sink = std::move(s); }
    void setSpeakerphoneSink(SpeakerphoneSink s) { m_spk = std::move(s); }   // J6
    RadioTuning&    radio()    { return m_radio; }
    BehaviorTuning& behavior() { return m_behavior; }

    // A speaker's encoded frame -> the filtered channels they are keying (J1).
    void onSpeakerFrame(uint16_t speakerId,
                        const uint8_t* opus, uint16_t opusLen, uint32_t nowMs);

    // PTT key transition on a radio channel -> squelch/roger beep to members (E2).
    void onRadioKey(uint16_t speakerId, uint16_t channelId, bool down, uint32_t nowMs);

    // Cleanup (J8): drop all codec/filter state for a player or a channel.
    void prunePlayer(uint16_t playerId);
    void pruneChannel(uint16_t channelId);

private:
    static uint32_t key2(uint16_t a, uint16_t b) { return (uint32_t(a) << 16) | b; }
    static uint32_t chanBit(uint16_t c) { return 1u << (c & 31); }

    struct Stream {                 // per (speaker, channel)
        dsp::BandpassChain band;
        Filter   filter    = Filter::None;
        bool     configured= false;
        std::vector<float> pcm;     // last filtered frame
        int      samples   = 0;
        uint32_t lastMs    = 0;
        uint32_t rng       = 0;
    };

    OpusDecoderWrap& decoder(uint16_t speakerId);
    OpusEncoderWrap& encoder(uint16_t listenerId, uint16_t channelId);
    Stream&          stream(uint16_t speakerId, uint16_t channelId, Filter f);

    bool  proximityAudible(uint16_t speakerId, uint16_t listenerId);
    void  emitChannel(Channel& ch, uint32_t nowMs);
    float busVolume(const Player& listener, const Channel& ch, uint32_t nowMs);
    bool  sendBus(uint16_t listenerId, Channel& ch, const float* pcm, int samples,
                  float volume);

    Registry&        m_reg;
    ProximityTuning& m_prox;
    RadioTuning      m_radio;
    BehaviorTuning   m_behavior;
    BusSink          m_sink;
    SpeakerphoneSink m_spk;          // J6 (optional; engine wires it)

    std::unordered_map<uint16_t, std::unique_ptr<OpusDecoderWrap>> m_decoders; // speaker
    std::unordered_map<uint32_t, std::unique_ptr<OpusEncoderWrap>> m_encoders; // (lis,chan)
    std::unordered_map<uint32_t, Stream>   m_streams;        // (speaker, channel)
    std::unordered_map<uint16_t, uint32_t> m_lastEmit;       // channel -> emit ms
    std::unordered_map<uint16_t, uint32_t> m_chanActiveMs;   // channel -> fresh ms
    std::unordered_map<uint16_t, float>    m_garblePhase;    // channel
    std::unordered_map<uint16_t, uint32_t> m_garbleRng;      // channel
    std::unordered_map<uint32_t, uint16_t> m_seq;            // (lis,chan) -> seq
    std::vector<float> m_scratch;
};

} // namespace vc::core
