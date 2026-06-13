# Security model (Stage A)

## Identity: token, not IP
Old system identified a speaker by the **source IP** of their UDP packets —
trivially spoofable and unable to tell two players behind one NAT apart.

v2 uses a per-connect **cryptographic token**:

1. On connect, the `.dll` mints 32 random bytes from a CSPRNG
   (`randombytes_buf`), sends `CtrlBindToken{token, playerid, expiry, ip}` to
   the relay over loopback, and delivers the token to the client.
2. The client presents the token in its UDP `HandshakeReq`.
3. The relay matches the token to a `playerid`, **binds the observed socket**
   to that id, and from then on identifies the player by the established
   session — not the IP.

Properties:
- **Single-use**: the pending entry is erased on redemption; a reconnect
  mints a fresh token (an old token can't be reused).
- **TTL-bound**: tokens expire (`[security] token_ttl_seconds`, default 30s);
  the relay sweeps expired pending tokens.
- **IP secondary check**: the relay verifies the handshake's source IP equals
  the IP the `.dll` registered (`[security] ip_secondary_check`). Defends a
  *sniffed* token replayed from a different host. Disable for double-NAT/proxy.

## Confidentiality + integrity: AEAD
After the handshake (X25519 ephemeral key exchange, `crypto_kx`), both sides
hold **directional** keys (`client.tx == relay.rx`, and vice-versa). Every
post-handshake datagram is **XChaCha20-Poly1305**:
`[nonce:24][ciphertext][mac:16]`.

- Forged/tampered packets fail the MAC and are dropped.
- **Anti-replay**: a 64-bit counter rides in the nonce tail; each receiver
  tracks a high-water mark and rejects stale/duplicate counters.
- Directional keys also block **reflection** (a packet a peer sent can't be
  bounced back and decrypted by that same peer).

Verified by `tests/crypto_roundtrip.cpp` (10 assertions) and
`tests/e2e_handshake.cpp` (real sockets: accept, bad-token reject, IP reject).

## Token delivery channel
The token must reach the client over a channel an attacker can't read.

| Channel | Status | Notes |
|---------|--------|-------|
| **Legacy cmd UDP** (`.dll → .asi :7780`) | implemented (default) | token sent to the player's game IP. Combined with single-use + short TTL + IP-bind, a sniffed token can't be redeemed from another host. Residual risk: an on-path attacker who can both read the token AND spoof the client's source IP. Acceptable for most RP servers; not for hostile-network deployments. |
| **open.mp RakNet packet** | recommended (omp) | the component sends the token via the public network API — already authenticated + encrypted per player. No extra channel. Wire in `omp/` `onLoad` (`g_link.setTokenDeliver`). |
| **SA-MP RakNet hook** | scaffold, opt-in (`-DVC_SAMP_RAKNET=ON`) | gold standard for SA-MP. Interface acquisition wired; `Send` + client receive hook are the on-target follow-up. Design + steps in `samp/raknet/README.md`. |

### SA-MP RakNet delivery (hardening, follow-up)
SA-MP classic has no public API to push a custom packet over the game's
RakNet stream, so it requires a hook:

- **Server (`samp/raknet/`)**: hook `RakServer::Send` (vtable hook on the
  `RakServerInterface`, the well-known RakSAMP approach) and emit a custom
  packet id (e.g. `ID_VOICE_TOKEN`) addressed to the connecting player. The
  token rides inside the already-encrypted RakNet session — no separate UDP.
- **Client (`client/raknet/`)**: in the `.asi`, hook the RakNet receive path
  (the existing MinHook setup already hooks the client) and intercept
  `ID_VOICE_TOKEN`, feeding it to `TokenInbox` instead of the UDP listener.

This removes the cmd-UDP channel entirely and closes the on-path read risk.
Scoped as a follow-up because it needs the RakNet structs/offsets for the
target SA-MP build and is platform-fiddly.

## What's authoritative (anti-cheat)
The server owns position, interior, virtual world, range, falloff, occlusion
and allowed mode. The client **reports none of these** (req. J10) and at
handshake receives the effective limits it must clamp to (req. H), so a
modified client cannot grant itself longer range ("audio wallhack").
