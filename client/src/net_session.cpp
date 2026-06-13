// ============================================================
//  VoiceChat client — secure network session implementation
//  File: client/src/net_session.cpp
// ============================================================
#include "../include/net_session.h"
#include <chrono>

namespace vc::client {

static uint32_t nowMs()
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// ── TokenInbox ──────────────────────────────────────────────
bool TokenInbox::start(uint16_t cmdPort)
{
    if (!m_sock.open() || !m_sock.bindAny(cmdPort)) return false;
    m_sock.setRecvTimeout(200);
    m_run = true;
    m_thr = std::thread([this] {
        uint8_t buf[64]; sockaddr_in from{};
        while (m_run) {
            int n = m_sock.recvFrom(buf, sizeof(buf), from);
            if (n < (int)(4 + TOKEN_BYTES)) continue;
            if (buf[0] != 'T' || buf[1] != 'K') continue;
            std::lock_guard<std::mutex> lk(m_mtx);
            std::memcpy(&m_pid, buf + 2, 2);
            std::memcpy(m_tok.data(), buf + 4, TOKEN_BYTES);
            m_have = true;
        }
    });
    return true;
}

void TokenInbox::stop()
{
    m_run = false;
    if (m_thr.joinable()) m_thr.join();
    m_sock.close();
}

bool TokenInbox::latest(crypto::Token& out, uint16_t& pid)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_have) return false;
    out = m_tok; pid = m_pid;
    return true;
}

// ── Session ─────────────────────────────────────────────────
bool Session::connect(const char* relayIp, uint16_t audioPort,
                      const crypto::Token& token)
{
    if (!m_sock.valid()) { if (!m_sock.open()) return false; }
    m_sock.setRecvTimeout(200);
    m_server = UdpSocket::addr(relayIp, audioPort);

    m_self = crypto::generateKeyPair();
    // Fresh ephemeral key => fresh AEAD session; restart the nonce counter so
    // each session's (key,counter) space starts clean. Safe because the key is
    // never reused across handshakes.
    m_tx = 1;

    HandshakeReq req{};
    req.type = (uint8_t)PktType::Handshake;
    req.verMajor = PROTOCOL_VERSION_MAJOR;
    req.verMinor = PROTOCOL_VERSION_MINOR;
    std::memcpy(req.token, token.data(), TOKEN_BYTES);
    std::memcpy(req.clientPubKey, m_self.pk.data(), 32);

    m_run = true;
    m_established = false;
    m_nak = false;
    // Reuse the recv thread/socket across reconnects: it already watches for
    // the handshake reply when !established, so we just (re)send the request.
    if (!m_recv.joinable())
        m_recv = std::thread([this] { recvLoop(); });
    m_sock.sendTo(&req, sizeof(req), m_server);   // CLEARTEXT (token is secret)

    // Wait briefly for the ack — or bail early if the relay NAKs (e.g. a stale
    // token during reconnect), so the manager can retry with a fresh one.
    uint32_t start = nowMs();
    while (!m_established && !m_nak && nowMs() - start < 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return m_established.load();
}

void Session::disconnect()
{
    if (m_established) {
        PingPong bye{ (uint8_t)PktType::Bye, m_limits.playerId, nowMs() };
        sendSealed((uint8_t*)&bye, sizeof(bye));
    }
    m_run = false;
    if (m_recv.joinable()) m_recv.join();
    m_sock.close();
    m_established = false;
}

void Session::sendSealed(const uint8_t* plain, size_t len)
{
    if (!m_established) return;
    uint8_t wire[AUDIO_MTU + 128];
    size_t wn = crypto::seal(m_keys, m_tx++, plain, len, wire, sizeof(wire));
    if (wn) m_sock.sendTo(wire, (int)wn, m_server);
}

void Session::sendAudio(uint16_t seq, uint8_t flags, const uint8_t* opus, uint16_t len)
{
    if (!m_established || len > MAX_OPUS_BYTES) return;
    uint8_t plain[sizeof(AudioUp) + MAX_OPUS_BYTES];
    auto* a = reinterpret_cast<AudioUp*>(plain);
    a->type = (uint8_t)PktType::AudioUp; a->seq = seq; a->flags = flags; a->length = len;
    std::memcpy(plain + sizeof(AudioUp), opus, len);
    sendSealed(plain, sizeof(AudioUp) + len);
}

void Session::recvLoop()
{
    uint8_t buf[AUDIO_MTU + 128];
    sockaddr_in from{};
    while (m_run) {
        int n = m_sock.recvFrom(buf, sizeof(buf), from);
        if (n <= 0) continue;
        m_lastRx = nowMs();

        if (!m_established) {
            // A reject (bad/expired/stale token, version, full) -> let connect()
            // return fast so the manager retries with a fresh token.
            if (n >= (int)sizeof(HandshakeNak) &&
                (PktType)buf[0] == PktType::HandshakeNak) {
                m_nak = true;
                continue;
            }
            // Expect a cleartext handshake reply.
            if (n >= (int)sizeof(HandshakeAck) &&
                (PktType)buf[0] == PktType::HandshakeAck) {
                HandshakeAck ack; std::memcpy(&ack, buf, sizeof(ack));
                crypto::PubKey relayPk; std::memcpy(relayPk.data(), ack.relayPubKey, 32);
                if (crypto::deriveSession(m_self, relayPk, /*isClient=*/true, m_keys)) {
                    m_limits.playerId    = ack.playerId;
                    m_limits.vadAllowed  = ack.vadAllowed != 0;
                    m_limits.defaultMode = ack.defaultMode;
                    m_limits.maxRange    = ack.maxRange;
                    m_limits.opusBitrate = ack.opusBitrate;
                    m_limits.maxStreams  = ack.maxConcurrentStreams;
                    m_rxLastSeen = 0;
                    m_established = true;
                }
            }
            continue;
        }

        // Established: AEAD-open then dispatch.
        uint8_t plain[AUDIO_MTU + 64];
        size_t pn = crypto::open(m_keys, m_rxLastSeen, buf, (size_t)n, plain, sizeof(plain));
        if (pn < 1) continue;

        switch ((PktType)plain[0]) {
        case PktType::ProximityDown: {
            if (pn < sizeof(ProximityDown)) break;
            ProximityDown p; std::memcpy(&p, plain, sizeof(p));
            if (sizeof(ProximityDown) + p.length > pn) break;
            if (m_hooks.onProximity)
                m_hooks.onProximity(p.speakerId, p.seq, p.flags, p.volume, p.pan,
                                    plain + sizeof(ProximityDown), p.length);
            break;
        }
        case PktType::BusDown: {
            if (pn < sizeof(BusDown)) break;
            BusDown b; std::memcpy(&b, plain, sizeof(b));
            if (sizeof(BusDown) + b.length > pn) break;
            if (m_hooks.onBus)
                m_hooks.onBus(b.channelId, b.filter, b.seq, b.volume,
                              plain + sizeof(BusDown), b.length);
            break;
        }
        case PktType::PlayerGone: {
            if (pn < sizeof(PlayerGone)) break;
            PlayerGone g; std::memcpy(&g, plain, sizeof(g));
            if (m_hooks.onGone) m_hooks.onGone(g.speakerId);
            break;
        }
        case PktType::PlayerName: {
            if (pn < sizeof(PlayerNameDown)) break;
            PlayerNameDown nm; std::memcpy(&nm, plain, sizeof(nm));
            nm.name[MAX_NAME_LEN - 1] = '\0';            // guard truncated names
            if (m_hooks.onName) m_hooks.onName(nm.playerId, nm.name);
            break;
        }
        case PktType::Pong: default: break;
        }
    }
}

void Session::tick()
{
    if (!m_established) return;
    uint32_t t = nowMs();
    // Heartbeat (req. B3).
    if (t - m_lastPing > 3000) {
        m_lastPing = t;
        PingPong p{ (uint8_t)PktType::Ping, m_limits.playerId, t };
        sendSealed((uint8_t*)&p, sizeof(p));
    }
    // Rx timeout -> drop session so the caller re-requests a token and
    // reconnects (req. B2 auto-reconnect with fresh token).
    if (m_lastRx && t - m_lastRx > 8000)
        m_established = false;
}

} // namespace vc::client
