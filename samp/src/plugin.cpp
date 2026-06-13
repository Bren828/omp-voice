// ============================================================
//  VoiceChat — SA-MP 0.3.7 R5 bridge (classic plugin)
//  File: samp/src/plugin.cpp
//
//  Thin bridge: the 5 required exports + an AMX native table that simply
//  forwards into core::ControlLink. All the real logic lives in /core and
//  /relay; this file only knows about AMX and the SA-MP plugin ABI, so an
//  equivalent /omp component can sit on the SAME core unchanged.
//
//  Token delivery (req. A): a fresh crypto token is minted per connect and
//  must reach the client over an authenticated channel. SKELETON ships it
//  over the legacy cmd UDP channel if the gamemode registered the player's
//  IP; STAGE A upgrades this to a real RakNet packet (samp/raknet/).
// ============================================================
#include "sdk.hpp"
#include "../../core/include/control_link.h"
#include "../../shared/relay_launcher.h"
#include "../../relay/include/config.h"   // RelayConfig::load — voice.ini reader (stage H)

#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <chrono>

using namespace vc;

logprintf_t      logprintf = nullptr;
extern void*     pAMXFunctions;   // defined by the SDK's amxplugin.cpp

static core::ControlLink g_link;
static std::unordered_map<uint16_t, std::string> g_playerIp;   // for token delivery
static UdpSocket g_cmdSock;                                     // .dll -> .asi (legacy)
static std::vector<AMX*> g_amxList;                            // loaded scripts (callbacks)

// ── helpers ──────────────────────────────────────────────────
static float amxF(cell c) { return amx_ctof(c); }

static std::string amxStr(AMX* amx, cell param)
{
    cell* addr = nullptr; amx_GetAddr(amx, param, &addr);
    int len = 0; amx_StrLen(addr, &len);
    std::string s(len + 1, '\0');
    amx_GetString(&s[0], addr, 0, len + 1); s.resize(len);
    return s;
}

// STAGE A: replace with a RakNet outgoing packet for true authenticity.
static void deliverToken(uint16_t pid, const crypto::Token& tok)
{
    auto it = g_playerIp.find(pid);
    if (it == g_playerIp.end() || !g_cmdSock.valid()) {
        logprintf("[VoiceChat] token for %u minted (no IP yet, client will re-request)", pid);
        return;
    }
    // [ 'T','K' ][ playerId:2 ][ token:32 ]  -> client cmd port
    uint8_t buf[2 + 2 + TOKEN_BYTES];
    buf[0] = 'T'; buf[1] = 'K';
    std::memcpy(buf + 2, &pid, 2);
    std::memcpy(buf + 4, tok.data(), TOKEN_BYTES);
    auto to = UdpSocket::addr(it->second.c_str(), DEFAULT_PORT_CMD);
    g_cmdSock.sendTo(buf, sizeof(buf), to);
}

// ── Pawn callback dispatch (req. B / G1) ─────────────────────
// Fire a forwarded public into every loaded script. Pawn pushes arguments in
// REVERSE order (last parameter first), so the helpers below mirror that.
static void firePublic1(const char* name, cell a)
{
    for (AMX* amx : g_amxList) {
        int idx;
        if (amx_FindPublic(amx, name, &idx) != AMX_ERR_NONE) continue;
        amx_Push(amx, a);
        amx_Exec(amx, nullptr, idx);
    }
}
static void firePublic3(const char* name, cell a, cell b, cell c)
{
    for (AMX* amx : g_amxList) {
        int idx;
        if (amx_FindPublic(amx, name, &idx) != AMX_ERR_NONE) continue;
        amx_Push(amx, c);                     // 3rd param
        amx_Push(amx, b);                     // 2nd param
        amx_Push(amx, a);                     // 1st param
        amx_Exec(amx, nullptr, idx);
    }
}

// Session timed out on the relay (heartbeat lost) -> mint a FRESH token and
// deliver it so the client can re-handshake (req. B2 auto-reconnect). A new
// token supersedes the stale one in the relay's pending table.
static void remintToken(uint16_t pid)
{
    uint32_t ipn = 0;
    auto it = g_playerIp.find(pid);
    if (it != g_playerIp.end()) {
        unsigned long n = inet_addr(it->second.c_str());
        ipn = (n == INADDR_NONE) ? 0u : (uint32_t)n;
    }
    g_link.onPlayerConnect(pid, ipn);
}

// Stage H: read the authoritative gameplay config from voice.ini and push it to
// the relay over CtrlConfig. The relay also reads voice.ini at boot (pre-IPC
// default); this push makes the .dll the authoritative gameplay-config source
// and lets the gamemode re-apply edits at runtime via Voice_ReloadConfig().
static void pushVoiceConfig()
{
    RelayConfig c = RelayConfig::load("voice.ini");
    g_link.pushConfig((uint16_t)c.bitrate, c.whisper, c.normal, c.shout,
                      c.falloffExp, c.occlusion, c.vadAllowed,
                      (uint8_t)c.defaultMode, (uint8_t)c.maxConcurrentStreams);
    logprintf("[VoiceChat] config pushed from voice.ini "
              "(bitrate=%d vad=%d mode=%d maxstreams=%d)",
              c.bitrate, c.vadAllowed ? 1 : 0, c.defaultMode, c.maxConcurrentStreams);
}

// ============================================================
//  Natives  (signatures mirror samp/include/voicechat.inc)
// ============================================================

// --- channel primitive (req. I1) ---
static cell AMX_NATIVE_CALL n_CreateChannel(AMX*, cell* p)
{   // Voice_CreateChannel(filter, bool:positional, priority = 100)
    return g_link.createChannel((Filter)p[1], p[2] != 0, (uint8_t)p[3]);
}
static cell AMX_NATIVE_CALL n_DestroyChannel(AMX*, cell* p)
{   g_link.destroyChannel((uint16_t)p[1]); return 1; }
static cell AMX_NATIVE_CALL n_AddToChannel(AMX*, cell* p)
{   g_link.joinChannel((uint16_t)p[1], (uint16_t)p[2]); return 1; }
static cell AMX_NATIVE_CALL n_RemoveFromChannel(AMX*, cell* p)
{   g_link.leaveChannel((uint16_t)p[1], (uint16_t)p[2]); return 1; }
static cell AMX_NATIVE_CALL n_IsInChannel(AMX*, cell* p)
{   core::Channel* c = g_link.registry().channel((uint16_t)p[2]);
    return (c && c->members.count((uint16_t)p[1])) ? 1 : 0; }
static cell AMX_NATIVE_CALL n_SetChannelFilter(AMX*, cell* p)
{   g_link.setChannelFilter((uint16_t)p[1], (Filter)p[2]); return 1; }
static cell AMX_NATIVE_CALL n_SetChannelPriority(AMX*, cell* p)
{   g_link.setChannelPriority((uint16_t)p[1], (uint8_t)p[2]); return 1; }

// --- channel queries (req. I; answered from the bridge-side registry mirror) ---
static cell AMX_NATIVE_CALL n_IsValidChannel(AMX*, cell* p)
{   return g_link.registry().channel((uint16_t)p[1]) != nullptr ? 1 : 0; }
static cell AMX_NATIVE_CALL n_GetChannelCount(AMX*, cell*)
{   return (cell)g_link.registry().channels().size(); }
static cell AMX_NATIVE_CALL n_GetChannelMemberCount(AMX*, cell* p)
{   core::Channel* c = g_link.registry().channel((uint16_t)p[1]);
    return c ? (cell)c->members.size() : 0; }
static cell AMX_NATIVE_CALL n_GetChannelPlayers(AMX* amx, cell* p)
{   // Voice_GetChannelPlayers(channelid, players[], maxplayers) -> count written
    core::Channel* c = g_link.registry().channel((uint16_t)p[1]);
    if (!c) return 0;
    cell* arr = nullptr;
    if (amx_GetAddr(amx, p[2], &arr) != AMX_ERR_NONE || !arr) return 0;
    int maxn = (int)p[3], n = 0;
    for (uint16_t mid : c->members) { if (n >= maxn) break; arr[n++] = (cell)mid; }
    return n;
}

// --- player control (req. G1, D2) ---
static cell AMX_NATIVE_CALL n_UpdatePosition(AMX*, cell* p)
{   // Voice_UpdatePosition(playerid, Float:x,y,z, interior, vworld, vehState)
    g_link.pushPosition((uint16_t)p[1], amxF(p[2]), amxF(p[3]), amxF(p[4]),
                        (uint16_t)p[5], (uint16_t)p[6], (uint8_t)p[7]);
    return 1;
}
static cell AMX_NATIVE_CALL n_SetPlayerRange(AMX*, cell* p)
{   // Voice_SetPlayerRange(playerid, RangePreset:preset, Float:override=0.0)
    g_link.pushMeta((uint16_t)p[1], /*flags unchanged*/ 0x04 /*alive default*/,
                    (uint8_t)p[2], amxF(p[3]));
    return 1;
}
static cell AMX_NATIVE_CALL n_SetPlayerFlags(AMX*, cell* p)
{   // Voice_SetPlayerFlags(playerid, muted, deafen, alive, spectator)
    uint8_t flags = (p[2]?1:0)|(p[3]?2:0)|(p[4]?4:0)|(p[5]?8:0);
    g_link.pushMeta((uint16_t)p[1], flags, 1 /*Normal*/, -1.f);
    return 1;
}
static cell AMX_NATIVE_CALL n_SetTransmitting(AMX*, cell* p)
{   g_link.setTransmit((uint16_t)p[1], p[2] != 0, (uint32_t)p[3]); return 1; }
static cell AMX_NATIVE_CALL n_SetMixPolicy(AMX*, cell* p)
{   g_link.setMixPolicy((uint16_t)p[1], (MixPolicy)p[2], amxF(p[3]), amxF(p[4]));
    return 1; }

// Per-pair mute (stage G / J11): `playerid` stops hearing `targetid` by
// proximity, for themselves only. Optional 3rd arg toggles mute/unmute.
static cell AMX_NATIVE_CALL n_MutePlayer(AMX*, cell* p)
{   // Voice_MutePlayer(playerid, targetid, bool:mute = true)
    bool on = (p[0] >= (cell)(3 * sizeof(cell))) ? (p[3] != 0) : true;
    g_link.mutePlayer((uint16_t)p[1], (uint16_t)p[2], on);
    return 1;
}

// Speakerphone (J6): broadcast a player's received call audio into their
// proximity so nearby players hear it. Optional 2nd arg toggles on/off.
static cell AMX_NATIVE_CALL n_SetSpeakerphone(AMX*, cell* p)
{   // Voice_SetSpeakerphone(playerid, bool:on = true)
    bool on = (p[0] >= (cell)(2 * sizeof(cell))) ? (p[2] != 0) : true;
    g_link.setSpeakerphone((uint16_t)p[1], on);
    return 1;
}

// --- lifecycle (req. A, J8) ---
// Voice_OnPlayerConnect(playerid, const ip[]) — atomic mint + deliver + bind.
// The gamemode passes GetPlayerIp() so the token is delivered to that IP and
// the relay can do the IP secondary check.
static cell AMX_NATIVE_CALL n_OnPlayerConnect(AMX* amx, cell* p)
{
    uint16_t pid = (uint16_t)p[1];
    std::string ip = amxStr(amx, p[2]);
    g_playerIp[pid] = ip;                                  // for token delivery
    unsigned long ipn = inet_addr(ip.c_str());             // network order
    g_link.onPlayerConnect(pid, ipn == INADDR_NONE ? 0u : (uint32_t)ipn);
    return 1;
}
static cell AMX_NATIVE_CALL n_OnPlayerDisconnect(AMX*, cell* p)
{   uint16_t pid=(uint16_t)p[1]; g_link.onPlayerDisconnect(pid); g_playerIp.erase(pid); return 1; }
static cell AMX_NATIVE_CALL n_RegisterPlayerIp(AMX* amx, cell* p)
{   g_playerIp[(uint16_t)p[1]] = amxStr(amx, p[2]); return 1; }

// Stage H: re-read voice.ini and re-push it to the relay at runtime.
static cell AMX_NATIVE_CALL n_ReloadConfig(AMX*, cell*)
{   pushVoiceConfig(); return 1; }

static AMX_NATIVE_INFO kNatives[] = {
    { "Voice_CreateChannel",     n_CreateChannel },
    { "Voice_DestroyChannel",    n_DestroyChannel },
    { "Voice_AddToChannel",      n_AddToChannel },
    { "Voice_RemoveFromChannel", n_RemoveFromChannel },
    { "Voice_IsInChannel",       n_IsInChannel },
    { "Voice_SetChannelFilter",  n_SetChannelFilter },
    { "Voice_SetChannelPriority",n_SetChannelPriority },
    { "Voice_IsValidChannel",    n_IsValidChannel },
    { "Voice_GetChannelCount",   n_GetChannelCount },
    { "Voice_GetChannelMemberCount", n_GetChannelMemberCount },
    { "Voice_GetChannelPlayers", n_GetChannelPlayers },
    { "Voice_UpdatePosition",    n_UpdatePosition },
    { "Voice_SetPlayerRange",    n_SetPlayerRange },
    { "Voice_SetPlayerFlags",    n_SetPlayerFlags },
    { "Voice_SetTransmitting",   n_SetTransmitting },
    { "Voice_SetMixPolicy",      n_SetMixPolicy },
    { "Voice_MutePlayer",        n_MutePlayer },
    { "Voice_SetSpeakerphone",   n_SetSpeakerphone },
    { "Voice_OnPlayerConnect",   n_OnPlayerConnect },
    { "Voice_OnPlayerDisconnect",n_OnPlayerDisconnect },
    { "Voice_RegisterPlayerIp",  n_RegisterPlayerIp },
    { "Voice_ReloadConfig",      n_ReloadConfig },
    { nullptr, nullptr }
};

// ============================================================
//  Required SA-MP plugin exports
// ============================================================
PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports()
{
    // PROCESS_TICK: drives the relay->gamemode callback pump on the main thread.
    return SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void** ppData)
{
    pAMXFunctions = ppData[PLUGIN_DATA_AMX_EXPORTS];
    logprintf     = (logprintf_t)ppData[PLUGIN_DATA_LOGPRINTF];

    logprintf("========================================");
    logprintf("  VoiceChat (SA-MP bridge) v2.0 skeleton");
    logprintf("========================================");

    if (!crypto::init()) { logprintf("[VoiceChat] libsodium init FAILED"); return false; }

    g_cmdSock.open();
    if (!g_link.init("127.0.0.1", DEFAULT_PORT_CONTROL))
        logprintf("[VoiceChat] WARNING: control link to relay failed");
    g_link.setTokenDeliver(deliverToken);

    // Stage H: voice.ini's gameplay policy is pushed to the relay from the FIRST
    // ProcessTick after a short delay (see ProcessTick) — not here — so the relay
    // (launched just below, asynchronously) has had time to boot and bind before
    // the CtrlConfig UDP packet arrives. The relay's own boot read is the
    // fallback if the packet is ever lost.

#ifdef VC_SAMP_RAKNET
    // Opt-in hardening (req. A): SA-MP hands the plugin the RakServer interface,
    // so the token can ride the game's already-encrypted session instead of the
    // cmd-UDP side channel. We acquire it here; the Send call + client receive
    // hook are completed per samp/raknet/README.md. cmd-UDP stays the active
    // deliverer until that lands, so token delivery keeps working.
    using GetRakServerFn = void* (*)();
    auto getRak = reinterpret_cast<GetRakServerFn>(ppData[PLUGIN_DATA_RAKSERVER]);
    if (getRak && getRak())
        logprintf("[VoiceChat] RakNet interface acquired "
                  "(complete token delivery per samp/raknet/README.md)");
#endif

    relay::launch();   // watchdog auto-restart (B1)
    logprintf("[VoiceChat] loaded.");
    return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload()
{
    relay::kill();
    g_link.shutdown();
    g_cmdSock.close();
    logprintf("[VoiceChat] unloaded.");
}

// Pumped by the server on the main thread (~every server frame). Drains the
// relay's status queue and fires the matching Pawn callbacks (req. B / G1).
PLUGIN_EXPORT void PLUGIN_CALL ProcessTick()
{
    // Stage H one-shot: push voice.ini's policy once the relay has had time to
    // boot+bind (it's launched asynchronously in Load). steady_clock keeps this
    // portable (SA-MP also builds on Linux). Runtime edits use Voice_ReloadConfig.
    static const auto s_loadAt = std::chrono::steady_clock::now();
    static bool s_configPushed = false;
    if (!s_configPushed &&
        std::chrono::steady_clock::now() - s_loadAt > std::chrono::seconds(3)) {
        pushVoiceConfig();
        s_configPushed = true;
    }

    StatusEvent ev;
    while (g_link.nextStatus(ev)) {
        switch ((StatusType)ev.event) {
        case StatusType::SpeakerStart:
            firePublic1("OnPlayerStartTalking", (cell)ev.playerId); break;
        case StatusType::SpeakerStop:
            firePublic1("OnPlayerStopTalking",  (cell)ev.playerId); break;
        case StatusType::RadioKey:
            firePublic3("OnPlayerRadioKey", (cell)ev.playerId,
                        (cell)ev.channelId, (cell)ev.flag); break;
        case StatusType::SessionTimeout:
            remintToken(ev.playerId); break;     // drive client reconnect (B2)
        }
    }
}

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX* amx)
{
    g_amxList.push_back(amx);
    return amx_Register(amx, kNatives, -1);
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX* amx)
{
    g_amxList.erase(std::remove(g_amxList.begin(), g_amxList.end(), amx),
                    g_amxList.end());
    return AMX_ERR_NONE;
}
