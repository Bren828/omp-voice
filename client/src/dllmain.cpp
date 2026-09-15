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
#include <objbase.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

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
static ClientConfig  g_cfg;

#ifdef VC_CLIENT_GUI
static constexpr float VAD_SCALE = 0.1f;

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
    be.setVadSensitivity = [](float s){
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
#endif

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

// Dump immutable information about the host executable. This lets us identify
// the exact GTA build (R1/R3/etc.) from VoiceChat.log without reading GTA memory.
static void logGameInfo()
{
    HMODULE game = GetModuleHandleA(nullptr);
    char path[MAX_PATH]{};
    GetModuleFileNameA(game, path, MAX_PATH);

    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(game);
    if (!game || !dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        printf("[gta] module probe failed (base=%p path=%s)\n", (void*)game, path);
        return;
    }

    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(game) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        printf("[gta] PE probe failed (base=%p path=%s)\n", (void*)game, path);
        return;
    }

    printf("[gta] exe=%s\n", path);
    printf("[gta] base=%p size=0x%08lX timestamp=0x%08lX checksum=0x%08lX\n",
           (void*)game,
           (unsigned long)nt->OptionalHeader.SizeOfImage,
           (unsigned long)nt->FileHeader.TimeDateStamp,
           (unsigned long)nt->OptionalHeader.CheckSum);
    printf("[gta] entry=0x%08lX machine=0x%04X sections=%u\n",
           (unsigned long)nt->OptionalHeader.AddressOfEntryPoint,
           (unsigned)nt->FileHeader.Machine,
           (unsigned)nt->FileHeader.NumberOfSections);
}

#ifdef VC_CLIENT_GUI
// d3d9::install() is the first code that actively hooks the GTA rendering/input
// path. Keep an SEH boundary around it so an access violation here is recorded
// in VoiceChat.log instead of leaving us with only a silent game crash.
static bool installD3D9WithDiagnostics()
{
    printf("[gui] ===== D3D9 install begin =====\n");
    bool result = false;
    __try {
        result = d3d9::install();
        printf("[gui] d3d9::install returned %s\n", result ? "SUCCESS" : "FAIL");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("[gui] !!! d3d9::install SEH exception: code=0x%08lX address=%p !!!\n",
               (unsigned long)GetExceptionCode(),
               GetExceptionInformation()->ExceptionRecord->ExceptionAddress);
        result = false;
    }
    printf("[gui] ===== D3D9 install end =====\n");
    return result;
}
#endif

static void run()
{
    Sleep(3000);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    crypto::init();

    printf("[VoiceChat v2] worker started\n");
    logGameInfo();

    g_cfg = ClientConfig::load("voicechat.ini");
    printf("[VoiceChat v2] relay=%s:%d cmd=%d\n", g_cfg.relayIp.c_str(), g_cfg.audioPort, g_cfg.cmdPort);

    printf("[audio] playback init begin\n");
    g_playback.init(g_cfg.masterVolume, g_cfg.proximityVolume, g_cfg.radioVolume);
    g_playback.setDeafen(g_cfg.deafen);
    printf("[audio] playback init done\n");

    overlay::OverlayCfg ocfg;
    ocfg.show   = g_cfg.showOverlay;
    ocfg.anchor = (g_cfg.overlayAnchor == "topleft") ? overlay::OverlayCfg::TopLeft
                                                     : overlay::OverlayCfg::RadarLeft;
    ocfg.x = g_cfg.overlayX; ocfg.y = g_cfg.overlayY;
#ifdef VC_CLIENT_GUI
    ocfg.ownWindow = false;
#endif
    printf("[overlay] init begin\n");
    overlay::init(ocfg);
    printf("[overlay] init done\n");

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

    g_capture.setFrameSink([](uint16_t seq, uint8_t fl, const uint8_t* op, uint16_t len) {
        g_voice.sendAudio(seq, fl, op, len); });
    printf("[audio] capture init begin\n");
    g_capture.init(g_cfg.micVolume, g_cfg.vadEnabled, g_cfg.vadThreshold, g_cfg.noiseSuppress);
    printf("[audio] capture init done\n");

#ifdef VC_CLIENT_GUI
    printf("[gui] applying saved audio devices\n");
    applySavedDevice(true,  g_cfg.inputDevice);
    applySavedDevice(false, g_cfg.outputDevice);
    printf("[gui] wiring panel\n");
    wirePanel();
    printf("[gui] panel wiring done\n");
#endif

    printf("[voice] relay start begin\n");
    g_voice.start(g_cfg.relayIp.c_str(), (uint16_t)g_cfg.audioPort, (uint16_t)g_cfg.cmdPort);
    printf("[voice] relay start done\n");
    printf("[VoiceChat v2] running. PTT keys: %d / %d\n", g_cfg.pttKey, g_cfg.pttKey2);

    bool lastMute = false, lastToggle = false, lastPanel = false;
    int guiTry = 0;
    while (g_run) {
#ifdef VC_CLIENT_GUI
        if (!d3d9::installed() && (guiTry++ % 60) == 0)
            installD3D9WithDiagnostics();

        bool panelNow = g_cfg.panelKey && (GetAsyncKeyState(g_cfg.panelKey) & 0x8000);
        if (panelNow && !lastPanel && d3d9::installed()) d3d9::togglePanel();
        lastPanel = panelNow;

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

        bool toggleNow = g_cfg.overlayKey && (GetAsyncKeyState(g_cfg.overlayKey) & 0x8000);
        if (toggleNow && !lastToggle) overlay::toggle();
        lastToggle = toggleNow;
        overlay::setStatus(g_voice.connected(), ptt, g_muted);
        overlay::pump();
        Sleep(15);
    }
}

static HANDLE g_instanceMutex = nullptr;

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_instanceMutex = CreateMutexA(nullptr, TRUE, "Local\\VoiceChatV2_ASI");
        if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(g_instanceMutex);
            g_instanceMutex = nullptr;
            return FALSE;
        }
        DisableThreadLibraryCalls(mod);
        setupLog(mod);
        printf("[VoiceChat v2] DllMain attach\n");
        g_run = true;
        g_thread = CreateThread(nullptr, 0,
            [](LPVOID) -> DWORD { run(); return 0; }, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!g_instanceMutex) return TRUE;
        g_run = false;
#ifdef VC_CLIENT_GUI
        d3d9::shutdown();
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
