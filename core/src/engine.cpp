// ============================================================
//  VoiceChat core — Engine implementation
//  File: core/src/engine.cpp
//
//  Proximity routing is implemented (req. D/F/J2-J5). Filtered-bus
//  decode+mix is stubbed and marked STAGE E.
// ============================================================
#include "../include/engine.h"
#include "../../shared/protocol.h"
#include <cstring>
#include <vector>
#include <chrono>

namespace vc::core {

// Monotonic milliseconds for the bus router's frame throttle / freshness.
static uint32_t monoMs()
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

void Engine::onControl(const uint8_t* data, size_t len)
{
    if (len < 1) return;
    auto type = (PktType)data[0];

    switch (type) {
    case PktType::CtrlPlayerPos: {
        if (len < sizeof(CtrlPlayerPos)) return;
        CtrlPlayerPos p; std::memcpy(&p, data, sizeof(p));
        Player& pl = m_reg.player(p.playerId);
        pl.pos = { p.x, p.y, p.z };
        pl.interior = p.interior; pl.vworld = p.virtualWorld;
        pl.vehicle = p.vehicleState;
        m_grid.update(pl);              // stage F: incremental cell maintenance
        break;
    }
    case PktType::CtrlPlayerMeta: {
        if (len < sizeof(CtrlPlayerMeta)) return;
        CtrlPlayerMeta m; std::memcpy(&m, data, sizeof(m));
        Player& pl = m_reg.player(m.playerId);
        pl.muted     = m.flags & 1;
        pl.deafen    = m.flags & 2;
        pl.alive     = m.flags & 4;
        pl.spectator = m.flags & 8;
        pl.range = (RangePreset)m.rangePreset;
        // Clamp range override to the authoritative ceiling (req. H gold rule).
        pl.rangeOverride = (m.rangeOverride > 0.f)
            ? (m.rangeOverride < m_tune.shout ? m.rangeOverride : m_tune.shout)
            : -1.f;
        break;
    }
    case PktType::CtrlChanCreate: {
        if (len < sizeof(CtrlChanCreate)) return;
        CtrlChanCreate c; std::memcpy(&c, data, sizeof(c));
        m_reg.createChannel(c.channelId, (Filter)c.filter, c.positional != 0, c.priority);
        break;
    }
    case PktType::CtrlChanDestroy: {
        if (len < sizeof(CtrlChanOp)) return;
        CtrlChanOp c; std::memcpy(&c, data, sizeof(c));
        m_reg.destroyChannel(c.channelId);
        m_bus.pruneChannel(c.channelId);     // drop codec/filter state (J8)
        break;
    }
    case PktType::CtrlChanJoin: {
        if (len < sizeof(CtrlChanOp)) return;
        CtrlChanOp c; std::memcpy(&c, data, sizeof(c));
        m_reg.joinChannel(c.playerId, c.channelId);
        break;
    }
    case PktType::CtrlChanLeave: {
        if (len < sizeof(CtrlChanOp)) return;
        CtrlChanOp c; std::memcpy(&c, data, sizeof(c));
        m_reg.leaveChannel(c.playerId, c.channelId);
        break;
    }
    case PktType::CtrlChanFilter: {
        if (len < sizeof(CtrlChanOp)) return;
        CtrlChanOp c; std::memcpy(&c, data, sizeof(c));
        if (Channel* ch = m_reg.channel(c.channelId)) ch->filter = (Filter)c.arg;
        break;
    }
    case PktType::CtrlChanPrio: {
        if (len < sizeof(CtrlChanOp)) return;
        CtrlChanOp c; std::memcpy(&c, data, sizeof(c));
        if (Channel* ch = m_reg.channel(c.channelId)) ch->priority = c.arg;
        break;
    }
    case PktType::CtrlTransmit: {
        if (len < sizeof(CtrlTransmit)) return;
        CtrlTransmit t; std::memcpy(&t, data, sizeof(t));
        Player& pl = m_reg.player(t.playerId);
        bool was = pl.transmitting;
        pl.transmitting = t.on != 0;
        pl.channelMask  = t.channelMask;
        // PTT press/release on a radio channel -> OnPlayerRadioKey (req. G1)
        // + a squelch/roger beep to the channel's listeners (req. E2).
        if (was != pl.transmitting) {
            uint32_t now = monoMs();
            for (auto& [cid, ch] : m_reg.channels()) {
                if (ch.filter != Filter::Radio) continue;
                if (!ch.members.count(t.playerId)) continue;
                emitStatus(StatusType::RadioKey, t.playerId, cid, pl.transmitting ? 1 : 0);
                m_bus.onRadioKey(t.playerId, cid, pl.transmitting, now);
            }
        }
        break;
    }
    case PktType::CtrlMixPolicy: {
        if (len < sizeof(CtrlMixPolicy)) return;
        CtrlMixPolicy m; std::memcpy(&m, data, sizeof(m));
        Player& pl = m_reg.player(m.playerId);
        pl.policy = (MixPolicy)m.policy;
        pl.duckLevel = m.duckLevel;
        pl.proximityFloor = m.proximityFloor;
        break;
    }
    case PktType::CtrlConfig: {
        if (len < sizeof(CtrlConfig)) return;
        CtrlConfig c; std::memcpy(&c, data, sizeof(c));
        // Stage H: the .dll pushes the authoritative gameplay config — apply ALL
        // fields. Proximity tuning feeds routing; the policy limits + bitrate are
        // read back into every subsequent handshake Ack (see Relay::handshake).
        m_tune.whisper = c.proxWhisper;
        m_tune.normal  = c.proxNormal;
        m_tune.shout   = c.proxShout;
        m_tune.falloffExp = c.falloffExp;
        m_tune.occlusion  = c.occlusionEnabled != 0;
        m_bus.behavior().bitrate = c.opusBitrate;
        m_policy.vadAllowed  = c.vadAllowed;
        m_policy.defaultMode = c.defaultMode;
        m_policy.maxStreams  = c.maxConcurrentStreams;
        break;
    }
    case PktType::CtrlPairMute: {
        if (len < sizeof(CtrlPairMute)) return;
        CtrlPairMute m; std::memcpy(&m, data, sizeof(m));
        Player& li = m_reg.player(m.listenerId);     // listener owns the mute set
        if (m.on) li.mutedTargets.insert(m.targetId);
        else      li.mutedTargets.erase(m.targetId);
        break;
    }
    case PktType::CtrlSpeakerphone: {
        if (len < sizeof(CtrlSpeakerphone)) return;
        CtrlSpeakerphone m; std::memcpy(&m, data, sizeof(m));
        Player& pl = m_reg.player(m.playerId);
        pl.speakerphone = m.on != 0;                 // J6 (gated by [behavior] too)
        break;
    }
    case PktType::CtrlPlayerGone: {
        if (len < sizeof(CtrlChanOp)) return;
        CtrlChanOp c; std::memcpy(&c, data, sizeof(c));
        m_reg.removePlayer(c.playerId);   // req. J8 full cleanup
        m_bus.prunePlayer(c.playerId);    // + codec/filter state
        m_grid.remove(c.playerId);        // stage F: pull from the spatial grid
        m_near.erase(c.playerId);         // drop their nearest-N listener set
        m_spkEnc.erase(c.playerId);       // J6: drop speakerphone encoder + seq
        m_spkSeq.erase(c.playerId);
        // Drop the id from everyone's per-pair mute set — SA-MP reuses player
        // ids, so a stale mute would wrongly silence whoever takes the slot.
        for (auto& [pid, pl] : m_reg.players()) pl.mutedTargets.erase(c.playerId);
        break;
    }
    default: break;
    }
}

void Engine::onAudioFrame(uint16_t speakerId, uint16_t seq, uint8_t flags,
                          const uint8_t* opus, uint16_t opusLen)
{
    Player* sp = m_reg.find(speakerId);
    if (!sp || sp->muted) return;
    if (!sp->alive && !sp->spectator) {
        // req. J13: dead don't transmit on proximity by default. Channels
        // (gameplay) may still carry them — handled in routeBuses (stage E).
    }

    // Talking detection (req. B / G1): first frame after silence -> Start.
    if (!sp->audioActive) {
        sp->audioActive = true;
        emitStatus(StatusType::SpeakerStart, speakerId);
    }
    sp->lastAudioTick = m_tickCount;

    routeProximity(speakerId, seq, flags, opus, opusLen);

    // Filtered channels (radio/phone): decode once, filter + mix per listener,
    // re-encode + emit BusDown. No-op without libopus (proximity unaffected).
    m_bus.onSpeakerFrame(speakerId, opus, opusLen, monoMs());
}

void Engine::routeProximity(uint16_t speakerId, uint16_t seq, uint8_t flags,
                            const uint8_t* opus, uint16_t opusLen)
{
    Player* sp = m_reg.find(speakerId);
    if (!sp) return;
    if (!sp->alive && !sp->spectator) return;   // req. J13

    float range = effectiveRange(*sp, m_tune);

    std::vector<uint16_t> neigh;
    neigh.reserve(64);
    m_grid.queryNeighbours(*sp, range, neigh);

    // Build the ProximityDown header + opus payload once; volume/pan vary.
    uint8_t buf[sizeof(ProximityDown) + MAX_OPUS_BYTES];
    auto* hdr = reinterpret_cast<ProximityDown*>(buf);
    hdr->type = (uint8_t)PktType::ProximityDown;
    hdr->speakerId = speakerId;
    hdr->seq = seq;
    if (opusLen > MAX_OPUS_BYTES) return;
    std::memcpy(buf + sizeof(ProximityDown), opus, opusLen);
    hdr->length = opusLen;

    for (uint16_t lid : neigh) {
        Player* li = m_reg.find(lid);
        if (!li || li->deafen) continue;          // req. J11 deafen
        if (li->mutedTargets.count(speakerId)) continue;   // per-pair mute (J11/G)

        ProximityResult pr = evaluate(*sp, *li, m_tune);
        if (!pr.audible) continue;

        // J9 nearest-N: drop this speaker for listeners already hearing N louder
        // ones, so a crowd can't flood any single client with streams.
        if (!nearestAdmit(lid, speakerId, pr.volume)) continue;

        hdr->flags  = flags | (pr.occluded ? AF_OCCLUDED : 0);
        hdr->volume = pr.volume;
        hdr->pan    = pr.pan;
        size_t total = sizeof(ProximityDown) + opusLen;
        m_sink(lid, buf, total);
    }
}

// J6 speakerphone: the bus router handed us the filtered call audio that
// `holderId` hears on `channelId`. Encode it and fan it out as a synthetic
// proximity speaker (id = holder | SPEAKERPHONE_ID_BIT) emanating from the
// holder's position, so nearby NON-members hear the call "out of the phone".
void Engine::routeSpeakerphone(uint16_t holderId, uint16_t channelId,
                               const float* pcm, int samples, uint32_t /*nowMs*/)
{
    Player* sp = m_reg.find(holderId);
    if (!sp || samples <= 0) return;
    if (!sp->alive && !sp->spectator) return;            // J13: dead phone stays silent

    // Encode the holder's call mix once (48k mono, bus bitrate).
    auto& enc = m_spkEnc[holderId];
    if (!enc) { enc = std::make_unique<OpusEncoderWrap>();
                enc->init(SAMPLE_RATE, VOICE_CHANNELS, m_bus.behavior().bitrate); }
    uint8_t opus[MAX_OPUS_BYTES];
    int bytes = enc->encode(pcm, samples, opus, MAX_OPUS_BYTES);
    if (bytes <= 0) return;                               // no opus build -> graceful

    const uint16_t synthId = holderId | SPEAKERPHONE_ID_BIT;
    Channel* ch = m_reg.channel(channelId);

    float range = effectiveRange(*sp, m_tune);
    std::vector<uint16_t> neigh; neigh.reserve(64);
    m_grid.queryNeighbours(*sp, range, neigh);

    uint8_t buf[sizeof(ProximityDown) + MAX_OPUS_BYTES];
    auto* hdr = reinterpret_cast<ProximityDown*>(buf);
    hdr->type = (uint8_t)PktType::ProximityDown;
    hdr->speakerId = synthId;
    hdr->seq = ++m_spkSeq[holderId];
    std::memcpy(buf + sizeof(ProximityDown), opus, bytes);
    hdr->length = (uint16_t)bytes;

    for (uint16_t lid : neigh) {
        if (lid == holderId) continue;                   // holder hears the bus directly
        Player* li = m_reg.find(lid);
        if (!li || li->deafen) continue;
        if (ch && ch->members.count(lid)) continue;      // members already hear the call
        if (li->mutedTargets.count(holderId)) continue;  // per-pair mute follows the holder

        ProximityResult pr = evaluate(*sp, *li, m_tune);
        if (!pr.audible) continue;
        if (!nearestAdmit(lid, synthId, pr.volume)) continue;

        hdr->flags  = AF_SPEAKERPH | (pr.occluded ? AF_OCCLUDED : 0);
        hdr->volume = pr.volume;
        hdr->pan    = pr.pan;
        m_sink(lid, buf, sizeof(ProximityDown) + (size_t)bytes);
    }
}

// Per-listener nearest-N admission (J9). Entries older than a couple of ticks
// are pruned so a speaker that went quiet frees its slot. With a free slot the
// speaker is admitted; when full, it replaces the weakest only if louder.
bool Engine::nearestAdmit(uint16_t listener, uint16_t speaker, float vol)
{
    if (m_nearestN <= 0) return true;            // unlimited

    constexpr uint32_t kFreshTicks = 2;          // ~2 ticks (≈400ms) of grace
    auto& set = m_near[listener];

    // Prune stale + locate this speaker + track the weakest in one pass.
    int weakest = -1; float weakestVol = 1e9f; int self = -1;
    for (size_t i = 0; i < set.size(); ) {
        if (m_tickCount - set[i].tick > kFreshTicks) {
            set[i] = set.back(); set.pop_back(); continue;   // swap-erase stale
        }
        if (set[i].speaker == speaker) self = (int)i;
        if (set[i].volume < weakestVol) { weakestVol = set[i].volume; weakest = (int)i; }
        ++i;
    }

    if (self >= 0) {                             // already heard: refresh
        set[self].volume = vol; set[self].tick = m_tickCount;
        return true;
    }
    if ((int)set.size() < m_nearestN) {          // free slot
        set.push_back({ speaker, vol, m_tickCount });
        return true;
    }
    if (weakest >= 0 && vol > weakestVol) {      // louder than the weakest -> evict
        set[weakest] = { speaker, vol, m_tickCount };
        return true;
    }
    return false;                               // listener is at capacity, quieter
}

void Engine::tick()
{
    // Stage F: the grid is maintained INCREMENTALLY (update/remove on pos +
    // disconnect), so no O(n) rebuild per tick. A rare full rebuild self-heals
    // against any drift (e.g. a missed update).
    ++m_tickCount;
    if (m_tickCount % 1024 == 0) m_grid.rebuild(m_reg.players());

    // SpeakerStop: a talker who hasn't produced a frame for m_silenceTicks
    // is no longer talking -> OnPlayerStopTalking (req. B / G1).
    for (auto& [id, pl] : m_reg.players()) {
        if (pl.audioActive && m_tickCount - pl.lastAudioTick >= m_silenceTicks) {
            pl.audioActive = false;
            emitStatus(StatusType::SpeakerStop, id);
        }
    }
}

} // namespace vc::core
