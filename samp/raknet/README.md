# SA-MP RakNet token delivery (opt-in hardening)

**Status:** scaffold + design. The acquisition is wired (behind `-DVC_SAMP_RAKNET=ON`);
the `Send` call and the client receive hook are the remaining on-target work.
Until then the plugin delivers the token over the **cmd-UDP** channel
(`:7780`), which works and is the default — see `docs/SECURITY.md`.

## Why
Stage A made identity a per-connect crypto **token**, single-use + TTL + IP-bound.
The token still has to reach the client over a channel an attacker can't read.
The default cmd-UDP path is fine for most RP servers (a sniffed token can't be
redeemed from another host), but an on-path attacker who can *both* read the token
*and* spoof the client's source IP could still redeem it. Riding the token inside
the game's **already-encrypted RakNet session** closes that gap and removes the
side channel entirely. open.mp does this cleanly via its public network API
(`omp/src/component.cpp`); SA-MP 0.3.7 R5 has no public API for it, so it needs a hook.

## Wire format
A custom RakNet packet, same payload the cmd-UDP path uses:

```
[ uint8 ID_VOICE_TOKEN ][ uint16 playerId ][ uint8 token[32] ]
```

Pick `ID_VOICE_TOKEN` from the unused custom-id range (e.g. `220`); it must not
collide with a SA-MP packet id for the target build.

## Server side (`samp/raknet/`)
1. **Acquire the interface.** SA-MP passes it to the plugin in `Load`:
   ```cpp
   auto getRak = reinterpret_cast<void* (*)()>(ppData[PLUGIN_DATA_RAKSERVER]);
   RakServerInterface* rak = (RakServerInterface*)getRak();
   ```
   This is already done (logged) when built with `VC_SAMP_RAKNET`.
2. **Send the token** from `deliverToken(pid, tok)`:
   ```cpp
   BitStream bs;
   bs.Write((uint8_t)ID_VOICE_TOKEN);
   bs.Write((uint16_t)pid);
   bs.Write((const char*)tok.data(), TOKEN_BYTES);
   rak->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0,
             rak->GetPlayerIDFromIndex(pid), false);
   ```
3. **`RakServerInterface` ABI.** `Send` / `GetPlayerIDFromIndex` are at fixed
   vtable slots for 0.3.7 R5. Do **not** hand-roll the vtable from memory — copy
   the verified `RakServer.h` / `BitStream.h` from a known-good reference
   (YSF, sampgdk, or the SA-MP RakNet plugin) into this folder and verify the
   slot order against your server binary before enabling. A wrong slot calls the
   wrong virtual and crashes the server.

## Client side (`client/raknet/`, in the `.asi`)
The `.asi` already hooks the client (MinHook). Add an interception on the RakNet
receive path for `ID_VOICE_TOKEN`: read `playerId` + `token` and feed them to
`TokenInbox` (same struct the cmd-UDP listener fills), then **swallow** the packet
so the game never sees it. With both halves in place, set
`g_link.setTokenDeliver(deliverTokenRak)` in `Load` and the cmd-UDP listener can
be retired.

## Enabling
```
cmake -B build -DVC_BUILD_SAMP=ON -DVC_SAMP_RAKNET=ON ...
```
Leave it **OFF** until the `Send` path and the client hook are implemented and
verified on your target build — otherwise tokens won't be delivered (or worse,
a mismatched vtable will crash the server).
