// ============================================================
//  Stage-F verification: incremental spatial grid + nearest-N + scale
//  File: tests/scale_bench.cpp
//
//  Correctness:
//   * incremental grid: moving a speaker between cells changes who hears it,
//     with no per-tick rebuild (the engine only calls grid.update on pos).
//   * dimension gate still holds (different virtual world => inaudible).
//   * nearest-N (J9): a listener surrounded by more than N speakers ends up
//     hearing exactly the N nearest/loudest.
//  Benchmark:
//   * route a full tick of audio for 50/100/200 players and report timing,
//     asserting it stays well within a generous ceiling (grid scaling).
// ============================================================
#include "../core/include/engine.h"
#include "../shared/protocol.h"

#include <chrono>
#include <random>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>

using namespace vc;
using namespace vc::core;

static int g_fail = 0;
static void check(const char* n, bool ok){ printf("  [%s] %s\n", ok?"PASS":"FAIL", n); if(!ok) ++g_fail; }

// ── A sink that records, per listener, which speakers it received ──
struct Recorder {
    std::unordered_map<uint16_t, std::unordered_set<uint16_t>> got; // listener -> speakers
    long packets = 0;
    void operator()(uint16_t to, const uint8_t* d, size_t len) {
        ++packets;
        if (len >= sizeof(ProximityDown) && d[0] == (uint8_t)PktType::ProximityDown) {
            uint16_t sp; std::memcpy(&sp, d + 1, sizeof(sp));   // ProximityDown.speakerId
            got[to].insert(sp);
        }
    }
    void clear() { got.clear(); packets = 0; }
};

// ── helpers: drive the engine through its real control/audio plane ──
static void sendPos(Engine& e, uint16_t id, float x, float y,
                    uint16_t interior = 0, uint16_t vw = 0)
{
    CtrlPlayerPos p{};
    p.type = (uint8_t)PktType::CtrlPlayerPos;
    p.playerId = id; p.x = x; p.y = y; p.z = 0.f;
    p.interior = interior; p.virtualWorld = vw; p.vehicleState = 0;
    e.onControl(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

static void sendAudio(Engine& e, uint16_t id, uint16_t seq)
{
    static uint8_t opus[40] = { 0 };   // payload is opaque to routing
    e.onAudioFrame(id, seq, 0, opus, sizeof(opus));
}

static void sendMute(Engine& e, uint16_t listener, uint16_t target, bool on)
{
    CtrlPairMute m{};
    m.type = (uint8_t)PktType::CtrlPairMute;
    m.listenerId = listener; m.targetId = target; m.on = on ? 1 : 0;
    e.onControl(reinterpret_cast<const uint8_t*>(&m), sizeof(m));
}

static void sendGone(Engine& e, uint16_t id)
{
    CtrlChanOp g{};
    g.type = (uint8_t)PktType::CtrlPlayerGone;
    g.channelId = INVALID_CHANNEL; g.playerId = id; g.arg = 0;
    e.onControl(reinterpret_cast<const uint8_t*>(&g), sizeof(g));
}

// ─────────────────────────────────────────────────────────────
static void testIncrementalGrid(Recorder& rec)
{
    printf("[incremental grid]\n");
    Recorder local;
    Engine eng([&](uint16_t to, const uint8_t* d, size_t n){ local(to, d, n); });
    eng.tuning().normal = 18.f; eng.setNearestN(0);     // unlimited; isolate the grid

    sendPos(eng, 1, 0, 0);        // listener
    sendPos(eng, 2, 5, 0);        // near speaker (audible)
    sendPos(eng, 3, 100, 0);      // far speaker (inaudible)

    local.clear(); sendAudio(eng, 2, 1);
    check("near speaker heard", local.got[1].count(2) == 1);

    local.clear(); sendAudio(eng, 3, 1);
    check("far speaker NOT heard", local.got[1].count(3) == 0);

    // Move the far speaker into range -> incremental cell move makes it audible.
    sendPos(eng, 3, 5, 5);
    local.clear(); sendAudio(eng, 3, 2);
    check("moved-in speaker now heard", local.got[1].count(3) == 1);

    // Move the near speaker far away -> it drops out without a rebuild.
    sendPos(eng, 2, 300, 0);
    local.clear(); sendAudio(eng, 2, 3);
    check("moved-out speaker now silent", local.got[1].count(2) == 0);

    // Dimension gate (J2): same spot, different virtual world -> inaudible.
    sendPos(eng, 4, 1, 1, /*interior*/0, /*vw*/7);
    local.clear(); sendAudio(eng, 4, 1);
    check("other virtual world NOT heard", local.got[1].count(4) == 0);

    (void)rec;
}

static void testNearestN()
{
    printf("[nearest-N cap (J9)]\n");
    Recorder rec;
    Engine eng([&](uint16_t to, const uint8_t* d, size_t n){ rec(to, d, n); });
    eng.tuning().normal = 30.f; eng.setNearestN(3);

    sendPos(eng, 1, 0, 0);                 // the listener
    const uint16_t spk[6] = { 10, 11, 12, 13, 14, 15 };
    const float    sx [6] = { 2,  4,  6,  8,  10, 12 };   // increasing distance
    for (int i = 0; i < 6; ++i) sendPos(eng, spk[i], sx[i], 0);

    // Warm up so the per-listener near-set converges (online eviction).
    for (int t = 0; t < 5; ++t) {
        for (int i = 0; i < 6; ++i) sendAudio(eng, spk[i], (uint16_t)t);
        eng.tick();
    }
    // Measured tick: only the 3 nearest/loudest should get through now.
    rec.clear();
    for (int i = 5; i >= 0; --i) sendAudio(eng, spk[i], 99);   // adversarial order

    auto& heard = rec.got[1];
    check("listener hears exactly N=3", heard.size() == 3);
    check("hears the 3 nearest (10,11,12)",
          heard.count(10) && heard.count(11) && heard.count(12) &&
          !heard.count(13) && !heard.count(14) && !heard.count(15));
}

static void testPairMute()
{
    printf("[per-pair mute (J11)]\n");
    Recorder rec;
    Engine eng([&](uint16_t to, const uint8_t* d, size_t n){ rec(to, d, n); });
    eng.tuning().normal = 18.f; eng.setNearestN(0);

    sendPos(eng, 1, 0, 0);     // listener
    sendPos(eng, 2, 4, 0);     // speaker (audible)

    rec.clear(); sendAudio(eng, 2, 1);
    check("heard before mute", rec.got[1].count(2) == 1);

    sendMute(eng, /*listener*/1, /*target*/2, true);
    rec.clear(); sendAudio(eng, 2, 2);
    check("silenced after mute", rec.got[1].count(2) == 0);

    // The mute is one-directional & per-listener: speaker 2 still hears 1.
    rec.clear(); sendAudio(eng, 1, 1);
    check("mute is one-way (target still hears listener)", rec.got[2].count(1) == 1);

    sendMute(eng, 1, 2, false);
    rec.clear(); sendAudio(eng, 2, 3);
    check("heard again after unmute", rec.got[1].count(2) == 1);

    // Disconnect clears stale mutes so a reused id isn't wrongly silenced.
    sendMute(eng, 1, 2, true);
    sendGone(eng, 2);
    sendPos(eng, 2, 4, 0);     // a "new" player reuses id 2
    rec.clear(); sendAudio(eng, 2, 4);
    check("reused id not stale-muted", rec.got[1].count(2) == 1);
}

static void benchmark()
{
    printf("[scale benchmark]\n");
    const int counts[] = { 50, 100, 200 };
    const int kTicks = 50;
    double worstMs = 0.0;

    for (int N : counts) {
        Recorder rec;
        Engine eng([&](uint16_t to, const uint8_t* d, size_t n){ rec(to, d, n); });
        eng.tuning().normal = 18.f; eng.setNearestN(8);

        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> field(0.f, 200.f);   // dense-ish crowd
        std::uniform_real_distribution<float> jitter(-2.f, 2.f);

        std::vector<float> px(N), py(N);
        for (int i = 0; i < N; ++i) {
            px[i] = field(rng); py[i] = field(rng);
            sendPos(eng, (uint16_t)(i + 1), px[i], py[i]);
        }

        auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < kTicks; ++t) {
            for (int i = 0; i < N; ++i) {                 // everyone moves a little
                px[i] += jitter(rng); py[i] += jitter(rng);
                sendPos(eng, (uint16_t)(i + 1), px[i], py[i]);
            }
            for (int i = 0; i < N; ++i)                    // ...and everyone talks
                sendAudio(eng, (uint16_t)(i + 1), (uint16_t)t);
            eng.tick();
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms > worstMs) worstMs = ms;
        printf("  N=%-3d  %d ticks  %7.1f ms  (%.3f ms/tick, %ld pkts)\n",
               N, kTicks, ms, ms / kTicks, rec.packets);
    }
    // Generous ceiling: this is a correctness guard against an O(n^2) regression,
    // not a hard perf SLA. 200 players * 50 ticks should be well under this.
    check("200-player routing stays within budget", worstMs < 5000.0);
}

int main()
{
    printf("== Stage-F: incremental grid + nearest-N + scale ==\n");
    Recorder rec;
    testIncrementalGrid(rec);
    testNearestN();
    testPairMute();
    benchmark();
    printf(g_fail ? "\nFAILED (%d)\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
