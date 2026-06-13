// ============================================================
//  VoiceChat client (.asi) — ImGui control panel (the "Voice - setari" menu)
//  File: client/include/voice_panel.h
//
//  The interactive settings panel drawn with Dear ImGui inside the D3D9
//  EndScene hook (see d3d9_overlay). It edits ONLY local prefs — volumes,
//  mic, deafen, PTT/VAD mode, PTT key, audio devices, and per-player local
//  mute/volume. Nothing here is authoritative: mode-switch is gated by what
//  the server allows (EffectiveLimits.vadAllowed), and per-player mute/volume
//  is pure client-side mixing.
//
//  The panel reaches the rest of the client ONLY through the Backend
//  callbacks (wired by dllmain) — it has no globals of its own beyond the UI
//  mirror state. d3d9_overlay owns open/close + input; it calls render() each
//  frame the panel is visible and feeds captured keys to applyPttRebind().
// ============================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vc::client::panel {

struct DeviceItem { std::wstring id; std::string name; };
struct PlayerItem { uint16_t id = 0; std::string name; };

// UI mirror of the editable settings (seeded from voicechat.ini at startup).
struct VoiceSettings {
    float master         = 1.0f;
    float mic            = 1.0f;
    int   mode           = 0;     // 0 = Push-to-Talk, 1 = Voice Activation
    float vadSensitivity = 0.2f;  // 0..1 panel slider
    bool  deafen         = false;
    int   pttKey         = 'B';   // VK code
    bool  noiseSuppress  = true;  // RNNoise mic denoise
};

// Everything the panel can do to the world. Wired by dllmain; the panel never
// touches g_playback/g_capture/cfg directly.
struct Backend {
    std::function<void(float)> setMaster;          // 0..1
    std::function<void(float)> setMic;             // 0..1
    std::function<void(bool)>  setDeafen;
    std::function<void(int)>   setMode;            // 0 PTT, 1 VAD
    std::function<void(float)> setVadSensitivity;  // 0..1
    std::function<void(int)>   setPttKey;          // VK code
    std::function<void(bool)>  setNoiseSuppress;   // RNNoise on/off

    std::function<std::vector<DeviceItem>()>  inputDevices;
    std::function<std::vector<DeviceItem>()>  outputDevices;
    std::function<void(const DeviceItem&)>    selectInput;
    std::function<void(const DeviceItem&)>    selectOutput;
    // Names of the currently-selected endpoints (to preselect the combos).
    std::string currentInputName;
    std::string currentOutputName;

    std::function<std::vector<PlayerItem>()>  audiblePlayers;
    std::function<bool(uint16_t)>             isMuted;
    std::function<void(uint16_t, bool)>       setMute;
    std::function<float(uint16_t)>            getVol;     // 0..2
    std::function<void(uint16_t, float)>      setVol;

    std::function<bool()> serverAllowsModeSwitch;
    std::function<void()> save;                    // persist cfg to voicechat.ini
};

// Wire the backend + seed the initial UI state. Call once from dllmain.
void init(const Backend& backend, const VoiceSettings& initial);

void onShow();                 // panel just opened: refresh device lists
void onHide();                 // panel just closed: persist settings
void render();                 // draw the window (inside EndScene, panel visible)

// PTT rebind handshake. The captured key is polled via GetAsyncKeyState in the
// dllmain loop (window messages are unreliable in fullscreen), not via WndProc.
void beginPttRebind();         // "Rebind PTT key" pressed
bool pttRebindActive();        // poller checks this each frame
void applyPttRebind(int vk);   // poller delivers the captured key
void cancelPttRebind();        // abort the capture (e.g. ESC) without changing the key

} // namespace vc::client::panel
