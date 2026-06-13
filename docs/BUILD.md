# Build & install

## Prerequisites
- CMake ≥ 3.16, a C++17 compiler.
- **libsodium** for production (`-DVC_NO_SODIUM=OFF`). vcpkg:
  `vcpkg install libsodium`; Debian/Ubuntu: `apt install libsodium-dev`.
- **libopus** for stage E relay bus mixing (`-DVC_WITH_OPUS=ON`).
- open.mp SDK in `omp/sdk` for the omp component (`-DVC_WITH_OMP_SDK=ON`).

> The skeleton defaults to `-DVC_NO_SODIUM=ON` (encryption disabled,
> pass-through) so it configures and builds with **no external deps**. Turn
> it OFF before shipping.

## Bitness (important)
| target | bitness | why |
|--------|---------|-----|
| relay  | x64 ok  | standalone process |
| samp plugin | **x86** | SA-MP 0.3.7 server is 32-bit |
| omp component | **x64** | open.mp is 64-bit |
| client .asi | **x86** | GTA SA is 32-bit |

Build the pieces for one bitness per configure dir.

## Server side — SA-MP plugin + relay (Windows, x86)
```powershell
cmake -B build-samp -A Win32 -DVC_BUILD_SAMP=ON -DVC_BUILD_RELAY=ON -DVC_NO_SODIUM=OFF
cmake --build build-samp --config Release
# -> build-samp/samp/Release/VoiceChat.dll   (place in server  plugins/)
# -> build-samp/relay/Release/voice_relay.exe (place in server components/ or plugins/)
```

## Server side — SA-MP plugin (Linux, 32-bit)
```bash
cmake -B build-samp -DVC_BUILD_SAMP=ON -DVC_BUILD_RELAY=ON -DVC_NO_SODIUM=OFF
cmake --build build-samp -j
# -> VoiceChat.so (plugins/), voice_relay (components/)   [standard, non-static]
```

## Server side — open.mp component (x64)
```bash
git clone https://github.com/openmultiplayer/open.mp-sdk omp/sdk
cmake -B build-omp -DVC_BUILD_OMP=ON -DVC_BUILD_RELAY=ON -DVC_WITH_OMP_SDK=ON -DVC_NO_SODIUM=OFF
cmake --build build-omp
# -> VoiceChat.dll/.so  ->  server components/
```

## Client (.asi, Windows x86)
```powershell
cmake -B build-asi -A Win32 -DVC_BUILD_CLIENT=ON -DVC_BUILD_RELAY=OFF -DVC_BUILD_SAMP=OFF -DVC_NO_SODIUM=OFF
cmake --build build-asi --config Release
# -> VoiceChat.asi  -> GTA SA root (next to gta_sa.exe)
```

## Install layout
```
server/
  plugins/        VoiceChat.dll|.so      (SA-MP)        + voice_relay(.exe)
  components/     VoiceChat.dll|.so      (open.mp)      + voice_relay(.exe)
  voice.ini                              (authoritative server config)
  gamemodes/  ... #include <voicechat>
GTA SA/
  VoiceChat.asi
  voicechat.ini                          (client-local prefs only)
```

## Skeleton smoke build (no deps)
```bash
cmake -B build -DVC_NO_SODIUM=ON -DVC_BUILD_OMP=ON
cmake --build build
```
