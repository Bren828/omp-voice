// ============================================================
//  VoiceChat — Cryptography (libsodium wrapper)
//  File: shared/crypto.h
//
//  Used by /relay, /samp, /omp and /client. Wraps libsodium so the
//  rest of the codebase never touches raw sodium calls.
//
//  Primitives
//  ----------
//  * Token        : 32 random bytes from a CSPRNG (randombytes_buf).
//                   Minted by the server (.dll) per connect, shipped to
//                   the client over the authenticated game channel, and
//                   presented in the UDP handshake. Replaces IP identity.
//  * Key exchange : X25519 ephemeral (crypto_kx). Client and relay each
//                   send a public key in the handshake; both derive the
//                   same 32-byte session key. No key ever travels in clear.
//  * AEAD         : XChaCha20-Poly1305-IETF. 24-byte nonce, 16-byte MAC.
//                   Encrypts + authenticates every post-handshake datagram.
//  * Anti-replay  : per-session monotonic 64-bit counter embedded in the
//                   nonce; the receiver rejects stale/duplicate counters.
//
//  Degradation: if VC_NO_SODIUM is defined the wrapper compiles to a
//  pass-through (no encryption) so the audio plane still works for local
//  testing — NEVER ship that build. (req. J7 graceful degradation only
//  applies to "voice off", not "encryption off".)
// ============================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include "protocol.h"

namespace vc::crypto {

using Token      = std::array<uint8_t, TOKEN_BYTES>;
using PubKey     = std::array<uint8_t, 32>;
using SecKey     = std::array<uint8_t, 32>;

// Directional AEAD keys (req. A2). With X25519/crypto_kx each side gets a
// receive key and a transmit key where client.tx == relay.rx and vice
// versa. seal() uses tx, open() uses rx. Using ONE symmetric key both ways
// is wrong here: the two endpoints would derive different keys (order
// dependence) AND it would expose a reflection attack — directional keys
// fix both.
struct SessionKeys {
    std::array<uint8_t, SESSION_KEY_BYTES> rx{};   // decrypt peer -> us
    std::array<uint8_t, SESSION_KEY_BYTES> tx{};   // encrypt us -> peer
};

// Call once per process before any other function here. Returns false
// if libsodium failed to initialise (treat as fatal on server).
bool init();

// ── Tokens ──────────────────────────────────────────────────
// Fill `out` with TOKEN_BYTES of cryptographically secure randomness.
void mintToken(Token& out);

// Constant-time comparison (use for token/MAC checks, never memcmp).
bool equals(const uint8_t* a, const uint8_t* b, size_t n);

// ── Ephemeral key exchange (X25519 via crypto_kx) ───────────
struct KeyPair { PubKey pk; SecKey sk; };
KeyPair generateKeyPair();

// Derive the directional session keys. `isClient` picks the crypto_kx
// role so client.tx == relay.rx and client.rx == relay.tx. Returns false
// on a malformed peer key.
bool deriveSession(const KeyPair& self, const PubKey& peer,
                   bool isClient, SessionKeys& out);

// ── AEAD envelope ───────────────────────────────────────────
// Wire layout produced by seal(): [nonce:24][ciphertext][mac:16].
//
// seal:  plaintext -> out (out must hold len + OVERHEAD bytes).
//        `counter` is the per-session anti-replay counter; pass a value
//        that strictly increases for every packet you send.
// open:  reverses it, verifying MAC and rejecting replayed counters via
//        `lastSeen` (updated in place; pass the session's high-water mark).
constexpr size_t NONCE_BYTES = 24;
constexpr size_t MAC_BYTES   = 16;
constexpr size_t OVERHEAD    = NONCE_BYTES + MAC_BYTES;

// Returns bytes written to `out`, or 0 on failure. Seals with keys.tx.
size_t seal(const SessionKeys& keys, uint64_t counter,
            const uint8_t* plain, size_t plainLen,
            uint8_t* out, size_t outCap);

// Returns plaintext length written to `out`, or 0 on auth/replay failure.
// Opens with keys.rx. `lastSeen` is the highest counter accepted so far;
// updated on success.
size_t open(const SessionKeys& keys, uint64_t& lastSeen,
            const uint8_t* wire, size_t wireLen,
            uint8_t* out, size_t outCap);

} // namespace vc::crypto
