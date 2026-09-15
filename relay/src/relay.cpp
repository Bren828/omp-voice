// ============================================================
//  VoiceChat — Relay implementation
//  File: relay/src/relay.cpp
// ============================================================
#include "../include/relay.h"
#include <cstring>
#include <iostream>
#include <chrono>
#include <algorithm>

namespace vc {

static uint64_t packAddr(const sockaddr_in& a)
{
    return (uint64_t(a.sin_addr.s_addr) << 16) | a.sin_port;
}

bool Relay::init(const RelayConfig& cfg)
{
    m_cfg = cfg;
    if (!crypto::init()) { std::cerr << "[relay] libsodium init failed\n"; return false; }
    if (!m_control.open() || !m_control.bindAny(cfg.controlPort)) {
        std::cerr << "[relay] bind control :" << cfg.controlPort << " failed\n"; return false;
    }
    if (!m_audio.open() || !m_audio.bindAny(cfg.audioPort)) {
        std::cerr << "[relay] bind audio :" << cfg.audioPort << " failed\n"; return false;
    }
    m_control.setRecvTimeout(200);
    m_audio.setRecvTimeout(200);

    m_engine = new core::Engine(
        [this](uint16_t to, const uint8_t* d, size_t n) { emitToPlayer(to, d, n); });
    m_engine->setStatusSink(
        [this](StatusType e, uint16_t pid, uint16_t cid, uint8_t f)
        { emitStatus(e, pid, cid, f); });

    auto& t = m_engine->tuning();
    t.whisper = cfg.whisper; t.normal = cfg.normal; t.shout = cfg.shout;
    t.falloffExp = cfg.falloffExp; t.occlusion = cfg.occlusion;
    t.occludeAtten = cfg.occludeAtten; t.panStrength = cfg.panStrength;
    m_engine->setNearestN(cfg.nearestN);

    auto& rt = m_engine->bus().radio();
    rt.bandpassLowHz = cfg.bandpassLowHz; rt.bandpassHighHz = cfg.bandpassHighHz;
    rt.squelchBeep = cfg.squelchBeep; rt.staticLevel = cfg.staticLevel;
    rt.halfDuplexGarble = cfg.halfDuplexGarble;
    auto& bt = m_engine->bus().behavior();
    bt.suppressDoublePlay = cfg.suppressDoublePlay; bt.speakerphone = cfg.speakerphone;
    bt.normalize = cfg.normalize; bt.limiterCeiling = cfg.limiterCeiling;
    bt.bitrate = cfg.bitrate;

    auto& pol = m_engine->policy();
    pol.vadAllowed  = cfg.vadAllowed ? 1 : 0;
    pol.defaultMode = (uint8_t)cfg.defaultMode;
    pol.maxStreams  = (uint8_t)cfg.maxConcurrentStreams;

    std::cout << "[relay] up. control :" << cfg.controlPort
              << "  audio :" << cfg.audioPort
              << "  | encrypt=" << (cfg.encryptAudio ? "on" : "OFF")
              << " ip_check=" << (cfg.ipSecondaryCheck ? "on" : "off")
              << " max=" << cfg.maxPlayers << "\n";
    return true;
}

void Relay::run()
{
    m_run = true;
    std::thread ctl([this]{ controlLoop(); });
    std::thread cln([this]{ cleanupLoop(); });
    audioLoop();
    m_run = false;
    ctl.join(); cln.join();
}

void Relay::stop() { m_run = false; }

void Relay::controlLoop()
{
    uint8_t buf[600];
    sockaddr_in from{};
    while (m_run) {
        int n = m_control.recvFrom(buf, sizeof(buf), from);
        if (n <= 0) continue;

        std::lock_guard<std::mutex> lk(m_mtx);
        m_bridgeAddr = from; m_haveBridge = true;

        if (n >= (int)sizeof(CtrlBindToken) &&
            (PktType)buf[0] == PktType::CtrlBindToken) {
            CtrlBindToken b; std::memcpy(&b, buf, sizeof(b));
            PendingToken pt; std::memcpy(pt.token.data(), b.token, TOKEN_BYTES);
            pt.playerId = b.playerId; pt.expiresUnix = b.expiresUnix;
            pt.expectedIp = b.ip;

            m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                [&](const PendingToken& o){ return o.playerId == b.playerId; }),
                m_pending.end());
            m_pending.push_back(pt);

            std::cout << "[relay] CtrlBindToken received player=" << b.playerId
                      << " expires=" << b.expiresUnix
                      << " expected_ip=" << b.ip
                      << " pending=" << m_pending.size() << "\n";
        }
        else if (n >= (int)sizeof(CtrlPlayerName) &&
                 (PktType)buf[0] == PktType::CtrlPlayerName) {
            CtrlPlayerName p; std::memcpy(&p, buf, sizeof(p));
            p.name[MAX_NAME_LEN - 1] = '\0';
            auto& slot = m_names[p.playerId];
            std::memcpy(slot.data(), p.name, MAX_NAME_LEN);
            for (auto& kv : m_sessions)
                if (kv.second.established) sendName(kv.first, p.playerId, p.name);
        }
        else if (n >= (int)sizeof(CtrlChanOp) &&
                 (PktType)buf[0] == PktType::CtrlPlayerGone) {
            CtrlChanOp o; std::memcpy(&o, buf, sizeof(o));
            m_names.erase(o.playerId);
        }
        m_engine->onControl(buf, (size_t)n);
    }
}

void Relay::audioLoop()
{
    uint8_t buf[AUDIO_MTU + 64];
    sockaddr_in from{};
    while (m_run) {
        int n = m_audio.recvFrom(buf, sizeof(buf), from);
        if (n <= 0) continue;

        std::lock_guard<std::mutex> lk(m_mtx);
        uint64_t key = packAddr(from);
        auto it = m_addrIndex.find(key);

        if (it != m_addrIndex.end()) {
            auto sit = m_sessions.find(it->second);
            if (sit != m_sessions.end() && sit->second.established) {
                handleEncrypted(sit->second, buf, n);
                continue;
            }
        }
        handleHandshake(buf, n, from);
    }
}

void Relay::handleHandshake(const uint8_t* buf, int len, const sockaddr_in& from)
{
    if (len < (int)sizeof(HandshakeReq)) return;
    if ((PktType)buf[0] != PktType::Handshake) return;

    std::cout << "[relay] handshake received from="
              << inet_ntoa(from.sin_addr) << ":" << ntohs(from.sin_port)
              << " len=" << len
              << " pending=" << m_pending.size() << "\n";

    HandshakeReq req; std::memcpy(&req, buf, sizeof(req));
    if (req.verMajor != PROTOCOL_VERSION_MAJOR) {
        std::cout << "[relay] handshake rejected: version "
                  << (unsigned)req.verMajor << "." << (unsigned)req.verMinor
                  << " expected major=" << (unsigned)PROTOCOL_VERSION_MAJOR << "\n";
        HandshakeNak nak{ (uint8_t)PktType::HandshakeNak, 2 };
        m_audio.sendTo(&nak, sizeof(nak), from); return;
    }

    if ((int)m_sessions.size() >= m_cfg.maxPlayers) {
        std::cout << "[relay] handshake rejected: server full sessions="
                  << m_sessions.size() << " max=" << m_cfg.maxPlayers << "\n";
        HandshakeNak nak{ (uint8_t)PktType::HandshakeNak, 3 };
        m_audio.sendTo(&nak, sizeof(nak), from); return;
    }

    time_t now = time(nullptr);
    int found = -1;
    for (size_t i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].expiresUnix < (uint32_t)now) {
            std::cout << "[relay] pending token expired player="
                      << m_pending[i].playerId
                      << " expires=" << m_pending[i].expiresUnix
                      << " now=" << (uint32_t)now << "\n";
            continue;
        }
        if (crypto::equals(m_pending[i].token.data(), req.token, TOKEN_BYTES)) {
            found = (int)i; break;
        }
    }

    if (found < 0) {
        std::cout << "[relay] handshake rejected: TOKEN NOT FOUND/EXPIRED"
                  << " pending=" << m_pending.size()
                  << " now=" << (uint32_t)now << "\n";
        HandshakeNak nak{ (uint8_t)PktType::HandshakeNak, 0 };
        m_audio.sendTo(&nak, sizeof(nak), from); return;
    }

    std::cout << "[relay] token match player=" << m_pending[found].playerId
              << " pending_index=" << found << "\n";

    bool fromLoopback = (ntohl(from.sin_addr.s_addr) >> 24) == 127;
    if (m_cfg.ipSecondaryCheck && m_pending[found].expectedIp != 0 && !fromLoopback &&
        from.sin_addr.s_addr != m_pending[found].expectedIp) {
        std::cout << "[relay] handshake IP mismatch for player "
                  << m_pending[found].playerId << " — rejected\n";
        HandshakeNak nak{ (uint8_t)PktType::HandshakeNak, 0 };
        m_audio.sendTo(&nak, sizeof(nak), from); return;
    }

    uint16_t pid = m_pending[found].playerId;
    m_pending.erase(m_pending.begin() + found);

    Session s;
    s.playerId = pid; s.addr = from; s.lastSeen = now;
    s.self = crypto::generateKeyPair();
    crypto::PubKey clientPk; std::memcpy(clientPk.data(), req.clientPubKey, 32);
    if (!crypto::deriveSession(s.self, clientPk, /*isClient=*/false, s.keys)) {
        std::cout << "[relay] handshake rejected: deriveSession FAILED player=" << pid << "\n";
        HandshakeNak nak{ (uint8_t)PktType::HandshakeNak, 0 };
        m_audio.sendTo(&nak, sizeof(nak), from); return;
    }
    s.established = true;

    m_addrIndex[packAddr(from)] = pid;
    m_sessions[pid] = s;

    HandshakeAck ack{};
    ack.type = (uint8_t)PktType::HandshakeAck;
    ack.playerId = pid;
    std::memcpy(ack.relayPubKey, s.self.pk.data(), 32);
    ack.vadAllowed           = m_engine->policy().vadAllowed;
    ack.defaultMode          = m_engine->policy().defaultMode;
    ack.maxRange             = m_engine->tuning().shout;
    ack.opusBitrate          = (uint16_t)m_engine->bus().behavior().bitrate;
    ack.maxConcurrentStreams = m_engine->policy().maxStreams;
    m_audio.sendTo(&ack, sizeof(ack), from);

    std::cout << "[relay] player " << pid << " authenticated (token ok)\n";

    for (auto& kv : m_names)
        sendName(pid, kv.first, kv.second.data());
}

void Relay::handleEncrypted(Session& s, const uint8_t* buf, int len)
{
    uint8_t plain[AUDIO_MTU + 64];
    size_t pn = crypto::open(s.keys, s.rxLastSeen, buf, (size_t)len,
                             plain, sizeof(plain));
    if (pn == 0) return;
    s.lastSeen = time(nullptr);
    if (pn < 1) return;

    switch ((PktType)plain[0]) {
    case PktType::AudioUp: {
        if (pn < sizeof(AudioUp)) return;
        AudioUp a; std::memcpy(&a, plain, sizeof(a));
        if (sizeof(AudioUp) + a.length > pn) return;
        m_engine->onAudioFrame(s.playerId, a.seq, a.flags,
                               plain + sizeof(AudioUp), a.length);
        break;
    }
    case PktType::Ping: {
        if (pn < sizeof(PingPong)) return;
        PingPong p; std::memcpy(&p, plain, sizeof(p));
        p.type = (uint8_t)PktType::Pong;
        emitToPlayer(s.playerId, (uint8_t*)&p, sizeof(p));
        break;
    }
    case PktType::Bye:
        m_addrIndex.erase(packAddr(s.addr));
        m_sessions.erase(s.playerId);
        break;
    default: break;
    }
}

void Relay::emitStatus(StatusType ev, uint16_t pid, uint16_t cid, uint8_t flag)
{
    if (!m_haveBridge) return;
    StatusEvent e{ (uint8_t)PktType::Status, (uint8_t)ev, pid, cid, flag };
    m_control.sendTo(&e, sizeof(e), m_bridgeAddr);
}

void Relay::emitToPlayer(uint16_t pid, const uint8_t* data, size_t len)
{
    auto it = m_sessions.find(pid);
    if (it == m_sessions.end() || !it->second.established) return;
    Session& s = it->second;

    uint8_t wire[AUDIO_MTU + 128];
    size_t wn = crypto::seal(s.keys, s.txCounter++, data, len, wire, sizeof(wire));
    if (wn) m_audio.sendTo(wire, (int)wn, s.addr);
}

void Relay::sendName(uint16_t to, uint16_t whoId, const char* name)
{
    PlayerNameDown d{};
    d.type = (uint8_t)PktType::PlayerName;
    d.playerId = whoId;
    std::memcpy(d.name, name, MAX_NAME_LEN);
    emitToPlayer(to, (const uint8_t*)&d, sizeof(d));
}

void Relay::cleanupLoop()
{
    using namespace std::chrono;
    while (m_run) {
        std::this_thread::sleep_for(milliseconds(200));
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_engine->tick();
            time_t now = time(nullptr);

            m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                [&](const PendingToken& p){ return p.expiresUnix < (uint32_t)now; }),
                m_pending.end());

            for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
                if (now - it->second.lastSeen > 15) {
                    uint16_t pid = it->first;
                    std::cout << "[relay] timeout player " << pid << "\n";
                    m_addrIndex.erase(packAddr(it->second.addr));
                    it = m_sessions.erase(it);
                    emitStatus(StatusType::SessionTimeout, pid);
                } else ++it;
            }
        }
    }
}

} // namespace vc
