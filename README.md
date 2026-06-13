# omp-voice

**VoiceChat v2** — proximity + radio/phone **voice chat** for **open.mp** and
**SA-MP 0.3.7 R5**, with a Windows `.asi` client. Server-authoritative positioning,
cryptographic-token identity, end-to-end AEAD-encrypted audio, neural noise suppression,
and an in-game ImGui control panel.

[![build](https://github.com/staark-dev/omp-voice/actions/workflows/build.yml/badge.svg)](https://github.com/staark-dev/omp-voice/actions/workflows/build.yml)
[![release](https://github.com/staark-dev/omp-voice/actions/workflows/release.yml/badge.svg)](https://github.com/staark-dev/omp-voice/actions/workflows/release.yml)
[![latest release](https://img.shields.io/github/v/release/staark-dev/omp-voice?include_prereleases&sort=semver)](https://github.com/staark-dev/omp-voice/releases)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![open.mp](https://img.shields.io/badge/open.mp-component-1f6feb)](https://open.mp/)
[![SA-MP](https://img.shields.io/badge/SA--MP-0.3.7%20R5-f59f00)](https://www.sa-mp.mp/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C)](#)
[![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)](#)

---

## Table of contents
- [What it does](#what-it-does)
- [Architecture](#architecture)
- [Repository layout](#repository-layout)
- [Build](#build)
- [Install / deploy](#install--deploy)
- [Networking & ports](#networking--ports)
- [Configuration](#configuration)
- [Pawn API](#pawn-api)
- [In-game controls (client)](#in-game-controls-client)
- [Test filterscript](#test-filterscript)
- [Status](#status)

---

## What it does

- **Proximity voice** — hear nearby players in 3D: logarithmic distance falloff, stereo
  panning, interior/virtual-world gating, and **occlusion** (muffled through walls and
  closed vehicles). Whisper / Normal / Shout ranges, or a custom range.
- **Radio & phone channels** — filtered "buses" (bandpass + squelch + half-duplex garble
  for radio, narrowband colouring for phone), mixed **per-listener** on the server.
- **Generic channel primitive** — the plugin exposes *channels*; gameplay (faction radio,
  taxi dispatch, phone calls) is built in Pawn on top of channels + filters + members.
- **Mix policy** — `MIX` / `DUCK` / `EXCLUSIVE` across a listener's active buses;
  proximity is never ducked below a floor.
- **Per-pair mute** — a listener can locally silence one target without affecting anyone
  else.
- **Speakerphone (J6)** — a player's received call audio is also played into their
  proximity so bystanders hear the call "out of the phone".
- **Client audio quality** — Opus codec, **RNNoise** neural noise suppression, an
  **adaptive jitter buffer**, exclusive **Push-to-Talk *or* Voice-Activation** modes.
- **In-game UI** — a Dear ImGui control panel (INSERT) for volumes, devices, mode and
  PTT-key rebind, plus a "who's talking" HUD and optional floating labels above heads.
- **Security** — identity is a per-connect **cryptographic token** (not the source IP);
  every relay↔client datagram is sealed with **XChaCha20-Poly1305** AEAD; the server is
  authoritative for position/flags (anti-spoof).

## Architecture

Hybrid mixing model:

- **Proximity** voice is relayed *per speaker* as opaque Opus with a server-computed
  volume/pan/occlusion; the **client** spatialises and mixes it (lowest latency).
- **Filtered channels** (radio/phone) are decoded, filtered and mixed **per listener on
  the server**, re-encoded, and sent as one bus stream the client just plays.

```
                 ┌─────────────────────────── game server host ───────────────────────────┐
   GTA SA client │   SA-MP plugin (.dll) / open.mp component  ── loopback IPC ──► relay     │
  ┌────────────┐ │   (mints tokens, pushes authoritative pos/flags/channels)     (.exe)    │
  │ VoiceChat  │ │                                                                 │ core   │
  │   .asi     │◄┼──── AEAD audio (UDP) ──────────────────────────────────────────┘ engine │
  │ WASAPI+Opus│ │     proximity (raw Opus) + buses (server-mixed)                          │
  └────────────┘ └──────────────────────────────────────────────────────────────────────┘
```

Six modules, one tree:

| Module      | Role | Bitness |
|-------------|------|---------|
| `shared/`   | wire protocol, crypto (libsodium), INI, UDP — platform-free | any |
| `core/`     | routing engine: proximity, bus router, channels, mix policy | any |
| `relay/`    | transport + AEAD shell around the core (standalone process) | x64 |
| `samp/`     | SA-MP 0.3.7 R5 plugin bridge (Pawn natives → core) | x86 |
| `omp/`      | open.mp component bridge (same core) | x64 |
| `client/`   | Windows `.asi`: WASAPI capture/playback, Opus, RNNoise, ImGui panel | x86 |

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and [`docs/SECURITY.md`](docs/SECURITY.md)
for the locked design decisions.

## Repository layout

```
shared/     protocol.h, crypto, ini.h, udp.h, relay_launcher.h
core/       engine, bus_router, proximity, dsp, mixer, registry, control_link, opus_codec
relay/      relay.cpp + main.cpp (voice_relay)            config.h reads voice.ini
samp/       plugin.cpp (+ include/voicechat.inc, voicechat_ui.inc; vendored sdk/)
omp/        component.cpp (open.mp; gated by VC_WITH_OMP_SDK)
client/     dllmain, audio_capture, audio_playback, voice_client, net_session,
            d3d9_overlay (ImGui), voice_panel, overlay (HUD), client_config.h
examples/   voicechat_test.pwn — a command-per-native test filterscript
tests/      crypto, dsp, e2e handshake/status/config, scale benchmark
docs/       ARCHITECTURE.md, BUILD.md, SECURITY.md
build.ps1   one-shot local build + deploy
voice.ini   server config (annotated)
```

Third-party deps are fetched + built by CMake (`FetchContent`): **libsodium**, **opus**,
**Dear ImGui**, **MinHook**, **RNNoise** (`cpuimage/rnnoise`). The SA-MP SDK is vendored
in `samp/sdk/`. Nothing else needs to be installed.

## Build

### One-shot (Windows, local)

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Builds the relay, the SA-MP plugin and the `.asi` client (Release, x86), compiles the
test filterscript with `pawncc`, and deploys everything to the local server + each GTA
install (with `.bak-<stamp>` backups). Flags: `-NoDeploy`, `-NoPawn`, `-Configure`.
Locked files (server/GTA running) are skipped with a warning instead of aborting.

### Manual CMake

```bash
# production config (real crypto + opus + full client), 32-bit:
cmake -S . -B build -A Win32 -DVC_NO_SODIUM=OFF -DVC_WITH_OPUS=ON -DVC_BUILD_CLIENT=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

Key CMake options (defaults in brackets):

| Option | Meaning |
|--------|---------|
| `VC_BUILD_RELAY` [ON] | build the standalone relay |
| `VC_BUILD_SAMP` [ON]  | build the SA-MP plugin (x86) |
| `VC_BUILD_OMP` [OFF]  | build the open.mp component (needs `omp/sdk`, x64) |
| `VC_BUILD_CLIENT` [OFF] | build the `.asi` client (Windows x86) |
| `VC_WITH_OPUS` [OFF]  | link libopus (required for buses + client) |
| `VC_NO_SODIUM` [ON]   | **pass-through crypto for testing** — set `OFF` for production |
| `VC_CLIENT_GUI` [ON]  | ImGui D3D9 control panel in the client |
| `VC_WITH_RNNOISE` [ON]| RNNoise noise suppression in the client |
| `VC_BUILD_TESTS` [ON] | build the verification tests |

> The client and SA-MP plugin must be **x86** (`-A Win32`); the relay and open.mp
> component are x64. Build the pieces you need per configure — see [`docs/BUILD.md`](docs/BUILD.md).

### CI & Releases

GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml)) builds the
**Windows x86** production stack and a **Linux x64** relay+core, runs the tests, and
uploads the binaries as artifacts on every push/PR to `main`.

Pushing a `v*` tag runs [`.github/workflows/release.yml`](.github/workflows/release.yml),
which publishes a **GitHub Release** with ready-to-use bundles — `voicechat-client-win32.zip`,
`voicechat-server-win.zip`, and `voicechat-relay-linux-x64.tar.gz` (each with an
`INSTALL.txt`):

```bash
git tag v0.1.0 && git push origin v0.1.0
```

Building on **Linux** (relay, 32-bit SA-MP `.so`, open.mp component): see
[`docs/LINUX.md`](docs/LINUX.md).

## Install / deploy

**Server** (SA-MP or open.mp):
1. `plugins/VoiceChat.dll` (SA-MP) or `components/` (open.mp) + `components/voice_relay.exe`.
2. `qawno/include/voicechat.inc` (+ `voicechat_ui.inc`) for compiling your gamemode.
3. `voice.ini` next to the server executable.
4. Add the plugin to `config.json` (`legacy_plugins: ["VoiceChat"]`) / `server.cfg`.
5. Call the lifecycle natives from your gamemode (see below). The plugin launches +
   supervises the relay automatically.

**Client** (each player's GTA SA folder):
1. `VoiceChat.asi` (needs an ASI loader, e.g. SilentPatch/CLEO).
2. `voicechat.ini` — set `relay_ip` to the server's IP (`127.0.0.1` if you host locally).

## Networking & ports

UDP only. Full details + firewall commands in [`docs/NETWORKING.md`](docs/NETWORKING.md).

| Port (default) | Direction | Expose? |
|----------------|-----------|---------|
| **7779** audio   | client ⇄ relay (voice + handshake) | **YES — open inbound on the server** |
| **7778** control | plugin → relay (loopback IPC) | No — keep local |
| **7780** cmd     | server → client (token delivery) | Client side (LAN: automatic; internet: may need forward) |

Open UDP 7779 on the server:

```powershell
# Windows (admin)
netsh advfirewall firewall add rule name="VoiceChat 7779" dir=in action=allow protocol=UDP localport=7779
```
```bash
# Linux
sudo ufw allow 7779/udp
```

All ports are overridable: server in `voice.ini [network]`, client in
`voicechat.ini [network]` (only `audio_port` must match on both sides).

## Configuration

### `voice.ini` (server — authoritative)

Annotated; the relay reads it at boot and the plugin re-pushes it over IPC. Sections:
`[network]` (ports), `[security]` (IPC secret), `[proximity]` (whisper/normal/shout
ranges, falloff exponent, occlusion, `nearest_n`, `max_streams_per_client`), `[audio]`
(`vad_allowed`, `default_mode`, bitrate), `[radio]` (bandpass/phone bands, drive, squelch,
static, garble), `[behavior]` (`suppress_double_play`, **`speakerphone`** server-wide
enable, `dead_can_talk`, `proximity_floor`, limiter ceiling). Edit live and call
`Voice_ReloadConfig()`.

### `voicechat.ini` (client — local prefs, never affects others)

```ini
[network]
relay_ip   = 127.0.0.1     ; server IP (LAN IP if the server is on another PC)
audio_port = 7779
cmd_port   = 7780
[keys]
ptt_key  = B               ; proximity push-to-talk
ptt_key2 = N               ; radio push-to-talk (use with your /radio command)
mute_key = F9
panel_key = INSERT         ; open the settings panel
[audio]
master_volume = 1.0
mic_volume    = 1.0
vad_enabled   = false      ; false = PTT only, true = Voice-Activation only (exclusive)
vad_threshold = 0.02
deafen        = false
noise_suppress = true      ; RNNoise
[ui]
show_overlay   = true
overlay_key    = F8
overlay_anchor = radar     ; above the minimap; or "topleft" + overlay_x/overlay_y
```

## Pawn API

`#include <voicechat>` (and optionally `#include <voicechat_ui>`).

### Channels
```pawn
Voice_CreateChannel(filter, bool:positional, priority = 100);   // -> channelid
Voice_DestroyChannel(channelid);
Voice_AddToChannel(playerid, channelid);
Voice_RemoveFromChannel(playerid, channelid);
Voice_IsInChannel(playerid, channelid);
Voice_SetChannelFilter(channelid, filter);     // VOICE_FILTER_NONE/RADIO/PHONE
Voice_SetChannelPriority(channelid, priority);
// queries (answered from the bridge mirror, no round-trip):
Voice_IsValidChannel(channelid);
Voice_GetChannelCount();
Voice_GetChannelMemberCount(channelid);
Voice_GetChannelPlayers(channelid, players[], maxplayers = sizeof players);  // -> count
```

### Player control
```pawn
Voice_UpdatePosition(playerid, Float:x, Float:y, Float:z, interior, vworld, vehState);
Voice_SetPlayerRange(playerid, preset, Float:override_units = 0.0);  // VOICE_RANGE_*
Voice_SetPlayerFlags(playerid, bool:muted, bool:deafen, bool:alive, bool:spectator);
Voice_SetTransmitting(playerid, bool:on, channelMask = 0);          // 1 << (channelid & 31)
Voice_SetMixPolicy(playerid, policy, Float:duckLevel = 0.5, Float:proximityFloor = 0.25);
Voice_MutePlayer(playerid, targetid, bool:mute = true);             // per-pair (J11)
Voice_SetSpeakerphone(playerid, bool:on = true);                    // J6
```

### Lifecycle & config
```pawn
Voice_OnPlayerConnect(playerid, const ip[]);   // GetPlayerIp — mints+delivers the token
Voice_OnPlayerDisconnect(playerid);
Voice_ReloadConfig();                           // re-read voice.ini at runtime
stock Voice_SetupProximity();                   // convenience: a proximity channel
```

### Callbacks (fired by the plugin into your script)
```pawn
forward OnPlayerStartTalking(playerid);
forward OnPlayerStopTalking(playerid);
forward OnPlayerRadioKey(playerid, channelid, bool:down);
```

`voicechat_ui.inc` turns those into floating "(( talking ))" / "(( radio ))" labels above
heads — call `VoiceUI_OnStartTalking/OnStopTalking/OnRadioKey/OnDisconnect` from your
callback bodies.

Minimal wiring:
```pawn
#include <voicechat>
#include <voicechat_ui>
public OnPlayerConnect(playerid) {
    new ip[16]; GetPlayerIp(playerid, ip, sizeof ip);
    Voice_OnPlayerConnect(playerid, ip);
    Voice_AddToChannel(playerid, gProximity);
    Voice_SetPlayerRange(playerid, VOICE_RANGE_NORMAL);
    return 1;
}
public OnPlayerUpdate(playerid) {
    new Float:x,Float:y,Float:z; GetPlayerPos(playerid,x,y,z);
    new veh = GetPlayerVehicleID(playerid) ? VOICE_VEH_CLOSED : VOICE_ONFOOT;
    Voice_UpdatePosition(playerid,x,y,z,GetPlayerInterior(playerid),GetPlayerVirtualWorld(playerid),veh);
    return 1;
}
public OnPlayerStartTalking(playerid) { VoiceUI_OnStartTalking(playerid); return 1; }
public OnPlayerStopTalking(playerid)  { VoiceUI_OnStopTalking(playerid);  return 1; }
```

## In-game controls (client)

- **B** — talk on proximity (PTT). **N** — talk on radio/phone (PTT).
- **F9** — self-mute. **F8** — toggle the "who's talking" HUD.
- **INSERT** — open the settings panel (volumes, devices, PTT/VAD mode, key rebind,
  noise suppression, per-player local mute/volume).

## Test filterscript

[`examples/voicechat_test.pwn`](examples/voicechat_test.pwn) exposes one chat command per
native — drop it in as a side-script and type `/vhelp`:

| Command | Tests |
|---------|-------|
| `/vrange whisper\|normal\|shout`, `/vrangeu <u>` | proximity range |
| `/vradio`, `/vphone`, `/vinchan` | bus membership + queries |
| `/vspeaker` | J6 speakerphone toggle |
| `/vfilter`, `/vprio`, `/vchans` | live channel tweaks + queries |
| `/vmute`, `/vdeafen`, `/valive`, `/vspec` | player flags |
| `/vmix mix\|duck\|exclusive` | mix policy |
| `/vpmute <id>`, `/vpunmute <id>` | per-pair mute |
| `/vreload`, `/vstatus` | config reload + state dump |

## Status

Core + all major features are implemented and validated in-game (2-player proximity with
occlusion + falloff, radio/phone, speakerphone, RNNoise, the ImGui panel). Remaining:
**AEC** (acoustic echo cancellation — deferred; headphones avoid echo) and a stale
loopback test (`e2e_handshake`, excluded from CI).
