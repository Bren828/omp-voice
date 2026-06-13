# VoiceChat v2 — Architecture & Roadmap

Dual-platform (open.mp + SA-MP 0.3.7 R5) positional + radio voice, one
codebase, hybrid server/client mixing, token-based crypto identity.

---

## 1. Module layout

```
/shared   protocol.h (single source of truth), crypto (libsodium), udp, relay_launcher
/core     platform-independent engine: registry, spatial grid, proximity, mixer,
          engine (relay brain), control_link (bridge brain), opus wrapper
/samp     SA-MP 0.3.7 R5 plugin: 5 exports + .def, AMX natives -> core::ControlLink
/omp      open.mp component: same natives, same core, IComponent ABI (stub until SDK)
/relay    standalone .exe: transport + crypto sessions around core::Engine
/client   .asi: WASAPI+Opus capture/playback + secure net_session
```

**Rule:** nothing in `/core` or `/shared` includes a platform/SAMP/omp API.
Both bridges link the *same* `vc_core`. Adding open.mp later touches only
`/omp`, never `/core`.

### Where each piece runs
```
   ┌─────────────── GAME SERVER (open.mp / SA-MP) ───────────────┐
   │  gamemode.pwn ──natives──► /samp or /omp bridge             │
   │                              │ core::ControlLink            │
   │                              │ (mint token, push pos/chans) │
   └──────────────────────────────┼──────────────────────────────┘
                                   │ loopback UDP :7778 (control)
                                   ▼
                         ┌──────── RELAY .exe ────────┐
   players' .asi ──UDP:7779──►  core::Engine          │
   (audio, AEAD)        ◄──────  (grid, proximity,     │
                                  hybrid mixer)        │
                         └────────────────────────────┘
```

---

## 2. Delivery roadmap (priority A > B > E/D > F)

This commit = **the skeleton** (compilable structure, real protocol/crypto
wrappers, proximity math, routing; DSP/mix marked as stages).

| Stage | Scope | Key files |
|-------|-------|-----------|
| **0 skeleton** ✅ | 6 modules, shared protocol, crypto wrapper, proximity routing, token handshake end-to-end, CMake | done |
| **A security** ✅ | directional-key AEAD (libsodium real), voice.ini `[security]` loaded, single-use + TTL tokens, IP secondary check, max-players cap; verified by `tests/`. Residual: SA-MP RakNet delivery (`docs/SECURITY.md`) | `shared/crypto`, `relay`, `core/control_link`, `tests/` |
| **B reliability** ✅ | watchdog backoff, heartbeat, relay→bridge status channel → Pawn callbacks (`OnPlayerStartTalking/StopTalking/RadioKey` via SA-MP `ProcessTick`), client reconnect w/ fresh token on timeout. Verified by `tests/e2e_status`. RakNet token delivery opt-in (`samp/raknet/`) | `core/control_link`, `samp/plugin`, `client/voice_client`, `shared/relay_launcher.h` |
| **D 3D** ✅ | dimension gate, 3D dist, log falloff, vehicle occlusion, stereo pan, ranges | `core/proximity` |
| **E radio/phone bus** ✅ | per-listener decode→filter→mix→re-encode `BusDown`: bandpass+drive+static (radio), narrowband (phone), squelch/roger beep, half-duplex garble, duck/exclusive policy, J5 double-play suppression, J12 limiter. libopus via FetchContent (`VC_WITH_OPUS=ON`). Verified `tests/dsp`. | `core/dsp.*`, `core/bus_router.*`, `core/opus_codec`, `core/mixer` |
| **F scale** | incremental grid, nearest-N cap, per-listener bus reuse, benchmarks @50/100/200 | `core/spatial_grid`, `core/engine` |
| **G/H/I/J** | natives/callbacks/UI, voice.ini full load + push, channel API polish, correctness scenarios | bridges, `core/engine` |

---

## 3. Handshake (token flow, req. A)

```
 gamemode        .dll bridge            game net         .asi client            relay .exe
   │ OnPlayerConnect                       │                  │                     │
   ├─Voice_OnPlayerConnect(pid)──►│        │                  │                     │
   │                               │ mintToken() (CSPRNG)     │                     │
   │                               ├─ CtrlBindToken(token,pid,ttl) ───loopback───► store token→pid
   │                               ├─ deliver token ─►(RakNet / cmd UDP)─► TokenInbox │
   │                               │        │                  │                     │
   │                               │        │   connect(): keypair, HandshakeReq      │
   │                               │        │   [token + clientPubKey]  (CLEARTEXT)   │
   │                               │        │                  ├────────UDP:7779─────►│ validate token
   │                               │        │                  │                     │ derive key (X25519)
   │                               │        │                  │◄── HandshakeAck ─────┤ bind socket↔pid
   │                               │        │                  │  [relayPubKey +      │ (IP = 2nd check only)
   │                               │        │                  │   effective limits]  │
   │                               │        │   derive same session key               │
   │                               │        │                  │═══ AEAD audio ═══════│ (XChaCha20-Poly1305)
```

Identity is the **token**, not the IP. Token is single-use and TTL-bound;
a new one is minted on every connect/reconnect. The relay binds the
observed socket to the playerid on a valid token and uses IP only as a
secondary sanity check.

---

## 4. UDP protocol (see `shared/protocol.h` for exact structs)

Two planes:

* **Control plane** — `.dll → relay`, loopback `:7778`. `Ctrl*` packets:
  bind-token, config push, authoritative position (x,y,z,**interior,vw**,
  vehicle), player meta (mute/deafen/alive/range), channel create/destroy/
  join/leave/filter/priority, transmit on/off + channel mask, mix policy.

* **Audio plane** — `.asi ↔ relay`, `:7779`. Handshake is cleartext; every
  later datagram is an **AEAD envelope**: `[nonce:24][ciphertext][mac:16]`.
  Plaintext payloads:
  * `AudioUp` (C→R): `seq, flags(VAD/PTT), len, opus[]`. No position — the
    server is authoritative (req. J10).
  * `ProximityDown` (R→C): `speaker, seq, flags, volume, pan, opus[]` — the
    client spatialises + mixes locally (hybrid).
  * `BusDown` (R→C): `channel, filter, volume, opus[]` — already filtered &
    mixed per-listener on the server.
  * `PlayerGone`, `Ping/Pong`, `Bye`.

Anti-replay: a 64-bit counter rides in the nonce tail; the receiver tracks
a high-water mark and drops stale/duplicate counters.

---

## 5. Pawn API

### Natives (channel primitive — the only gameplay-agnostic surface)
```pawn
Voice_CreateChannel(filter, bool:positional, priority = 100);  // -> channelid
Voice_DestroyChannel(channelid);
Voice_AddToChannel(playerid, channelid);
Voice_RemoveFromChannel(playerid, channelid);
Voice_IsInChannel(playerid, channelid);
Voice_SetChannelFilter(channelid, filter);     // NONE | RADIO | PHONE
Voice_SetChannelPriority(channelid, priority);
```
### Natives (player control)
```pawn
Voice_UpdatePosition(playerid, Float:x, Float:y, Float:z, interior, vworld, vehState);
Voice_SetPlayerRange(playerid, preset, Float:override_units = 0.0);  // WHISPER/NORMAL/SHOUT
Voice_SetPlayerFlags(playerid, bool:muted, bool:deafen, bool:alive, bool:spectator);
Voice_SetTransmitting(playerid, bool:on, channelMask = 0);
Voice_SetMixPolicy(playerid, policy, Float:duckLevel = 0.5, Float:proximityFloor = 0.25);
Voice_MutePlayer(playerid, targetid);          // per-pair mute (stage G)
```
### Natives (lifecycle)
```pawn
Voice_OnPlayerConnect(playerid);               // mint + deliver token
Voice_OnPlayerDisconnect(playerid);            // full cleanup, all channels
Voice_RegisterPlayerIp(playerid, const ip[]);  // token delivery (legacy chan)
```
### Callbacks (plugin → gamemode, fired via relay status; stage B/G)
```pawn
forward OnPlayerStartTalking(playerid);
forward OnPlayerStopTalking(playerid);
forward OnPlayerRadioKey(playerid, channelid, bool:down);
```

Gameplay (phone/taxi/faction) is built in Pawn from channels + filters +
members; the plugin stays unopinionated (req. I).

---

## 6. Build — see `docs/BUILD.md`.
```

```
