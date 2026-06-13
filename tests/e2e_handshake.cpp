// ============================================================
//  Stage-A verification: end-to-end token handshake over real UDP
//  File: tests/e2e_handshake.cpp
//
//  Spins a real Relay, then plays the .dll + .asi roles against it:
//   1. valid token + matching IP                          -> HandshakeAck
//   2. wrong token                                        -> HandshakeNak (reason 0)
//   3. valid token, bound IP differs, source is loopback  -> HandshakeAck
//      (documented "server host plays locally" exception; the true IP-reject
//       path needs a non-loopback source and isn't reachable from this harness)
//  Exercises CtrlBindToken, token table, X25519 handshake and the IP check.
// ============================================================
#include "../relay/include/relay.h"
#include "../shared/protocol.h"
#include "../shared/udp.h"
#include "../shared/crypto.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace vc;

static int g_fail = 0;
static void check(const char* n, bool ok){ printf("  [%s] %s\n", ok?"PASS":"FAIL", n); if(!ok) ++g_fail; }
static void nap(int ms){ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Send a CtrlBindToken to the relay control port (plays the .dll).
static void bindToken(UdpSocket& s, const sockaddr_in& ctrl,
                      uint16_t pid, const crypto::Token& tok, uint32_t ip)
{
    CtrlBindToken b{};
    b.type = (uint8_t)PktType::CtrlBindToken; b.playerId = pid;
    std::memcpy(b.token, tok.data(), TOKEN_BYTES);
    b.expiresUnix = (uint32_t)time(nullptr) + 30; b.ip = ip;
    s.sendTo(&b, sizeof(b), ctrl);
}

// Returns: 1 = Ack, 0 = Nak, -1 = nothing.
static int doHandshake(uint16_t audioPort, const crypto::Token& tok, uint16_t* outPid)
{
    UdpSocket c; c.open(); c.setRecvTimeout(800);
    auto srv = UdpSocket::addr("127.0.0.1", audioPort);
    auto kp  = crypto::generateKeyPair();

    HandshakeReq r{};
    r.type = (uint8_t)PktType::Handshake;
    r.verMajor = PROTOCOL_VERSION_MAJOR; r.verMinor = PROTOCOL_VERSION_MINOR;
    std::memcpy(r.token, tok.data(), TOKEN_BYTES);
    std::memcpy(r.clientPubKey, kp.pk.data(), 32);
    c.sendTo(&r, sizeof(r), srv);

    uint8_t buf[256]; sockaddr_in from{};
    int n = c.recvFrom(buf, sizeof(buf), from);
    if (n <= 0) return -1;
    if ((PktType)buf[0] == PktType::HandshakeAck && n >= (int)sizeof(HandshakeAck)) {
        HandshakeAck a; std::memcpy(&a, buf, sizeof(a)); if (outPid) *outPid = a.playerId;
        return 1;
    }
    if ((PktType)buf[0] == PktType::HandshakeNak) return 0;
    return -1;
}

int main()
{
    crypto::init();
    printf("Stage-A e2e handshake:\n");

    RelayConfig cfg;
    cfg.controlPort = 17778; cfg.audioPort = 17779;
    cfg.ipSecondaryCheck = true;

    Relay relay;
    if (!relay.init(cfg)) { printf("relay init failed\n"); return 1; }
    std::thread th([&]{ relay.run(); });
    nap(200);

    UdpSocket ctl; ctl.open();
    auto ctrlAddr = UdpSocket::addr("127.0.0.1", cfg.controlPort);
    uint32_t loop = UdpSocket::addr("127.0.0.1", 0).sin_addr.s_addr; // 127.0.0.1

    // 1) valid token + matching IP (127.0.0.1, the test's own source)
    crypto::Token good; crypto::mintToken(good);
    bindToken(ctl, ctrlAddr, 7, good, loop);
    nap(150);
    uint16_t pid = 0xFFFF;
    int r1 = doHandshake(cfg.audioPort, good, &pid);
    check("valid token -> Ack", r1 == 1 && pid == 7);

    // 2) wrong token -> Nak
    crypto::Token bad; crypto::mintToken(bad);
    int r2 = doHandshake(cfg.audioPort, bad, nullptr);
    check("wrong token -> Nak", r2 == 0);

    // 3) valid token, but the IP the .dll registered (203.0.113.9) differs from
    //    the 127.0.0.1 this handshake actually arrives from. The relay TRUSTS a
    //    loopback source: that's the server host playing locally, whose game-IP
    //    (a LAN address via GetPlayerIp) legitimately differs from the 127.0.0.1
    //    its .asi dials the relay over. So this is Ack'd, not Nak'd.
    //
    //    SECURITY NOTE: this is NOT a weakening of the IP secondary check. That
    //    check still rejects a sniffed-token replay from a *non-loopback* host
    //    (relay.cpp: the `!fromLoopback && from != expectedIp` reject). Driving
    //    that reject path needs a non-loopback source, which a pure-loopback
    //    unit-test harness cannot forge, so it isn't exercised here.
    crypto::Token good2; crypto::mintToken(good2);
    uint32_t wrongIp = UdpSocket::addr("203.0.113.9", 0).sin_addr.s_addr;
    bindToken(ctl, ctrlAddr, 8, good2, wrongIp);
    nap(150);
    uint16_t pid3 = 0xFFFF;
    int r3 = doHandshake(cfg.audioPort, good2, &pid3);
    check("loopback source trusted despite bound-IP mismatch -> Ack", r3 == 1 && pid3 == 8);

    relay.stop();
    th.join();
    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 2 : 0;
}
