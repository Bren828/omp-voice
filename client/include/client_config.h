// ============================================================
//  VoiceChat client (.asi) — LOCAL preferences (req. H2)
//  File: client/include/client_config.h
//
//  Reads voicechat.ini next to the GTA SA exe. This is NOT authoritative:
//  it holds ONLY local prefs (relay address, PTT keys, volumes, VAD). The
//  server hands down the authoritative limits at handshake; the client can
//  never widen range/falloff/allowed-mode (that lives in voice.ini).
//
//  Identity is the cryptographic token (delivered by the .dll), so there is
//  no player_id / name here any more — the old IP-based identity is gone.
// ============================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>

namespace vc::client {

inline int keyNameToVK(const std::string& nameIn)
{
    static const std::unordered_map<std::string, int> map = {
        {"A",'A'},{"B",'B'},{"C",'C'},{"D",'D'},{"E",'E'},{"F",'F'},{"G",'G'},
        {"H",'H'},{"I",'I'},{"J",'J'},{"K",'K'},{"L",'L'},{"M",'M'},{"N",'N'},
        {"O",'O'},{"P",'P'},{"Q",'Q'},{"R",'R'},{"S",'S'},{"T",'T'},{"U",'U'},
        {"V",'V'},{"W",'W'},{"X",'X'},{"Y",'Y'},{"Z",'Z'},
        {"0",'0'},{"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},{"5",'5'},{"6",'6'},
        {"7",'7'},{"8",'8'},{"9",'9'},
        {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},{"F5",VK_F5},
        {"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8},{"F9",VK_F9},{"F10",VK_F10},
        {"F11",VK_F11},{"F12",VK_F12},
        {"CAPSLOCK",VK_CAPITAL},{"LSHIFT",VK_LSHIFT},{"RSHIFT",VK_RSHIFT},
        {"LCTRL",VK_LCONTROL},{"RCTRL",VK_RCONTROL},{"LALT",VK_LMENU},
        {"RALT",VK_RMENU},{"SPACE",VK_SPACE},{"ENTER",VK_RETURN},{"TAB",VK_TAB},
        {"TILDE",VK_OEM_3},{"MOUSE4",VK_XBUTTON1},{"MOUSE5",VK_XBUTTON2},
        {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
        {"INSERT",VK_INSERT},{"DELETE",VK_DELETE},{"HOME",VK_HOME},{"END",VK_END},
        {"PAGEUP",VK_PRIOR},{"PAGEDOWN",VK_NEXT},
    };
    std::string s = nameIn;
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    auto it = map.find(s);
    if (it != map.end()) return it->second;
    if (s.rfind("0X", 0) == 0) { try { return std::stoi(s, nullptr, 16); } catch (...) {} }
    try { return std::stoi(s); } catch (...) {}
    return 0;
}

// Reverse of keyNameToVK for writing prefs back (panel rebind -> ini). Falls
// back to the numeric VK (which keyNameToVK also accepts) for unmapped keys.
inline std::string keyVkToName(int vk)
{
    if (vk == 0)                       return "NONE";
    if (vk >= 'A' && vk <= 'Z')        return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9')        return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F12)   return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk) {
        case VK_SPACE:    return "SPACE";   case VK_RETURN:  return "ENTER";
        case VK_TAB:      return "TAB";     case VK_CAPITAL: return "CAPSLOCK";
        case VK_LSHIFT:   return "LSHIFT";  case VK_RSHIFT:  return "RSHIFT";
        case VK_LCONTROL: return "LCTRL";   case VK_RCONTROL:return "RCTRL";
        case VK_LMENU:    return "LALT";    case VK_RMENU:   return "RALT";
        case VK_OEM_3:    return "TILDE";   case VK_XBUTTON1:return "MOUSE4";
        case VK_XBUTTON2: return "MOUSE5";  case VK_UP:      return "UP";
        case VK_DOWN:     return "DOWN";    case VK_LEFT:    return "LEFT";
        case VK_RIGHT:    return "RIGHT";   case VK_INSERT:  return "INSERT";
        case VK_DELETE:   return "DELETE";  case VK_HOME:    return "HOME";
        case VK_END:      return "END";     case VK_PRIOR:   return "PAGEUP";
        case VK_NEXT:     return "PAGEDOWN";
    }
    return std::to_string(vk);
}

struct ClientConfig {
    // [network]  — where the relay lives (the game server's host). The audio
    // port is the relay's :audio; cmd_port is where the token arrives.
    std::string relayIp   = "127.0.0.1";
    int         audioPort = 7779;
    int         cmdPort   = 7780;

    // [keys]
    int  pttKey   = 'B';          // proximity push-to-talk
    int  pttKey2  = 'N';          // 2nd PTT (use for radio; pair with /vradio)
    int  muteKey  = VK_F9;        // self-mute toggle
    int  panelKey = VK_INSERT;    // ImGui control panel toggle (avoid SA-MP F-keys)

    // [audio] — purely local mixing prefs (never affects other players).
    float masterVolume    = 1.0f;
    float proximityVolume = 1.0f;
    float radioVolume     = 1.0f;
    float micVolume       = 1.0f;
    bool  vadEnabled      = false; // voice mode: false = PTT only, true = VAD only (exclusive)
    float vadThreshold    = 0.02f; // RMS gate (0..1)
    bool  deafen          = false; // hear no one (control panel)
    bool  noiseSuppress   = true;  // RNNoise neural noise suppression on the mic

    // [devices] — chosen WASAPI endpoints (friendly name; empty = system default).
    // Stored by name (stable across reboots better than the endpoint id).
    std::string inputDevice;       // microphone
    std::string outputDevice;      // speakers/headphones

    // [ui] — on-screen "who's talking" overlay (stage G2). Local only.
    bool  showOverlay     = true;     // create the panel on start
    int   overlayKey      = VK_F8;    // toggle show/hide (F9 is taken by mute)
    std::string overlayAnchor = "radar"; // "radar" = above the minimap; "topleft" = use x/y
    int   overlayX        = 20;       // TopLeft anchor: offset from window top-left (px)
    int   overlayY        = 150;

    static ClientConfig load(const char* path = "voicechat.ini")
    {
        ClientConfig c;
        std::ifstream f(path);
        if (!f.is_open()) { save(c, path); return c; }   // write a template
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            auto cm = v.find(';'); if (cm != std::string::npos) v = trim(v.substr(0, cm));
            if      (k == "relay_ip")        c.relayIp = v;
            else if (k == "audio_port")      c.audioPort = atoi(v.c_str());
            else if (k == "cmd_port")        c.cmdPort = atoi(v.c_str());
            else if (k == "ptt_key")         c.pttKey = (v=="NONE"?0:keyNameToVK(v));
            else if (k == "ptt_key2")        c.pttKey2 = (v=="NONE"?0:keyNameToVK(v));
            else if (k == "mute_key")        c.muteKey = keyNameToVK(v);
            else if (k == "panel_key")       c.panelKey = (v=="NONE"?0:keyNameToVK(v));
            else if (k == "master_volume")   c.masterVolume = (float)atof(v.c_str());
            else if (k == "proximity_volume")c.proximityVolume = (float)atof(v.c_str());
            else if (k == "radio_volume")    c.radioVolume = (float)atof(v.c_str());
            else if (k == "mic_volume")      c.micVolume = (float)atof(v.c_str());
            else if (k == "vad_enabled")     c.vadEnabled = (v=="true"||v=="1");
            else if (k == "vad_threshold")   c.vadThreshold = (float)atof(v.c_str());
            else if (k == "deafen")          c.deafen = (v=="true"||v=="1");
            else if (k == "noise_suppress")  c.noiseSuppress = (v=="true"||v=="1");
            else if (k == "input_device")    c.inputDevice = v;
            else if (k == "output_device")   c.outputDevice = v;
            else if (k == "show_overlay")    c.showOverlay = (v=="true"||v=="1");
            else if (k == "overlay_key")     c.overlayKey  = (v=="NONE"?0:keyNameToVK(v));
            else if (k == "overlay_anchor")  c.overlayAnchor = v;
            else if (k == "overlay_x")       c.overlayX    = atoi(v.c_str());
            else if (k == "overlay_y")       c.overlayY    = atoi(v.c_str());
        }
        return c;
    }

    static void save(const ClientConfig& c, const char* path = "voicechat.ini")
    {
        std::ofstream f(path);
        if (!f.is_open()) return;
        f << "; VoiceChat v2 client — LOCAL preferences (does not affect other players)\n";
        f << "[network]\n";
        f << "relay_ip   = " << c.relayIp   << "   ; server IP (open.mp/SA-MP)\n";
        f << "audio_port = " << c.audioPort << "\n";
        f << "cmd_port   = " << c.cmdPort   << "   ; port the auth token arrives on\n\n";
        f << "[keys]\n";
        f << "ptt_key  = " << keyVkToName(c.pttKey)  << "     ; proximity PTT (hold to talk)\n";
        f << "ptt_key2 = " << keyVkToName(c.pttKey2) << "     ; radio PTT (use with /vradio)\n";
        f << "mute_key = " << keyVkToName(c.muteKey) << "\n";
        f << "panel_key = " << keyVkToName(c.panelKey) << "   ; opens the settings panel (avoid SA-MP's F-keys)\n\n";
        f << "[audio]\n";
        f << "master_volume    = " << c.masterVolume    << "\n";
        f << "proximity_volume = " << c.proximityVolume << "\n";
        f << "radio_volume     = " << c.radioVolume     << "\n";
        f << "mic_volume       = " << c.micVolume       << "\n";
        f << "vad_enabled      = " << (c.vadEnabled ? "true" : "false") << "   ; mode: false=PTT, true=VAD\n";
        f << "vad_threshold    = " << c.vadThreshold    << "\n";
        f << "deafen           = " << (c.deafen ? "true" : "false") << "\n";
        f << "noise_suppress   = " << (c.noiseSuppress ? "true" : "false") << "   ; RNNoise mic noise suppression\n\n";
        f << "[devices]\n";
        f << "input_device  = " << c.inputDevice  << "   ; microphone name (empty = system default)\n";
        f << "output_device = " << c.outputDevice << "   ; output name (empty = system default)\n\n";
        f << "[ui]\n";
        f << "show_overlay   = true   ; on-screen \"who's talking\" HUD\n";
        f << "overlay_key    = F8     ; toggle key (show/hide)\n";
        f << "overlay_anchor = " << c.overlayAnchor << "  ; radar = above the minimap; topleft = use x/y\n";
        f << "overlay_x      = " << c.overlayX << "      ; only for anchor=topleft (from the top-left corner)\n";
        f << "overlay_y      = " << c.overlayY << "\n";
    }

private:
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
};

} // namespace vc::client
