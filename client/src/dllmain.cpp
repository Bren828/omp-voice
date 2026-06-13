// ============================================================
//  VoiceChat client (.asi) — entry point / orchestration
//  File: client/src/dllmain.cpp
//
//  Injected into GTA SA. On attach it spins a worker thread that:
//   1. loads voicechat.ini (local prefs),
//   2. starts WASAPI playback + capture (Opus),
//   3. starts the secure VoiceClient (token handshake + AEAD + reconnect),
//   4. wires relay->playback and capture->relay,
//   5. polls the PTT / mute keys.
//
//  Identity is the token delivered by the .dll (TokenInbox) — no SA-MP memory
//  reading. If the relay is down the game runs fine, just no voice (req. J7).
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>     // CoInitializeEx (excluded by WIN32_LEAN_AND_MEAN)
#include <thread>
#include <atomic>
#include <cstdio>

#include "../include/client_config.h"
#include "../include/voice_client.h"
#include "../include/audio_capture.h"
#include "../include/audio_playback.h"
#include "../include/overlay.h"
#include "../../shared/crypto.h"
#ifdef VC_CLIENT_GUI
#  include "../include/voice_panel.h"
#  include "../include/d3d9_overlay.h"
#  include "../include/audio_devices.h"
#endif

using namespace vc;
using namespace vc::client;

static std::atomic<bool> g_run{ false };
static HANDLE        g_thread = nullptr;
static VoiceClient   g_voice;
static AudioCapture  g_capture;
static AudioPlayback g_playback;
static bool          g_muted = false;
static ClientConfig  g_cfg;          // live config (panel edits it; save persists)

#ifdef VC_CLIENT_GUI
// Panel slider (0..1) -> capture RMS gate (~0..0.1) and back.
static constexpr float VAD_SCALE = 0.1f;

// Apply a saved device preference (by friendly name) at startup, if present.
static void applySavedDevice(bool capture, const std::string& name)
{
    if (name.empty()) return;
    for (auto& d : enumerateDevices(capture))
        if (d.name == name) {
            if (capture) g_capture.restartOnDevice(d.id);
            else         g_playback.restartOnDevice(d.id);
            return;
        }
}

static void wirePanel()
{
    panel::Backend be;
    be.setMaster = [](float v){ g_cfg.masterVolume = v; g_playback.setMasterVolume(v); };
    be.setMic    = [](float v){ g_cfg.micVolume = v;    g_capture.setMicVolume(v); };
    be.setDeafen = [](bool d){  g_cfg.deafen = d;        g_playback.setDeafen(d); };
    be.setMode   = [](int m){   g_cfg.vadEnabled = (m == 1); g_capture.setVad(m == 1); };
    be.setVadSensitivity = [](float s){   // higher slider = more sensitive = lower gate
        g_cfg.vadThreshold = (1.0f - s) * VAD_SCALE; g_capture.setVadThreshold(g_cfg.vadThreshold); };
    be.setPttKey = [](int vk){ g_cfg.pttKey = vk; };
    be.setNoiseSuppress = [](bool on){ g_cfg.noiseSuppress = on; g_capture.setNoiseSuppress(on); };

    be.inputDevices  = []{
        std::vector<panel::DeviceItem> v;
        for (auto& d : enumerateDevices(true))  v.push_back({ d.id, d.name });
        return v; };
    be.outputDevices = []{
        std::vector<panel::DeviceItem> v;
        for (auto& d : enumerateDevices(false)) v.push_back({ d.id, d.name });
        return v; };
    be.selectInput  = [](const panel::DeviceItem& d){ g_cfg.inputDevice  = d.name; g_capture.restartOnDevice(d.id); };
    be.selectOutput = [](const panel::DeviceItem& d){ g_cfg.outputDevice = d.name; g_playback.restartOnDevice(d.id); };
    be.currentInputName  = g_cfg.inputDevice;
    be.currentOutputName = g_cfg.outputDevice;

    be.audiblePlayers = []{
        std::vector<panel::PlayerItem> v;
        for (uint16_t id : g_playback.activeSpeakers()) {
            char nm[24]; snprintf(nm, sizeof(nm), "Player %u", (unsigned)id);
            v.push_back({ id, nm });
        }
        return v; };
    be.isMuted = [](uint16_t id){ return g_playback.isSpeakerMuted(id); };
    be.setMute = [](uint16_t id, bool m){ g_playback.setSpeakerMute(id, m); };
    be.getVol  = [](uint16_t id){ return g_playback.getSpeakerVolume(id); };
    be.setVol  = [](uint16_t id, float v){ g_playback.setSpeakerVolume(id, v); };

    be.serverAllowsModeSwitch = []{ return g_voice.limits().vadAllowed; };
    be.save = []{ ClientConfig::save(g_cfg, "voicechat.ini"); };

    panel::VoiceSettings init;
    init.master = g_cfg.masterVolume;
    init.mic    = g_cfg.micVolume;
    init.mode   = g_cfg.vadEnabled ? 1 : 0;
    init.vadSensitivity = 1.0f - std::min(1.0f, std::max(0.0f, g_cfg.vadThreshold / VAD_SCALE));
    init.deafen = g_cfg.deafen;
    init.pttKey = g_cfg.pttKey;
    init.noiseSuppress = g_cfg.noiseSuppress;
    panel::init(be, init);
}
#endif // VC_CLIENT_GUI

// Redirect printf to a log next to the .asi (clients have no console).
static void setupLog(HMODULE mod)
{
    char path[MAX_PATH];
    GetModuleFileNameA(mod, path, MAX_PATH);
    if (char* p = strrchr(path, '\\'))
        strcpy_s(p + 1, MAX_PATH - (p + 1 - path), "VoiceChat.log");
    FILE* f = nullptr;
    if (freopen_s(&f, path, "a", stdout) == 0 && f) setvbuf(f, nullptr, _IONBF, 0);
    freopen_s(&f, path, "a", stderr);
}

static void run()
{
    Sleep(3000);                         // let GTA SA finish loading
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    crypto::init();

    g_cfg = ClientConfig::load("voicechat.ini");
    printf("[VoiceChat v2] relay=%s:%d cmd=%d\n", g_cfg.relayIp.c_str(), g_cfg.audioPort, g_cfg.cmdPort);

    g_playback.init(g_cfg.masterVolume, g_cfg.proximityVolume, g_cfg.radioVolume);
    g_playback.setDeafen(g_cfg.deafen);

    // On-screen "who's talking" overlay (stage G2). Optional; if the window
    // can't be created the rest of the client runs unaffected (req. J7).
    overlay::OverlayCfg ocfg;
    ocfg.show   = g_cfg.showOverlay;
    ocfg.anchor = (g_cfg.overlayAnchor == "topleft") ? overlay::OverlayCfg::TopLeft
                                                     : overlay::OverlayCfg::RadarLeft;
    ocfg.x = g_cfg.overlayX; ocfg.y = g_cfg.overlayY;
#ifdef VC_CLIENT_GUI
    // ImGui build: the D3D9 HUD draws the lines, so skip the GDI window entirely
    // (a second copy at the same spot doubled the text above the radar). init()
    // still runs so g_visible + the talker/status data feed the ImGui HUD.
    ocfg.ownWindow = false;
#endif
    // Always init so overlay visibility (g_visible) is authoritative for the HUD
    // (show=false just starts hidden, still toggleable with the overlay key).
    overlay::init(ocfg);

    // relay -> local playback (hybrid: client spatialises proximity, plays buses).
    // A decoded frame arriving = that speaker/channel is talking -> feed overlay.
    PlaybackHooks hooks;
    hooks.onProximity = [](uint16_t sp, uint16_t seq, uint8_t fl, float vol, float pan,
                           const uint8_t* op, uint16_t len) {
        overlay::noteSpeaker(sp, /*isBus=*/false);
        g_playback.pushProximity(sp, seq, fl, vol, pan, op, len); };
    hooks.onBus = [](uint16_t ch, uint8_t filt, uint16_t seq, float vol,
                     const uint8_t* op, uint16_t len) {
        overlay::noteSpeaker(ch, /*isBus=*/true);
        g_playback.pushBus(ch, filt, seq, vol, op, len); };
    hooks.onGone = [](uint16_t sp) {
        overlay::removeSpeaker(sp);
        g_playback.removeSpeaker(sp); };
    g_voice.setHooks(hooks);

    // mic -> relay (sealed AudioUp)
    g_capture.setFrameSink([](uint16_t seq, uint8_t fl, const uint8_t* op, uint16_t len) {
        g_voice.sendAudio(seq, fl, op, len); });
    g_capture.init(g_cfg.micVolume, g_cfg.vadEnabled, g_cfg.vadThreshold, g_cfg.noiseSuppress);

#ifdef VC_CLIENT_GUI
    // Apply saved device prefs (if the named endpoints still exist) + wire the
    // ImGui control panel. The D3D9 hook is installed lazily in the loop below
    // once the game's Direct3D device exists.
    applySavedDevice(true,  g_cfg.inputDevice);
    applySavedDevice(false, g_cfg.outputDevice);
    wirePanel();
#endif

    g_voice.start(g_cfg.relayIp.c_str(), (uint16_t)g_cfg.audioPort, (uint16_t)g_cfg.cmdPort);
    printf("[VoiceChat v2] running. PTT keys: %d / %d\n", g_cfg.pttKey, g_cfg.pttKey2);

    bool lastMute = false, lastToggle = false, lastPanel = false;
    int  guiTry = 0;
    while (g_run) {
#ifdef VC_CLIENT_GUI
        // Retry installing the D3D9 hook (~once/sec) until the device is up.
        if (!d3d9::installed() && (guiTry++ % 60) == 0) d3d9::install();

        // Panel toggle (edge). Polled here so it's independent of who consumes
        // window messages (SA-MP grabs several F-keys; default is INSERT).
        bool panelNow = g_cfg.panelKey && (GetAsyncKeyState(g_cfg.panelKey) & 0x8000);
        if (panelNow && !lastPanel && d3d9::installed()) d3d9::togglePanel();
        lastPanel = panelNow;

        // PTT rebind: capture the next key the user presses. Polled here (not in
        // WndProc) because WM_KEYDOWN delivery is unreliable in fullscreen — the
        // same reason every other key uses GetAsyncKeyState. ESC cancels; the
        // panel key (INSERT) is skipped so it can still close the panel.
        if (panel::pttRebindActive()) {
            for (int vk = 0x08; vk <= 0xFE; ++vk) {
                if (vk == VK_LBUTTON  || vk == VK_RBUTTON  || vk == VK_MBUTTON ||
                    vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || vk == g_cfg.panelKey)
                    continue;
                if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
                if (vk == VK_ESCAPE) panel::cancelPttRebind();
                else                 panel::applyPttRebind(vk);
                break;
            }
        }
#endif
        bool ptt = (g_cfg.pttKey  && (GetAsyncKeyState(g_cfg.pttKey)  & 0x8000)) ||
                   (g_cfg.pttKey2 && (GetAsyncKeyState(g_cfg.pttKey2) & 0x8000));
        g_capture.setTransmit(ptt);

        bool muteNow = g_cfg.muteKey && (GetAsyncKeyState(g_cfg.muteKey) & 0x8000);
        if (muteNow && !lastMute) {
            g_muted = !g_muted; g_capture.setMuted(g_muted);
            printf("[VoiceChat] %s\n", g_muted ? "MUTED" : "UNMUTED");
        }
        lastMute = muteNow;

        // Overlay: toggle key (edge), latest status, repaint/message pump.
        bool toggleNow = g_cfg.overlayKey && (GetAsyncKeyState(g_cfg.overlayKey) & 0x8000);
        if (toggleNow && !lastToggle) overlay::toggle();
        lastToggle = toggleNow;
        overlay::setStatus(g_voice.connected(), ptt, g_muted);
        overlay::pump();

        Sleep(15);
    }
}

// Single-instance guard. ASI loaders scan BOTH the game root and scripts\ (and
// launchers may inject their own mods\asi too), so a stray duplicate copy of
// this .asi gets loaded into the SAME process — two ImGui panels, two HUDs,
// two relay sessions fighting over the mic. First instance wins; any duplicate
// refuses to load (returning FALSE unloads it).
static HANDLE g_instanceMutex = nullptr;

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_instanceMutex = CreateMutexA(nullptr, TRUE, "Local\\VoiceChatV2_ASI");
        if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(g_instanceMutex);
            g_instanceMutex = nullptr;
            return FALSE;              // duplicate copy — refuse to load
        }
        DisableThreadLibraryCalls(mod);
        setupLog(mod);
        printf("[VoiceChat v2] DllMain attach\n");
        g_run = true;
        g_thread = CreateThread(nullptr, 0,
            [](LPVOID) -> DWORD { run(); return 0; }, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        // A duplicate instance bounced in ATTACH (returned FALSE) — Windows
        // still sends DETACH; nothing was initialised, so don't touch anything.
        if (!g_instanceMutex) return TRUE;
        g_run = false;
#ifdef VC_CLIENT_GUI
        d3d9::shutdown();          // unhook EndScene/Reset + restore WndProc first
#endif
        g_capture.shutdown();
        g_voice.stop();
        g_playback.shutdown();
        overlay::shutdown();
        if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); }
        if (g_instanceMutex) { CloseHandle(g_instanceMutex); g_instanceMutex = nullptr; }
    }
    return TRUE;
}
