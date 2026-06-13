// ============================================================
//  Stage-B verification: relay -> bridge status channel
//  File: tests/e2e_status.cpp
//
//  Drives the REAL pieces that make OnPlayerStartTalking / OnPlayerStopTalking
//  / OnPlayerRadioKey fire:
//    * a real Relay (transport + engine + status emit),
//    * a real core::ControlLink playing the .dll bridge (mint token, push
//      position/channels, AND receive StatusEvents on its loopback socket),
//    * a hand-rolled sealed-audio client (handshake + AEAD AudioUp).
//
//  Asserts the bridge observes, via ControlLink::nextStatus():
//    1. SpeakerStart when the client sends its first voice frame,
//    2. RadioKey(down) when the player keys a radio channel,
//    3. SpeakerStop after the speaker goes silent (engine silence timeout).
//
//  This is the path SA-MP ProcessTick / open.mp pumpStatus translate into
//  Pawn callbacks — so if this passes, the callbacks fire.
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

using namespace vc;

static int g_fail = 0;
static void check(const char* n, bool ok){ printf("  [%s] %s\n", ok?"PASS":"FAIL", n); if(!ok) ++g_fail; }
static void nap(int ms){ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Minimal client: handshake with a token, then seal AudioUp frames.
struct TestClient {
    UdpSocket sock;
    sockaddr_in srv{};
    crypto::SessionKeys keys{};
    uint64_t tx = 1, rxSeen = 0;
    uint16_t pid = INVALID_PLAYER;

    bool handshake(uint16_t audioPort, const crypto::Token& tok) {
        sock.open(); sock.setRecvTimeout(1000);
        srv = UdpSocket::addr("127.0.0.1", audioPort);
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
        HandshakeAck a; std::memcpy(&a, buf, sizeof(a));
        pid = a.playerId;
        crypto::PubKey rpk; std::memcpy(rpk.data(), a.relayPubKey, 32);
        return crypto::deriveSession(kp, rpk, /*isClient=*/true, keys);
    }

    void sendVoice() {
        uint8_t plain[sizeof(AudioUp) + 8];
        auto* a = reinterpret_cast<AudioUp*>(plain);
        a->type = (uint8_t)PktType::AudioUp; a->seq = (uint16_t)tx;
        a->flags = AF_PTT; a->length = 8;
        std::memset(plain + sizeof(AudioUp), 0x5A, 8);   // fake Opus bytes
        uint8_t wire[256];
        size_t wn = crypto::seal(keys, tx++, plain, sizeof(plain), wire, sizeof(wire));
        if (wn) sock.sendTo(wire, (int)wn, srv);
    }
};

// Drain the bridge status queue until `want` for `pid` appears or we time out.
static bool waitEvent(core::ControlLink& br, StatusType want, uint16_t pid,
                      int timeoutMs, StatusEvent* outEv = nullptr) {
    using namespace std::chrono;
    auto end = steady_clock::now() + milliseconds(timeoutMs);
    StatusEvent ev;
    while (steady_clock::now() < end) {
        while (br.nextStatus(ev)) {
            if ((StatusType)ev.event == want && ev.playerId == pid) {
                if (outEv) *outEv = ev;
                return true;
            }
        }
        nap(20);
    }
    return false;
}

int main()
{
    crypto::init();
    printf("Stage-B relay->bridge status channel:\n");

    RelayConfig cfg;
    cfg.controlPort = 17782; cfg.audioPort = 17783;
    cfg.ipSecondaryCheck = false;          // identity under test is the token

    Relay relay;
    if (!relay.init(cfg)) { printf("relay init failed\n"); return 1; }
    std::thread th([&]{ relay.run(); });
    nap(200);

    // The .dll bridge: mints the token, captures it via the deliver callback.
    crypto::Token captured{}; bool haveToken = false;
    core::ControlLink bridge;
    if (!bridge.init("127.0.0.1", cfg.controlPort)) { printf("bridge init failed\n"); return 1; }
    bridge.setTokenDeliver([&](uint16_t, const crypto::Token& t){ captured = t; haveToken = true; });

    const uint16_t PID = 42;
    bridge.onPlayerConnect(PID, /*ip=*/0);          // mint + bind + deliver
    bridge.pushPosition(PID, 0, 0, 0, 0, 0, 0);     // create the engine player
    nap(150);
    check("token delivered to bridge", haveToken);

    // Client authenticates with the token.
    TestClient cli;
    bool ok = haveToken && cli.handshake(cfg.audioPort, captured);
    check("client handshake ok", ok && cli.pid == PID);

    // 1) First voice frame -> SpeakerStart reaches the bridge.
    if (ok) cli.sendVoice();
    check("SpeakerStart -> bridge", waitEvent(bridge, StatusType::SpeakerStart, PID, 1500));

    // 2) Key a radio channel -> RadioKey(down) reaches the bridge.
    uint16_t cid = bridge.createChannel(Filter::Radio, /*positional=*/false, 200);
    bridge.joinChannel(PID, cid);
    nap(120);
    bridge.setTransmit(PID, true, 0);
    StatusEvent rk{};
    bool gotRadio = waitEvent(bridge, StatusType::RadioKey, PID, 1500, &rk);
    check("RadioKey -> bridge (down, channel)", gotRadio && rk.channelId == cid && rk.flag == 1);

    // Keep proximity audio alive briefly, then go silent.
    for (int i = 0; i < 3 && ok; ++i) { cli.sendVoice(); nap(100); }

    // 3) Silence -> SpeakerStop after the engine silence timeout (~1.6s).
    check("SpeakerStop -> bridge", waitEvent(bridge, StatusType::SpeakerStop, PID, 3000));

    bridge.shutdown();
    relay.stop();
    th.join();
    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 2 : 0;
}
