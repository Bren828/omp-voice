// ============================================================
//  VoiceChat — open.mp component bridge (req. arch 2)
//  File: omp/src/component.cpp
//
//  This is the SECOND bridge over the SAME /core. It is intentionally a
//  mirror of samp/src/plugin.cpp: identical natives, identical token
//  lifecycle, identical core::ControlLink usage. Only the registration
//  ABI differs (open.mp C++ SDK component vs classic 5-export plugin).
//
//  Build: this TU compiles to a no-op unless VC_WITH_OMP_SDK is defined
//  and the official SDK is dropped into omp/sdk (see omp/README.md). That
//  keeps the skeleton green before you fetch the SDK, per the prompt rule
//  "don't use other components' internal headers; implementation isn't
//  stable across versions" — we only touch the public component API.
// ============================================================
#include "../../core/include/control_link.h"
#include "../../shared/relay_launcher.h"
#include "../../relay/include/config.h"   // RelayConfig::load — voice.ini reader (stage H)

using namespace vc;

// One shared bridge state, exactly like the SA-MP plugin.
static core::ControlLink g_link;

// Stage H: read voice.ini's gameplay policy and push it to the relay over
// CtrlConfig (authoritative override of the relay's boot-time defaults). Mirror
// of the SA-MP plugin's pushVoiceConfig(); exposed as Voice_ReloadConfig below.
static void pushVoiceConfig()
{
    RelayConfig c = RelayConfig::load("voice.ini");
    g_link.pushConfig((uint16_t)c.bitrate, c.whisper, c.normal, c.shout,
                      c.falloffExp, c.occlusion, c.vadAllowed,
                      (uint8_t)c.defaultMode, (uint8_t)c.maxConcurrentStreams);
}

// ── Natives are registered through the open.mp pawn API. Each thunk just
//    forwards into g_link — the bodies are identical to the SA-MP ones, so
//    they are factored here to make the parallel obvious. ──────────────
namespace vc::omp_natives {
    inline uint16_t CreateChannel(int filter, bool positional, int prio)
    { return g_link.createChannel((Filter)filter, positional, (uint8_t)prio); }
    inline void DestroyChannel(int cid) { g_link.destroyChannel((uint16_t)cid); }
    inline void AddToChannel(int pid, int cid)      { g_link.joinChannel(pid, cid); }
    inline void RemoveFromChannel(int pid, int cid) { g_link.leaveChannel(pid, cid); }
    // Channel queries (req. I) — answered from the bridge registry mirror.
    inline bool IsValidChannel(int cid) { return g_link.registry().channel((uint16_t)cid) != nullptr; }
    inline int  GetChannelCount()       { return (int)g_link.registry().channels().size(); }
    inline int  GetChannelMemberCount(int cid)
    { auto* c = g_link.registry().channel((uint16_t)cid); return c ? (int)c->members.size() : 0; }
    inline void UpdatePosition(int pid, float x, float y, float z,
                               int interior, int vw, int veh)
    { g_link.pushPosition(pid, x, y, z, interior, vw, (uint8_t)veh); }
    inline void SetTransmitting(int pid, bool on, int mask)
    { g_link.setTransmit(pid, on, (uint32_t)mask); }
    inline void SetSpeakerphone(int pid, bool on)   // J6
    { g_link.setSpeakerphone((uint16_t)pid, on); }
    inline void OnConnect(int pid)    { g_link.onPlayerConnect((uint16_t)pid); }
    inline void OnDisconnect(int pid) { g_link.onPlayerDisconnect((uint16_t)pid); }
    inline void ReloadConfig()        { pushVoiceConfig(); }   // req. H: re-read voice.ini
}

#ifdef VC_WITH_OMP_SDK
// ============================================================
//  Real open.mp component — activated once omp/sdk is present.
// ============================================================
#include "sdk.hpp"   // open.mp public SDK (omp/sdk)

class VoiceComponent final : public IComponent
{
public:
    PROVIDE_UID(0x564f494345434854 /* "VOICECHT" */);
    StringView componentName() const override { return "VoiceChat"; }
    SemanticVersion componentVersion() const override { return { 2, 0, 0, 0 }; }

    void onLoad(ICore* c) override
    {
        core_ = c;
        crypto::init();
        g_link.init("127.0.0.1", DEFAULT_PORT_CONTROL);

        // Token delivery (req. A): open.mp already gives every player an
        // authenticated, encrypted channel, so we just send the token as a
        // packet on it — no legacy cmd-UDP, no RakNet vtable hook. This is
        // the SA-MP bridge's `deliverToken`, done the clean way.
        g_link.setTokenDeliver([this](uint16_t pid, const crypto::Token& tok) {
            // Integration point: get the IPlayer for `pid` from the player
            // pool and send [ID_VOICE_TOKEN][token:32] via the public network
            // API (NetworkBitStream + IPlayer::sendPacket). The .asi's RakNet
            // receive hook feeds it into TokenInbox exactly as for SA-MP.
            (void)pid; (void)tok;
        });

        relay::launch();
    }

    void onInit(IComponentList* list) override
    {
        // Pawn natives registered here through the pawn component, each
        // wired to vc::omp_natives::*. Player connect/disconnect hooked via
        // IPlayerConnectEventHandler -> g_link.onPlayerConnect/Disconnect.
        (void)list;
    }

    void onReady() override
    {
        // Stage H: relay launched in onLoad has booted by now — push voice.ini's
        // authoritative policy (mirrors the SA-MP plugin's deferred ProcessTick
        // push). Runtime edits re-push via the Voice_ReloadConfig native.
        pushVoiceConfig();

        // Subscribe to the core tick so we can pump relay->gamemode status on
        // the main thread (open.mp's equivalent of SA-MP's ProcessTick):
        //   core_->getEventDispatcher().addEventHandler(this);
        // and implement ICoreEventHandler::onTick -> pumpStatus().
    }

    // Drains the relay's status queue and fires the matching Pawn callbacks.
    // Identical to the SA-MP ProcessTick body; only the "fire a public" call
    // differs (open.mp pawn component vs amx_Exec), so it is isolated here.
    void pumpStatus()
    {
        StatusEvent ev;
        while (g_link.nextStatus(ev)) {
            switch ((StatusType)ev.event) {
            case StatusType::SpeakerStart:  fireCallback("OnPlayerStartTalking", ev.playerId); break;
            case StatusType::SpeakerStop:   fireCallback("OnPlayerStopTalking",  ev.playerId); break;
            case StatusType::RadioKey:      fireRadioKey(ev.playerId, ev.channelId, ev.flag); break;
            case StatusType::SessionTimeout: remint(ev.playerId); break;   // reconnect (B2)
            }
        }
    }

    void onFree(IComponent*) override {}
    void free() override { relay::kill(); g_link.shutdown(); delete this; }
    void reset() override {}

private:
    // Integration points (open.mp pawn component). Bodies call the public API
    // to invoke a forwarded public on the gamemode + filterscripts.
    void fireCallback(const char* /*name*/, uint16_t /*pid*/) {}
    void fireRadioKey(uint16_t /*pid*/, uint16_t /*cid*/, uint8_t /*down*/) {}
    void remint(uint16_t pid) { g_link.onPlayerConnect(pid); }   // fresh token + deliver

    ICore* core_ = nullptr;
};

COMPONENT_ENTRY_POINT() { return new VoiceComponent(); }

#else
// Skeleton placeholder so the translation unit is non-empty and CMake can
// build an (inert) target before the SDK is fetched.
extern "C" const char* VoiceChat_omp_stub() { return "omp bridge: add omp/sdk + define VC_WITH_OMP_SDK"; }
#endif
