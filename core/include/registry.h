// ============================================================
//  VoiceChat core — Player & Channel model (platform-independent)
//  File: core/include/registry.h
//
//  The authoritative in-memory model shared by the relay engine and
//  (the bookkeeping half of) the game-server bridges. No sockets, no
//  SA-MP, no open.mp here.
// ============================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "../../shared/protocol.h"

namespace vc::core {

struct Vec3 { float x = 0, y = 0, z = 0; };

// One connected player as the SERVER sees them (authoritative).
struct Player {
    uint16_t id        = INVALID_PLAYER;
    Vec3     pos;
    uint16_t interior  = 0;        // req. J2 gate
    uint16_t vworld    = 0;        // req. J2 gate
    uint8_t  vehicle   = 0;        // 0 onfoot,1 open,2 closed (req. J4 occlusion)

    bool     muted     = false;    // can't transmit
    bool     deafen    = false;    // hears no one (req. J11)
    bool     alive     = true;     // req. J13 dead/spectator
    bool     spectator = false;

    RangePreset range  = RangePreset::Normal;
    float    rangeOverride = -1.f; // <=0 = use preset (already clamped by relay)

    bool     transmitting = false;
    uint32_t channelMask  = 0;     // which channels currently keyed
    bool     speakerphone = false; // J6: broadcast received call audio to proximity

    // talking detection (req. B / G1 callbacks)
    bool     audioActive  = false; // currently producing voice frames
    uint32_t lastAudioTick= 0;     // engine tick of last frame

    // mixing policy (req. I4)
    MixPolicy policy        = MixPolicy::Duck;
    float     duckLevel     = 0.5f;
    float     proximityFloor= 0.25f;

    std::unordered_set<uint16_t> channels;   // channel memberships

    // req. J11 (stage G): targets THIS player has muted for themselves only —
    // a per-pair proximity gate. Channels are unaffected.
    std::unordered_set<uint16_t> mutedTargets;
};

// A channel is the plugin's single generic primitive (req. I). Gameplay
// (phone/taxi/faction) is built in Pawn on top of these.
struct Channel {
    uint16_t id        = INVALID_CHANNEL;
    Filter   filter    = Filter::None;
    bool     positional= false;
    uint8_t  priority  = 100;
    std::unordered_set<uint16_t> members;
};

class Registry {
public:
    Player&  player(uint16_t id);              // get-or-create
    Player*  find(uint16_t id);                // nullptr if absent
    void     removePlayer(uint16_t id);        // + pull from all channels (J8)

    Channel& createChannel(uint16_t id, Filter f, bool positional, uint8_t prio);
    Channel* channel(uint16_t id);
    void     destroyChannel(uint16_t id);      // + pull all members
    void     joinChannel(uint16_t player, uint16_t channel);
    void     leaveChannel(uint16_t player, uint16_t channel);
    void     removeFromAllChannels(uint16_t player);  // req. I1 internal

    std::unordered_map<uint16_t, Player>&  players()  { return m_players; }
    std::unordered_map<uint16_t, Channel>& channels() { return m_channels; }

private:
    std::unordered_map<uint16_t, Player>  m_players;
    std::unordered_map<uint16_t, Channel> m_channels;
};

} // namespace vc::core
