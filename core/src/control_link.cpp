// ============================================================
//  VoiceChat core — Control link implementation
//  File: core/src/control_link.cpp
// ============================================================
#include "../include/control_link.h"
#include "../../shared/protocol.h"
#include <ctime>
#include <cstring>

namespace vc::core {

bool ControlLink::init(const char* relayIp, uint16_t controlPort)
{
    if (!m_sock.open()) return false;
    m_relay = UdpSocket::addr(relayIp, controlPort);
    // Poll-with-timeout so the status thread can observe the shutdown flag.
    m_sock.setRecvTimeout(200);
    m_statusRun = true;
    m_statusThr = std::thread([this] { statusLoop(); });
    return true;
}

void ControlLink::shutdown()
{
    m_statusRun = false;
    if (m_statusThr.joinable()) m_statusThr.join();
    m_sock.close();
}

// The relay replies to whatever source address it last saw on the control
// socket — i.e. THIS socket — so StatusEvents arrive here. We never bind a
// fixed port: the bridge always speaks first (CtrlBindToken at connect), so
// the relay has learned our address before any status can be sent.
void ControlLink::statusLoop()
{
    uint8_t buf[256];
    sockaddr_in from{};
    while (m_statusRun) {
        int n = m_sock.recvFrom(buf, sizeof(buf), from);
        if (n < (int)sizeof(StatusEvent)) continue;
        if ((PktType)buf[0] != PktType::Status) continue;
        StatusEvent e; std::memcpy(&e, buf, sizeof(e));
        std::lock_guard<std::mutex> lk(m_statusMtx);
        if (m_statusQ.size() < 4096) m_statusQ.push_back(e);   // bound the queue
    }
}

bool ControlLink::nextStatus(StatusEvent& out)
{
    std::lock_guard<std::mutex> lk(m_statusMtx);
    if (m_statusQ.empty()) return false;
    out = m_statusQ.front();
    m_statusQ.pop_front();
    return true;
}

template<typename T>
void ControlLink::send(const T& pkt)
{
    if (m_sock.valid()) m_sock.sendTo(&pkt, (int)sizeof(pkt), m_relay);
}

void ControlLink::onPlayerConnect(uint16_t playerId, uint32_t ip, uint32_t ttlSeconds)
{
    crypto::Token tok;
    crypto::mintToken(tok);                       // req. A1 fresh per connect

    CtrlBindToken b{};
    b.type = (uint8_t)PktType::CtrlBindToken;
    b.playerId = playerId;
    std::memcpy(b.token, tok.data(), TOKEN_BYTES);
    b.expiresUnix = (uint32_t)time(nullptr) + ttlSeconds;
    b.ip = ip;                                    // req. A: IP secondary check
    send(b);

    if (m_deliver) m_deliver(playerId, tok);      // ship to client (game chan)

    m_reg.player(playerId);                       // ensure bookkeeping entry
}

void ControlLink::pushPosition(uint16_t playerId, float x, float y, float z,
                               uint16_t interior, uint16_t vworld, uint8_t veh)
{
    CtrlPlayerPos p{};
    p.type = (uint8_t)PktType::CtrlPlayerPos;
    p.playerId = playerId; p.x = x; p.y = y; p.z = z;
    p.interior = interior; p.virtualWorld = vworld; p.vehicleState = veh;
    send(p);
}

void ControlLink::pushMeta(uint16_t playerId, uint8_t flags,
                           uint8_t rangePreset, float rangeOverride)
{
    CtrlPlayerMeta m{};
    m.type = (uint8_t)PktType::CtrlPlayerMeta;
    m.playerId = playerId; m.flags = flags;
    m.rangePreset = rangePreset; m.rangeOverride = rangeOverride;
    send(m);
}

void ControlLink::setTransmit(uint16_t playerId, bool on, uint32_t mask)
{
    CtrlTransmit t{};
    t.type = (uint8_t)PktType::CtrlTransmit;
    t.playerId = playerId; t.on = on ? 1 : 0; t.channelMask = mask;
    send(t);
}

void ControlLink::setMixPolicy(uint16_t playerId, MixPolicy pol,
                               float duck, float proxFloor)
{
    CtrlMixPolicy m{};
    m.type = (uint8_t)PktType::CtrlMixPolicy;
    m.playerId = playerId; m.policy = (uint8_t)pol;
    m.duckLevel = duck; m.proximityFloor = proxFloor;
    send(m);
}

void ControlLink::mutePlayer(uint16_t listenerId, uint16_t targetId, bool on)
{
    CtrlPairMute m{};
    m.type = (uint8_t)PktType::CtrlPairMute;
    m.listenerId = listenerId; m.targetId = targetId; m.on = on ? 1 : 0;
    send(m);
}

void ControlLink::setSpeakerphone(uint16_t playerId, bool on)
{
    CtrlSpeakerphone m{};
    m.type = (uint8_t)PktType::CtrlSpeakerphone;
    m.playerId = playerId; m.on = on ? 1 : 0;
    send(m);
}

void ControlLink::pushConfig(uint16_t opusBitrate, float whisper, float normal,
                             float shout, float falloffExp, bool occlusion,
                             bool vadAllowed, uint8_t defaultMode, uint8_t maxStreams)
{
    CtrlConfig c{};
    c.type = (uint8_t)PktType::CtrlConfig;
    c.opusBitrate = opusBitrate;
    c.proxWhisper = whisper; c.proxNormal = normal; c.proxShout = shout;
    c.falloffExp = falloffExp;
    c.occlusionEnabled = occlusion ? 1 : 0;
    c.vadAllowed = vadAllowed ? 1 : 0;
    c.defaultMode = defaultMode;
    c.maxConcurrentStreams = maxStreams;
    send(c);
}

void ControlLink::setPlayerName(uint16_t playerId, const char* name)
{
    CtrlPlayerName p{};
    p.type = (uint8_t)PktType::CtrlPlayerName;
    p.playerId = playerId;
    if (name) {                                   // copy, always null-terminated
        std::strncpy(p.name, name, MAX_NAME_LEN - 1);
        p.name[MAX_NAME_LEN - 1] = '\0';
    }
    send(p);
}

uint16_t ControlLink::createChannel(Filter f, bool positional, uint8_t prio)
{
    uint16_t id = m_chanAlloc.alloc();
    m_reg.createChannel(id, f, positional, prio);

    CtrlChanCreate c{};
    c.type = (uint8_t)PktType::CtrlChanCreate;
    c.channelId = id; c.filter = (uint8_t)f;
    c.positional = positional ? 1 : 0; c.priority = prio;
    send(c);
    return id;
}

static CtrlChanOp chanOp(PktType t, uint16_t cid, uint16_t pid, uint8_t arg)
{
    CtrlChanOp o{};
    o.type = (uint8_t)t; o.channelId = cid; o.playerId = pid; o.arg = arg;
    return o;
}

void ControlLink::destroyChannel(uint16_t cid)
{
    m_reg.destroyChannel(cid);
    send(chanOp(PktType::CtrlChanDestroy, cid, INVALID_PLAYER, 0));
}

void ControlLink::joinChannel(uint16_t pid, uint16_t cid)
{
    m_reg.joinChannel(pid, cid);
    send(chanOp(PktType::CtrlChanJoin, cid, pid, 0));
}

void ControlLink::leaveChannel(uint16_t pid, uint16_t cid)
{
    m_reg.leaveChannel(pid, cid);
    send(chanOp(PktType::CtrlChanLeave, cid, pid, 0));
}

void ControlLink::setChannelFilter(uint16_t cid, Filter f)
{
    if (Channel* c = m_reg.channel(cid)) c->filter = f;
    send(chanOp(PktType::CtrlChanFilter, cid, INVALID_PLAYER, (uint8_t)f));
}

void ControlLink::setChannelPriority(uint16_t cid, uint8_t prio)
{
    if (Channel* c = m_reg.channel(cid)) c->priority = prio;
    send(chanOp(PktType::CtrlChanPrio, cid, INVALID_PLAYER, prio));
}

void ControlLink::onPlayerDisconnect(uint16_t playerId)
{
    m_reg.removeFromAllChannels(playerId);        // req. I1
    m_reg.removePlayer(playerId);
    send(chanOp(PktType::CtrlPlayerGone, INVALID_CHANNEL, playerId, 0));
}

} // namespace vc::core
