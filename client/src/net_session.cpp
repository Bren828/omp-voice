// ============================================================
//  VoiceChat client — secure network session implementation
//  File: client/src/net_session.cpp
// ============================================================
#include "../include/net_session.h"
#include <chrono>
#include <cstdio>

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
    std::printf("[token] start: opening UDP socket port=%u\n",
                (unsigned)cmdPort);

    if (!m_sock.open()) {
        std::printf("[token] !!! socket OPEN FAILED !!!\n");
        return false;
    }

    if (!m_sock.bindAny(cmdPort)) {
        std::printf("[token] !!! bind FAILED port=%u !!!\n",
                    (unsigned)cmdPort);
        return false;
    }

    m_sock.setRecvTimeout(200);

    std::printf("[token] socket bound 0.0.0.0:%u\n",
                (unsigned)cmdPort);

    m_run = true;

    m_thr = std::thread([this] {
        uint8_t buf[64];
        sockaddr_in from{};

        std::printf("[token] receiver thread started\n");

        while (m_run) {
            int n = m_sock.recvFrom(buf, sizeof(buf), from);

            if (n <= 0)
                continue;

            char fromIp[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &from.sin_addr,
                      fromIp, sizeof(fromIp));

            std::printf(
                "[token] RX from=%s:%u len=%d first=%02X %02X\n",
                fromIp,
                (unsigned)ntohs(from.sin_port),
                n,
                n > 0 ? buf[0] : 0,
                n > 1 ? buf[1] : 0
            );

            if (n < (int)(4 + TOKEN_BYTES)) {
                std::printf(
                    "[token] DROP: packet too small (%d, expected >= %d)\n",
                    n,
                    (int)(4 + TOKEN_BYTES)
                );
                continue;
            }

            if (buf[0] != 'T' || buf[1] != 'K') {
                std::printf(
                    "[token] DROP: invalid magic %02X %02X, expected 54 4B\n",
                    buf[0],
                    buf[1]
                );
                continue;
            }

            uint16_t pid = 0;
            std::memcpy(&pid, buf + 2, sizeof(pid));

            {
                std::lock_guard<std::mutex> lk(m_mtx);

                std::memcpy(&m_pid, buf + 2, 2);
                std::memcpy(m_tok.data(), buf + 4, TOKEN_BYTES);

                m_have = true;
            }

            std::printf(
                "[token] !!! TOKEN ACCEPTED player=%u len=%d !!!\n",
                (unsigned)pid,
                n
            );
        }

        std::printf("[token] receiver thread stopped\n");
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
    if (!m_sock.valid()) {
        std::printf("[net] socket open begin\n");
        if (!m_sock.open()) {
            std::printf("[net] !!! socket open FAILED !!!\n");
            return false;
        }
        std::printf("[net] socket open OK\n");
    }

    m_sock.setRecvTimeout(200);
    m_server = UdpSocket::addr(relayIp, audioPort);

    char relayAddr[INET_ADDRSTRLEN]{};
    if (m_server.sin_family == AF_INET) {
        inet_ntop(AF_INET, &m_server.sin_addr, relayAddr, sizeof(relayAddr));
    }
    std::printf("[net] handshake target=%s:%u\n", relayAddr[0] ? relayAddr : "?",
                (unsigned)ntohs(m_server.sin_port));

    m_self = crypto::generateKeyPair();
    m_tx = 1;

    HandshakeReq req{};
    req.type = (uint8_t)PktType::Handshake;
    req.verMajor = PROTOCOL_VERSION_MAJOR;
    req.verMinor = PROTOCOL_VERSION_MINOR;
    std::memcpy(req.token, token.data(), TOKEN_BYTES);
    std::memcpy(req.clientPubKey, m_self.pk.data(), 32);

    std::printf("[net] handshake packet type=%u ver=%u.%u size=%zu\n",
                (unsigned)req.type, (unsigned)req.verMajor, (unsigned)req.verMinor,
                sizeof(req));

    m_run = true;
    m_established = false;
    m_nak = false;
    if (!m_recv.joinable()) {
        std::printf("[net] recv thread start\n");
        m_recv = std::thread([this] { recvLoop(); });
    }

    int sent = m_sock.sendTo(&req, sizeof(req), m_server);
    std::printf("[net] handshake sendTo result=%d expected=%zu\n", sent, sizeof(req));
    if (sent != (int)sizeof(req)) {
        std::printf("[net] !!! handshake UDP SEND FAILED !!!\n");
        return false;
    }

    uint32_t start = nowMs();
    while (!m_established && !m_nak && nowMs() - start < 2000)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (m_established.load()) {
        std::printf("[net] handshake completed: ACK received player=%u\n",
                    (unsigned)m_limits.playerId);
        return true;
    }
    if (m_nak.load()) {
        std::printf("[net] handshake completed: NAK received\n");
        return false;
    }

    std::printf("[net] handshake timeout: no ACK/NAK within 2000 ms\n");
    return false;
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
            if (n >= (int)sizeof(HandshakeNak) &&
                (PktType)buf[0] == PktType::HandshakeNak) {
                HandshakeNak nak{};
                std::memcpy(&nak, buf, sizeof(nak));
                std::printf("[net] received HANDSHAKE NAK size=%d\n", n);
                m_nak = true;
                continue;
            }

            if (n >= (int)sizeof(HandshakeAck) &&
                (PktType)buf[0] == PktType::HandshakeAck) {
                HandshakeAck ack; std::memcpy(&ack, buf, sizeof(ack));
                char fromAddr[INET_ADDRSTRLEN]{};
                if (from.sin_family == AF_INET)
                    inet_ntop(AF_INET, &from.sin_addr, fromAddr, sizeof(fromAddr));
                std::printf("[net] received HANDSHAKE ACK size=%d from=%s:%u player=%u\n",
                            n, fromAddr[0] ? fromAddr : "?", (unsigned)ntohs(from.sin_port),
                            (unsigned)ack.playerId);
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
                    std::printf("[net] session keys derived OK\n");
                } else {
                    std::printf("[net] !!! session key derivation FAILED !!!\n");
                }
                continue;
            }

            std::printf("[net] received unexpected handshake packet size=%d type=%u\n",
                        n, (unsigned)buf[0]);
            continue;
        }

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
            nm.name[MAX_NAME_LEN - 1] = '\0';
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
    if (t - m_lastPing > 3000) {
        m_lastPing = t;
        PingPong p{ (uint8_t)PktType::Ping, m_limits.playerId, t };
        sendSealed((uint8_t*)&p, sizeof(p));
    }
    if (m_lastRx && t - m_lastRx > 8000)
        m_established = false;
}

} // namespace vc::client
