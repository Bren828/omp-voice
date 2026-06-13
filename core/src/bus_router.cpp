// ============================================================
//  VoiceChat core — Filtered-bus router implementation (Stage E)
//  File: core/src/bus_router.cpp
// ============================================================
#include "../include/bus_router.h"
#include "../../shared/protocol.h"
#include <cstring>
#include <algorithm>
#include <iterator>

namespace vc::core {

static constexpr int   FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;   // 960 @48k
static constexpr int   MAX_DECODE    = SAMPLE_RATE * 60 / 1000;         // 60 ms safety
static constexpr uint32_t FRESH_MS   = 60;                              // "still talking"

// ── codec / filter accessors (lazy create) ─────────────────
OpusDecoderWrap& BusRouter::decoder(uint16_t speakerId)
{
    auto& up = m_decoders[speakerId];
    if (!up) { up = std::make_unique<OpusDecoderWrap>(); up->init(SAMPLE_RATE, VOICE_CHANNELS); }
    return *up;
}

OpusEncoderWrap& BusRouter::encoder(uint16_t listenerId, uint16_t channelId)
{
    auto& up = m_encoders[key2(listenerId, channelId)];
    if (!up) { up = std::make_unique<OpusEncoderWrap>(); up->init(SAMPLE_RATE, VOICE_CHANNELS, m_behavior.bitrate); }
    return *up;
}

BusRouter::Stream& BusRouter::stream(uint16_t speakerId, uint16_t channelId, Filter f)
{
    Stream& st = m_streams[key2(speakerId, channelId)];
    if (!st.configured || st.filter != f) {
        st.filter = f;
        if (f == Filter::Phone)
            st.band.configure((float)SAMPLE_RATE, m_radio.phoneLowHz, m_radio.phoneHighHz);
        else  // Radio (None never reaches here)
            st.band.configure((float)SAMPLE_RATE, m_radio.bandpassLowHz, m_radio.bandpassHighHz);
        st.band.reset();
        st.configured = true;
        if (st.rng == 0) st.rng = 0x2545F491u ^ (uint32_t(speakerId) * 2654435761u) ^ channelId;
    }
    return st;
}

bool BusRouter::proximityAudible(uint16_t speakerId, uint16_t listenerId)
{
    Player* s = m_reg.find(speakerId);
    Player* l = m_reg.find(listenerId);
    if (!s || !l) return false;
    return evaluate(*s, *l, m_prox).audible;
}

// ── ingest one speaker frame ─────────────────────────────────
void BusRouter::onSpeakerFrame(uint16_t speakerId, const uint8_t* opus,
                               uint16_t opusLen, uint32_t nowMs)
{
    Player* sp = m_reg.find(speakerId);
    if (!sp || sp->muted) return;
    if (!sp->transmitting || sp->channels.empty()) return;   // not keying a channel

    // Decode ONCE; reused for every channel/listener this frame.
    float pcm[MAX_DECODE];
    int ns = decoder(speakerId).decode(opus, opusLen, pcm, MAX_DECODE);
    if (ns <= 0) return;                       // no opus build / decode fail -> graceful

    for (uint16_t cid : sp->channels) {
        Channel* ch = m_reg.channel(cid);
        if (!ch || ch->filter == Filter::None) continue;            // proximity-clean elsewhere
        if (sp->channelMask && !(sp->channelMask & chanBit(cid))) continue;  // not keyed (J1)

        // Filter the speaker's frame for this channel (stateful per stream).
        Stream& st = stream(speakerId, cid, ch->filter);
        st.pcm.assign(pcm, pcm + ns);
        st.samples = ns;
        st.lastMs  = nowMs;
        st.band.process(st.pcm.data(), ns);
        dsp::drive(st.pcm.data(), ns, m_radio.drive);
        if (ch->filter == Filter::Radio)
            dsp::addStatic(st.pcm.data(), ns, m_radio.staticLevel, st.rng);

        m_chanActiveMs[cid] = nowMs;

        // Rate-limit each channel's bus to ~one frame per FRAME_MS regardless
        // of how many members feed it (a collision must not double the rate).
        uint32_t le = m_lastEmit[cid];
        if (le && nowMs - le < (uint32_t)(FRAME_MS - 2)) continue;
        m_lastEmit[cid] = nowMs;
        emitChannel(*ch, nowMs);
    }
}

// ── mix + emit one channel's bus to each member ──────────────
void BusRouter::emitChannel(Channel& ch, uint32_t nowMs)
{
    // Fresh contributors = members with a recent filtered frame on this channel.
    struct Contrib { uint16_t id; Stream* st; };
    std::vector<Contrib> contrib;
    contrib.reserve(ch.members.size());
    int ns = 0;
    for (uint16_t mid : ch.members) {
        auto it = m_streams.find(key2(mid, ch.id));
        if (it == m_streams.end()) continue;
        Stream& s = it->second;
        if (s.samples <= 0 || nowMs - s.lastMs > FRESH_MS) continue;
        contrib.push_back({ mid, &s });
        ns = std::max(ns, s.samples);
    }
    if (contrib.empty() || ns <= 0) return;

    bool collision = m_radio.halfDuplexGarble && contrib.size() >= 2;

    for (uint16_t lid : ch.members) {
        Player* li = m_reg.find(lid);
        if (!li || li->deafen) continue;                 // J11 deafen

        // Sum every OTHER contributor, dropping anyone already audible in person.
        m_scratch.assign(ns, 0.f);
        int included = 0;
        for (auto& c : contrib) {
            if (c.id == lid) continue;                   // never hear yourself
            if (m_behavior.suppressDoublePlay && proximityAudible(c.id, lid))
                continue;                                 // J5 proximity wins
            int sn = std::min(ns, c.st->samples);
            const float* p = c.st->pcm.data();
            for (int i = 0; i < sn; ++i) m_scratch[i] += p[i];
            ++included;
        }
        if (included == 0) continue;

        if (collision)
            dsp::garble(m_scratch.data(), ns, (float)SAMPLE_RATE,
                        m_garblePhase[ch.id], m_garbleRng[ch.id]);
        dsp::limiter(m_scratch.data(), ns, m_behavior.limiterCeiling);

        // J6 speakerphone: this listener has their phone "on speaker", so the
        // call audio they hear is ALSO broadcast into their proximity (handled by
        // the engine). Independent of their personal duck/exclusive mix policy.
        if (m_behavior.speakerphone && m_spk && li->speakerphone)
            m_spk(lid, ch.id, m_scratch.data(), ns, nowMs);

        float vol = busVolume(*li, ch, nowMs);
        if (vol <= 0.001f) continue;                     // exclusive policy muted it
        sendBus(lid, ch, m_scratch.data(), ns, vol);
    }
}

// Duck/exclusive across the listener's currently-active filtered buses (I4).
float BusRouter::busVolume(const Player& listener, const Channel& ch, uint32_t nowMs)
{
    uint8_t top = 0;
    for (uint16_t cid : listener.channels) {
        Channel* c = m_reg.channel(cid);
        if (!c || c->filter == Filter::None) continue;
        auto it = m_chanActiveMs.find(cid);
        if (it == m_chanActiveMs.end() || nowMs - it->second > FRESH_MS) continue;
        top = std::max(top, c->priority);
    }
    return BusMixer::policyGain(listener.policy, ch.priority, top, listener.duckLevel);
}

bool BusRouter::sendBus(uint16_t listenerId, Channel& ch, const float* pcm,
                        int samples, float volume)
{
    uint8_t opus[MAX_OPUS_BYTES];
    int bytes = encoder(listenerId, ch.id).encode(pcm, samples, opus, MAX_OPUS_BYTES);
    if (bytes <= 0) return false;                        // no opus build -> graceful

    uint8_t buf[sizeof(BusDown) + MAX_OPUS_BYTES];
    auto* hdr = reinterpret_cast<BusDown*>(buf);
    hdr->type      = (uint8_t)PktType::BusDown;
    hdr->channelId = ch.id;
    hdr->filter    = (uint8_t)ch.filter;
    hdr->seq       = ++m_seq[key2(listenerId, ch.id)];
    hdr->volume    = volume;
    hdr->length    = (uint16_t)bytes;
    std::memcpy(buf + sizeof(BusDown), opus, bytes);
    if (m_sink) m_sink(listenerId, buf, sizeof(BusDown) + (size_t)bytes);
    return true;
}

// ── squelch / roger beep on PTT key transition (E2) ──────────
void BusRouter::onRadioKey(uint16_t speakerId, uint16_t channelId, bool down, uint32_t nowMs)
{
    (void)nowMs;
    if (!m_radio.squelchBeep) return;
    Channel* ch = m_reg.channel(channelId);
    if (!ch || ch->filter != Filter::Radio) return;

    std::vector<float> beep(FRAME_SAMPLES);
    float phase = 0.f;
    float freq  = down ? 1500.f : 1200.f;       // key-down higher, key-up lower
    dsp::squelchBeep(beep.data(), FRAME_SAMPLES, (float)SAMPLE_RATE, freq, 0.30f, phase);

    for (uint16_t mid : ch->members) {
        if (mid == speakerId) continue;          // operator doesn't get their own beep
        Player* li = m_reg.find(mid);
        if (!li || li->deafen) continue;
        sendBus(mid, *ch, beep.data(), FRAME_SAMPLES, 1.f);
    }
}

// ── cleanup ──────────────────────────────────────────────────
// Keys are (high16, low16). prunePlayer drops anything where the player is the
// speaker (stream high16) or the listener (encoder/seq high16) + their decoder.
void BusRouter::prunePlayer(uint16_t pid)
{
    m_decoders.erase(pid);
    for (auto it = m_streams.begin();  it != m_streams.end();  )
        it = (uint16_t)(it->first >> 16) == pid ? m_streams.erase(it)  : std::next(it);
    for (auto it = m_encoders.begin(); it != m_encoders.end(); )
        it = (uint16_t)(it->first >> 16) == pid ? m_encoders.erase(it) : std::next(it);
    for (auto it = m_seq.begin();      it != m_seq.end();      )
        it = (uint16_t)(it->first >> 16) == pid ? m_seq.erase(it)      : std::next(it);
}

void BusRouter::pruneChannel(uint16_t cid)
{
    for (auto it = m_streams.begin();  it != m_streams.end();  )
        it = (uint16_t)it->first == cid ? m_streams.erase(it)  : std::next(it);
    for (auto it = m_encoders.begin(); it != m_encoders.end(); )
        it = (uint16_t)it->first == cid ? m_encoders.erase(it) : std::next(it);
    for (auto it = m_seq.begin();      it != m_seq.end();      )
        it = (uint16_t)it->first == cid ? m_seq.erase(it)      : std::next(it);
    m_lastEmit.erase(cid); m_chanActiveMs.erase(cid);
    m_garblePhase.erase(cid); m_garbleRng.erase(cid);
}

} // namespace vc::core
