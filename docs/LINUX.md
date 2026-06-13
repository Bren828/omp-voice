# Linux — build, install, run

The **relay** is a standalone x64 process and builds/runs cleanly on Linux (this is what
CI verifies on every push). The **SA-MP plugin** must be **32-bit** (the SA-MP 0.3.7
Linux server is 32-bit), and the **open.mp component** is x64 but needs the open.mp SDK.
The **client (`.asi`) is Windows-only** (GTA SA) and does not apply to Linux servers.

## What to install

Everything else (libsodium, opus) is fetched + built from source by CMake, so the only
prerequisites are a toolchain, CMake and git:

```bash
# Debian / Ubuntu
sudo apt update
sudo apt install -y build-essential cmake git
# for the 32-bit SA-MP plugin only:
sudo apt install -y gcc-multilib g++-multilib
```

- `build-essential` — gcc/g++ + make
- `cmake` ≥ 3.16
- `git` — CMake's `FetchContent` clones libsodium/opus at configure time
- `gcc-multilib g++-multilib` — only if you build the 32-bit SA-MP plugin

**Runtime dependencies: none.** libsodium and opus are linked statically, so the produced
`voice_relay` / `.so` have no extra package requirements — just run them.

## Build the relay (x64)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DVC_NO_SODIUM=OFF -DVC_WITH_OPUS=ON \
  -DVC_BUILD_RELAY=ON -DVC_BUILD_SAMP=OFF -DVC_BUILD_CLIENT=OFF -DVC_BUILD_TESTS=ON
cmake --build build -j
# -> build/relay/voice_relay
ctest --test-dir build --output-on-failure -E e2e_handshake   # optional
```

## Build the SA-MP plugin (32-bit `.so`)

The SA-MP server loads 32-bit plugins, so configure a 32-bit build (requires multilib):

```bash
cmake -S . -B build32 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 \
  -DVC_NO_SODIUM=OFF -DVC_WITH_OPUS=ON \
  -DVC_BUILD_SAMP=ON -DVC_BUILD_RELAY=OFF -DVC_BUILD_CLIENT=OFF -DVC_BUILD_TESTS=OFF
cmake --build build32 -j
# -> build32/samp/VoiceChat.so   ->  copy into the SA-MP server's plugins/
```

> Note: the relay is what CI builds on Linux; the 32-bit plugin build needs multilib and
> may want flag tweaks for your distro. The relay can also be the same x64 binary
> regardless of which (32-bit) SA-MP plugin loads it — they talk over UDP.

## Build the open.mp component (x64 `.so`)

Place the open.mp SDK in `omp/sdk/`, then:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DVC_NO_SODIUM=OFF -DVC_WITH_OPUS=ON \
  -DVC_BUILD_OMP=ON -DVC_WITH_OMP_SDK=ON \
  -DVC_BUILD_SAMP=OFF -DVC_BUILD_RELAY=ON -DVC_BUILD_CLIENT=OFF
cmake --build build -j
# -> copy the component .so into the open.mp server's components/
```

## Run

You normally **do not** start the relay by hand: the plugin/component launches and
supervises it (auto-restart with backoff). To run it standalone for testing:

```bash
cd /path/with/voice.ini
./voice_relay            # reads voice.ini from the current directory
```

Install layout on a Linux SA-MP/open.mp server:

```
samp03/                       openmp/
  plugins/VoiceChat.so          components/<voice>.so
  voice_relay                   components/voice_relay   (or alongside the server)
  pawno/include/voicechat.inc   qawno/include/voicechat.inc
  voice.ini                     voice.ini
```

Open inbound **UDP 7779** on the host — see [NETWORKING.md](NETWORKING.md).
