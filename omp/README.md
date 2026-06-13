# /omp — open.mp component bridge

Second bridge over the shared `/core`. It mirrors `samp/src/plugin.cpp`
one-to-one (same natives, same token lifecycle, same `core::ControlLink`);
only the registration ABI differs.

## Status
Skeleton. Compiles to an inert target until the official SDK is present and
`VC_WITH_OMP_SDK` is defined.

## Enabling the real component
1. Clone the public SDK (do **not** copy internal headers of other
   components — their layout is not stable across versions):
   ```
   git clone https://github.com/openmultiplayer/open.mp-sdk omp/sdk
   ```
2. Configure with the SDK on:
   ```
   cmake -B build -DVC_WITH_OMP_SDK=ON
   ```
3. The component implements `IComponent` (`onLoad`/`onInit`/`onReady`/`free`),
   registers the Pawn natives through the pawn component, and hooks
   `IPlayerConnectEventHandler` to call `g_link.onPlayerConnect()` /
   `onPlayerDisconnect()`.

## Delivery
Drop `VoiceChat.dll` / `VoiceChat.so` into the server's `components/` folder
(the SA-MP plugin instead goes into `plugins/`). Both talk to the same
standalone relay on `127.0.0.1:7778`.

## Token delivery
On open.mp, prefer a **RakNet packet** through the public network API for
token delivery (already authenticated per player) instead of the legacy
`.dll -> .asi` cmd UDP channel the SA-MP bridge falls back to.
