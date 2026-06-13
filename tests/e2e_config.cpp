// ============================================================
//  Stage-H verification: voice.ini config push -> handshake Ack
//  File: tests/e2e_config.cpp
//
//  Proves the stage-H "full load + push" wiring end to end:
//    * a real Relay (engine + transport) booted with default config,
//    * a real core::ControlLink playing the .dll bridge, which calls
//      pushConfig(...) — the exact path samp/omp use after reading voice.ini,
//    * a minimal client that handshakes and reads the HandshakeAck limits.
//
//  Asserts:
//    1. BEFORE any push, the Ack carries the relay's boot defaults (so the
//       limits come from config/engine, not hardcoded).
//    2. AFTER bridge.pushConfig(...) with distinct values, a fresh handshake's
//       Ack reflects the PUSHED limits (vadAllowed/defaultMode/opusBitrate/
//       maxConcurrentStreams/maxRange) — i.e. the engine applied CtrlConfig and
//       the Ack reads from the engine.
// ============================================================
#include "../relay/include/relay.h"
#include "../core/include/control_link.h"
#include "../shared/protocol.h"
#include "../shared/udp.h"
#include "../shared/crypto.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <unordered_map>

using namespace vc;

static int g_fail = 0;
static void check(const char* n, bool ok){ printf("  [%s] %s\n", ok?"PASS":"FAIL", n); if(!ok) ++g_fail; }
static void nap(int ms){ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Minimal client: handshake with a token, return the Ack limits it received.
static bool handshakeGetAck(uint16_t audioPort, const crypto::Token& tok, HandshakeAck& out)
{
    UdpSocket sock; sock.open(); sock.setRecvTimeout(1000);
    sockaddr_in srv = UdpSocket::addr("127.0.0.1", audioPort);
    auto kp = crypto::generateKeyPair();
    HandshakeReq r{};
    r.type = (uint8_t)PktType::Handshake;
    r.verMajor = PROTOCOL_VERSION_MAJOR; r.verMinor = PROTOCOL_VERSION_MINOR;
    std::memcpy(r.token, tok.data(), TOKEN_BYTES);
    std::memcpy(r.clientPubKey, kp.pk.data(), 32);
    sock.sendTo(&r, sizeof(r), srv);

    uint8_t buf[256]; sockaddr_in from{};
    int n = sock.recvFrom(buf, sizeof(buf), from);
    if (n < (int)sizeof(HandshakeAck) || (PktType)buf[0] != PktType::HandshakeAck)
        return false;
    std::memcpy(&out, buf, sizeof(out));
    return true;
}

int main()
{
    crypto::init();
    printf("Stage-H voice.ini config push -> Ack limits:\n");

    RelayConfig cfg;                       // boot defaults
    cfg.controlPort = 17786; cfg.audioPort = 17787;
    cfg.ipSecondaryCheck = false;          // identity under test is the token
    // Make the relay's boot defaults explicit (these are the RelayConfig
    // defaults, restated so the assertions below are unambiguous).
    cfg.vadAllowed = true; cfg.defaultMode = 0;
    cfg.bitrate = 24000; cfg.maxConcurrentStreams = 8; cfg.shout = 40.f;

    Relay relay;
    if (!relay.init(cfg)) { printf("relay init failed\n"); return 1; }
    std::thread th([&]{ relay.run(); });
    nap(200);

    // The .dll bridge: mints tokens, captures them per-player via the callback.
    std::unordered_map<uint16_t, crypto::Token> tokens;
    core::ControlLink bridge;
    if (!bridge.init("127.0.0.1", cfg.controlPort)) { printf("bridge init failed\n"); return 1; }
    bridge.setTokenDeliver([&](uint16_t pid, const crypto::Token& t){ tokens[pid] = t; });

    // ── 1) Baseline: handshake before any push -> boot defaults in the Ack. ──
    const uint16_t PID_A = 1;
    bridge.onPlayerConnect(PID_A, /*ip=*/0);
    nap(150);
    HandshakeAck a{};
    bool gotA = tokens.count(PID_A) && handshakeGetAck(cfg.audioPort, tokens[PID_A], a);
    check("baseline handshake ok", gotA);
    if (gotA) {
        check("baseline vadAllowed = 1",        a.vadAllowed == 1);
        check("baseline defaultMode = 0",       a.defaultMode == 0);
        check("baseline opusBitrate = 24000",   a.opusBitrate == 24000);
        check("baseline maxStreams = 8",        a.maxConcurrentStreams == 8);
        check("baseline maxRange = 40",         std::fabs(a.maxRange - 40.f) < 0.01f);
    }

    // ── 2) Push distinct config, then a fresh handshake must reflect it. ──
    bridge.pushConfig(/*opusBitrate=*/32000, /*whisper=*/5.f, /*normal=*/12.f,
                      /*shout=*/55.f, /*falloffExp=*/1.5f, /*occlusion=*/false,
                      /*vadAllowed=*/false, /*defaultMode=*/1, /*maxStreams=*/5);
    nap(200);   // let the relay apply CtrlConfig on its control thread

    const uint16_t PID_B = 2;
    bridge.onPlayerConnect(PID_B, /*ip=*/0);
    nap(150);
    HandshakeAck b{};
    bool gotB = tokens.count(PID_B) && handshakeGetAck(cfg.audioPort, tokens[PID_B], b);
    check("post-push handshake ok", gotB);
    if (gotB) {
        check("pushed vadAllowed = 0",          b.vadAllowed == 0);
        check("pushed defaultMode = 1",         b.defaultMode == 1);
        check("pushed opusBitrate = 32000",     b.opusBitrate == 32000);
        check("pushed maxStreams = 5",          b.maxConcurrentStreams == 5);
        check("pushed maxRange = 55",           std::fabs(b.maxRange - 55.f) < 0.01f);
    }

    bridge.shutdown();
    relay.stop();
    th.join();
    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 2 : 0;
}
