// ============================================================
//  Stage-A verification: token + X25519 handshake + AEAD round-trip
//  File: tests/crypto_roundtrip.cpp
//
//  Proves the directional-key fix: both sides derive agreeing keys, audio
//  seals/opens both ways, replays are rejected, tampering is rejected, and
//  a wrong token never matches. Build with -DVC_BUILD_TESTS=ON.
// ============================================================
#include "../shared/crypto.h"
#include <cstdio>
#include <cstring>

using namespace vc;

static int g_fail = 0;
static void check(const char* name, bool ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

int main()
{
    if (!crypto::init()) { printf("sodium init failed\n"); return 1; }
    printf("Stage-A crypto round-trip:\n");

    // --- token ---
    crypto::Token t1, t2;
    crypto::mintToken(t1); crypto::mintToken(t2);
    check("tokens are unique", !crypto::equals(t1.data(), t2.data(), TOKEN_BYTES));
    check("token equals itself", crypto::equals(t1.data(), t1.data(), TOKEN_BYTES));

    // --- handshake (X25519) ---
    auto client = crypto::generateKeyPair();
    auto relay  = crypto::generateKeyPair();
    crypto::SessionKeys ck{}, rk{};
    bool dc = crypto::deriveSession(client, relay.pk, /*isClient=*/true,  ck);
    bool dr = crypto::deriveSession(relay,  client.pk,/*isClient=*/false, rk);
    check("both sides derive keys", dc && dr);
    // The whole point of directional keys: client.tx must equal relay.rx.
    check("client.tx == relay.rx",
          0 == memcmp(ck.tx.data(), rk.rx.data(), SESSION_KEY_BYTES));
    check("relay.tx == client.rx",
          0 == memcmp(rk.tx.data(), ck.rx.data(), SESSION_KEY_BYTES));

    uint8_t wire[256], out[256];

    // --- client -> relay ---
    const char* m1 = "AudioUp:hello-voice";
    size_t w1 = crypto::seal(ck, 1, (const uint8_t*)m1, strlen(m1), wire, sizeof(wire));
    uint64_t relaySeen = 0;
    size_t o1 = crypto::open(rk, relaySeen, wire, w1, out, sizeof(out));
    check("c->r decrypts", o1 == strlen(m1) && 0 == memcmp(out, m1, o1));

    // --- replay of the same packet is rejected ---
    size_t o1b = crypto::open(rk, relaySeen, wire, w1, out, sizeof(out));
    check("c->r replay rejected", o1b == 0);

    // --- tampered ciphertext is rejected (needs a real MAC) ---
#ifndef VC_NO_SODIUM
    wire[w1 - 1] ^= 0xFF;
    uint64_t s2 = 0;
    size_t o1c = crypto::open(rk, s2, wire, w1, out, sizeof(out));
    check("c->r tamper rejected", o1c == 0);
#endif

    // --- relay -> client (reverse direction) ---
    const char* m2 = "ProximityDown:pan+vol";
    size_t w2 = crypto::seal(rk, 1, (const uint8_t*)m2, strlen(m2), wire, sizeof(wire));
    uint64_t clientSeen = 0;
    size_t o2 = crypto::open(ck, clientSeen, wire, w2, out, sizeof(out));
    check("r->c decrypts", o2 == strlen(m2) && 0 == memcmp(out, m2, o2));

    // --- a different session's key cannot open it (no reflection/confusion) ---
#ifndef VC_NO_SODIUM
    auto other = crypto::generateKeyPair();
    crypto::SessionKeys ok{};
    crypto::deriveSession(other, relay.pk, true, ok);
    uint64_t s3 = 0;
    size_t o3 = crypto::open(ok, s3, wire, w2, out, sizeof(out));
    check("wrong-session key rejected", o3 == 0);
#endif

    printf("%s (%d failure(s))\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 2 : 0;
}
