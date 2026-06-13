// ============================================================
//  VoiceChat — Cryptography implementation (libsodium)
//  File: shared/crypto.cpp
// ============================================================
#include "crypto.h"
#include <cstring>

#ifndef VC_NO_SODIUM
#  include <sodium.h>
#endif

namespace vc::crypto {

bool init()
{
#ifdef VC_NO_SODIUM
    return true;  // pass-through build (local testing ONLY)
#else
    return sodium_init() >= 0;
#endif
}

void mintToken(Token& out)
{
#ifdef VC_NO_SODIUM
    // Deterministic-but-unique-ish fallback; NOT secure. Test builds only.
    static uint64_t ctr = 0x9E3779B97F4A7C15ull;
    for (auto& b : out) { ctr = ctr * 6364136223846793005ull + 1442695040888963407ull; b = uint8_t(ctr >> 56); }
#else
    randombytes_buf(out.data(), out.size());
#endif
}

bool equals(const uint8_t* a, const uint8_t* b, size_t n)
{
#ifdef VC_NO_SODIUM
    // Best-effort constant-time memcmp.
    uint8_t d = 0; for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i]; return d == 0;
#else
    return sodium_memcmp(a, b, n) == 0;
#endif
}

KeyPair generateKeyPair()
{
    KeyPair kp{};
#ifdef VC_NO_SODIUM
    Token t{}; mintToken(t);
    std::memcpy(kp.pk.data(), t.data(), 32);
    std::memcpy(kp.sk.data(), t.data(), 32);
#else
    crypto_kx_keypair(kp.pk.data(), kp.sk.data());
#endif
    return kp;
}

bool deriveSession(const KeyPair& self, const PubKey& peer,
                   bool isClient, SessionKeys& out)
{
#ifdef VC_NO_SODIUM
    // XOR of the two pubkeys, same both ways — placeholder, NOT secure.
    for (size_t i = 0; i < out.rx.size(); ++i) {
        out.rx[i] = self.pk[i] ^ peer[i];
        out.tx[i] = out.rx[i];
    }
    (void)isClient; return true;
#else
    int rc = isClient
        ? crypto_kx_client_session_keys(out.rx.data(), out.tx.data(),
                                        self.pk.data(), self.sk.data(), peer.data())
        : crypto_kx_server_session_keys(out.rx.data(), out.tx.data(),
                                        self.pk.data(), self.sk.data(), peer.data());
    return rc == 0;   // client.tx == relay.rx, client.rx == relay.tx
#endif
}

size_t seal(const SessionKeys& keys, uint64_t counter,
            const uint8_t* plain, size_t plainLen,
            uint8_t* out, size_t outCap)
{
    const auto& key = keys.tx;
    if (outCap < plainLen + OVERHEAD) return 0;

    uint8_t* nonce = out;                 // [0 .. 24)
    uint8_t* ct    = out + NONCE_BYTES;   // ciphertext + MAC

#ifdef VC_NO_SODIUM
    // Counter in the nonce TAIL (big-endian), the SAME place open() reads it
    // from — otherwise the reconstructed counter is always 0 and the replay
    // check never fires. Mirrors the real-sodium layout below.
    std::memset(nonce, 0, NONCE_BYTES);
    for (int i = 0; i < 8; ++i)
        nonce[NONCE_BYTES - 1 - i] = uint8_t(counter >> (8 * i));
    std::memcpy(ct, plain, plainLen);
    std::memset(ct + plainLen, 0, MAC_BYTES); // fake MAC
    (void)key;
    return NONCE_BYTES + plainLen + MAC_BYTES;
#else
    // Nonce = 16 random bytes || 8-byte big-endian counter. Random prefix
    // means even a counter reuse across sessions won't collide.
    randombytes_buf(nonce, NONCE_BYTES - sizeof(counter));
    for (int i = 0; i < 8; ++i)
        nonce[NONCE_BYTES - 1 - i] = uint8_t(counter >> (8 * i));

    unsigned long long clen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            ct, &clen, plain, plainLen,
            nullptr, 0, nullptr, nonce, key.data()) != 0)
        return 0;
    return NONCE_BYTES + (size_t)clen;
#endif
}

size_t open(const SessionKeys& keys, uint64_t& lastSeen,
            const uint8_t* wire, size_t wireLen,
            uint8_t* out, size_t outCap)
{
    const auto& key = keys.rx;
    if (wireLen < OVERHEAD) return 0;

    const uint8_t* nonce = wire;
    const uint8_t* ct    = wire + NONCE_BYTES;
    size_t ctLen         = wireLen - NONCE_BYTES;

    // Reconstruct the counter from the nonce tail for replay checks.
    uint64_t counter = 0;
    for (int i = 0; i < 8; ++i)
        counter |= uint64_t(nonce[NONCE_BYTES - 1 - i]) << (8 * i);

#ifdef VC_NO_SODIUM
    if (ctLen < MAC_BYTES) return 0;
    size_t plainLen = ctLen - MAC_BYTES;
    if (plainLen > outCap) return 0;
    if (counter && counter <= lastSeen) return 0; // replay
    std::memcpy(out, ct, plainLen);
    if (counter > lastSeen) lastSeen = counter;
    (void)key;
    return plainLen;
#else
    if (outCap + MAC_BYTES < ctLen) return 0;
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            out, &mlen, nullptr, ct, ctLen,
            nullptr, 0, nonce, key.data()) != 0)
        return 0;  // bad MAC / forged / corrupted
    if (counter && counter <= lastSeen) return 0;       // replayed
    if (counter > lastSeen) lastSeen = counter;
    return (size_t)mlen;
#endif
}

} // namespace vc::crypto
