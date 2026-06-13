# /client — the .asi (GTA SA injected module)

Captures the mic, encodes Opus, talks to the relay, decodes + plays back.

## Status (2026-06-12): functional, builds, runtime-unverified
The full client is implemented and compiles (x86, sodium + opus):
`net_session` + `voice_client` (secure transport), `audio_capture` (WASAPI mic
→ Opus → `VoiceClient::sendAudio`), `audio_playback` (relay hooks → Opus →
**stereo** render with pan + occlusion low-pass), `dllmain` (orchestration),
`client_config` (voicechat.ini). NOT yet run inside GTA SA. Still TODO:
RNNoise/AEC (stage C), the on-screen overlay (stage G2, needs the legacy
MinHook/D3D9 code).

## What changed in v2
The **security + transport** layer is rewritten against `/shared`:
`client/include/net_session.h` + `net_session.cpp` replace the old
`NetworkClient.hpp` (which identified the player by source IP and never
actually used the token). New flow: token from the `.dll` → cleartext
handshake with X25519 key-exchange → AEAD-sealed audio (req. A, B2, B3).

## File migration map (from the old tree)
| old (voicechat-asi/client-asi)        | new (client/)                         | change |
|---------------------------------------|---------------------------------------|--------|
| `src/dllmain.cpp`                      | `src/dllmain.cpp`                     | swap NetworkClient → net_session; feed `TokenInbox` |
| `include/NetworkClient.hpp`            | `include/net_session.h` (+.cpp)       | **rewritten** (handshake + AEAD) |
| `include/AudioCapture.hpp`             | `include/AudioCapture.hpp`            | unchanged; call `Session::sendAudio()` |
| `include/AudioPlayback.hpp`            | `include/AudioPlayback.hpp`           | **stage D**: add local pan + per-bus mixing |
| `include/VoiceASI.hpp`                 | use `shared/protocol.h`               | drop the duplicated packet structs |
| `include/Config.hpp`                   | `include/Config.hpp`                  | client-only prefs (keep); add handshake limits |
| `include/SAMP_API.hpp`                 | `include/SAMP_API.hpp`               | no longer needed for identity (token replaces it) |
| `include/SpeakerNameplates.hpp` etc.   | keep                                  | UI (stage G2) |

## Hybrid playback (stage D)
`net_session` exposes two hooks:
* `onProximity(speaker, seq, flags, volume, pan, opus, len)` — decode and
  mix **locally** (apply `volume`, `pan`, and a low-pass if `AF_OCCLUDED`).
* `onBus(channel, filter, seq, volume, opus, len)` — already filtered and
  mixed server-side; just decode and play at the category volume.

## Client config (`voicechat.ini`) — NOT authoritative
Keeps ONLY local preferences (devices, master/category volume, PTT key, mic
sensitivity/VAD threshold, overlay). The server hands down the authoritative
limits at handshake (`Session::limits()`); the client clamps to them and can
never widen range/falloff/allowed-mode (req. H gold rule).
