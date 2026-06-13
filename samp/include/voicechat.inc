// ============================================================
//  VoiceChat — Pawn include (open.mp + SA-MP 0.3.7 R5)
//  File: voicechat.inc   (v2 — generic channel API)
//
//  The plugin exposes ONE primitive: the channel. Gameplay (phone, taxi,
//  faction radio) is built in Pawn on top of channels + filters + members
//  (req. I). Nothing here is opinionated about gameplay.
// ============================================================
#if defined _voicechat_included
    #endinput
#endif
#define _voicechat_included

// ── Channel filters (per channel, req. I2) ──────────────────
#define VOICE_FILTER_NONE   0
#define VOICE_FILTER_RADIO  1
#define VOICE_FILTER_PHONE  2

// ── Proximity range presets (req. D2) ───────────────────────
#define VOICE_RANGE_WHISPER 0   // ~8u
#define VOICE_RANGE_NORMAL  1   // ~18u
#define VOICE_RANGE_SHOUT   2   // ~40u

// ── Mix policy (req. I4) ────────────────────────────────────
#define VOICE_MIX           0   // all buses equal
#define VOICE_DUCK          1   // duck low-priority (recommended default)
#define VOICE_EXCLUSIVE     2   // only highest priority audible

// ── Vehicle/occlusion state (req. J4) ───────────────────────
#define VOICE_ONFOOT        0
#define VOICE_VEH_OPEN      1   // bike/convertible: no occlusion
#define VOICE_VEH_CLOSED    2   // muffled in/out

// ============================================================
//  Natives — channel primitive (req. I1)
// ============================================================
native Voice_CreateChannel(filter, bool:positional, priority = 100);
native Voice_DestroyChannel(channelid);
native Voice_AddToChannel(playerid, channelid);
native Voice_RemoveFromChannel(playerid, channelid);
native Voice_IsInChannel(playerid, channelid);
native Voice_SetChannelFilter(channelid, filter);
native Voice_SetChannelPriority(channelid, priority);

// ── Channel queries (req. I, read-only; answered from the bridge mirror) ──
native bool:Voice_IsValidChannel(channelid);
native Voice_GetChannelCount();                 // how many channels exist
native Voice_GetChannelMemberCount(channelid);  // members in one channel
// Fill `players[]` with the member ids; returns how many were written.
native Voice_GetChannelPlayers(channelid, players[], maxplayers = sizeof players);

// ============================================================
//  Natives — player control (req. G1, D2, J)
// ============================================================
// interior/vworld/vehState are AUTHORITATIVE here — the server reads them,
// the client never reports position (req. J10 anti-spoof).
native Voice_UpdatePosition(playerid, Float:x, Float:y, Float:z, interior, vworld, vehState);
native Voice_SetPlayerRange(playerid, preset, Float:override_units = 0.0);
native Voice_SetPlayerFlags(playerid, bool:muted, bool:deafen, bool:alive, bool:spectator);
native Voice_SetTransmitting(playerid, bool:on, channelMask = 0);
native Voice_SetMixPolicy(playerid, policy, Float:duckLevel = 0.5, Float:proximityFloor = 0.25);
native Voice_MutePlayer(playerid, targetid, bool:mute = true);   // per-pair mute (J11)
// Speakerphone (J6): broadcast the call audio this player hears into their
// proximity so bystanders hear it "on speaker". Server-wide gate: voice.ini
// [behavior] speakerphone. Turn off with bool:on = false.
native Voice_SetSpeakerphone(playerid, bool:on = true);

// ============================================================
//  Natives — lifecycle (req. A, J8)
// ============================================================
// Pass the player's IP (GetPlayerIp) so the token is delivered to that host
// and the relay's IP secondary check works.
native Voice_OnPlayerConnect(playerid, const ip[]);  // mint + deliver token + bind
native Voice_OnPlayerDisconnect(playerid);           // full cleanup
native Voice_RegisterPlayerIp(playerid, const ip[]); // optional/legacy override

// ============================================================
//  Natives — server config (req. H)
// ============================================================
// Re-read voice.ini [policy]/[audio]/[proximity] and push it to the relay so
// edits take effect without a restart. Auto-pushed once at startup; call this
// after editing voice.ini at runtime.
native Voice_ReloadConfig();

// ============================================================
//  Callbacks — fired BY the plugin INTO the gamemode (req. G1)
//  (relay -> .dll status channel drives these; wired in stage B/G)
// ============================================================
forward OnPlayerStartTalking(playerid);
forward OnPlayerStopTalking(playerid);
forward OnPlayerRadioKey(playerid, channelid, bool:down);

// ── Convenience: proximity channel every player joins ───────
stock Voice_SetupProximity()
{
    // filter NONE, positional = true, low priority floor.
    new chan = Voice_CreateChannel(VOICE_FILTER_NONE, true, 1);
    printf("[VoiceChat] proximity channel = %d", chan);
    return chan;
}
