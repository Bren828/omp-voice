// ============================================================
//  VoiceChat client — ImGui control panel implementation
//  File: client/src/voice_panel.cpp
//
//  Draws the "Voice Settings" window. Mirrors the mockup:
//    Volume (Master/Microphone + Deafen) | Voice mode (PTT/VAD + key rebind) |
//    Devices (Microphone/Output) | Players in range (per-player mute + volume).
//  Edits only local prefs; mode-switch gated by the server.
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>          // VK_F1.. for the PTT-key label
#include "imgui.h"
#include "../include/voice_panel.h"

#include <atomic>
#include <cstdio>             // snprintf

namespace vc::client::panel {

static Backend       g_be;
static VoiceSettings g_set;

// Device lists are cached while the panel is open so the combos are stable
// across frames (re-enumerated on each open via onShow / the Refresh button).
static std::vector<DeviceItem> g_inputs;
static std::vector<DeviceItem> g_outputs;
static int g_inputIdx  = -1;
static int g_outputIdx = -1;

static std::atomic<bool> g_rebind{ false };     // waiting for a key for PTT
static std::atomic<bool> g_recenter{ false };   // re-center the window on next open

static int findByName(const std::vector<DeviceItem>& list, const std::string& name)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].name == name) return (int)i;
    return -1;
}

static void refreshDevices()
{
    g_inputs  = g_be.inputDevices  ? g_be.inputDevices()  : std::vector<DeviceItem>{};
    g_outputs = g_be.outputDevices ? g_be.outputDevices() : std::vector<DeviceItem>{};
    g_inputIdx  = findByName(g_inputs,  g_be.currentInputName);
    g_outputIdx = findByName(g_outputs, g_be.currentOutputName);
}

void init(const Backend& backend, const VoiceSettings& initial)
{
    g_be  = backend;
    g_set = initial;
}

void onShow() { refreshDevices(); g_recenter.store(true); }

void onHide() { if (g_be.save) g_be.save(); }   // persist on close

void beginPttRebind()       { g_rebind.store(true); }
bool pttRebindActive()      { return g_rebind.load(); }

void applyPttRebind(int vk)
{
    g_set.pttKey = vk;
    if (g_be.setPttKey) g_be.setPttKey(vk);
    g_rebind.store(false);
}

void cancelPttRebind() { g_rebind.store(false); }

// Char label for a VK code (printable letters/digits, else a short tag).
static const char* vkLabel(int vk, char* buf, size_t n)
{
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(buf, n, "F%d", vk - VK_F1 + 1); return buf; }
    snprintf(buf, n, "0x%02X", vk);
    return buf;
}

// One device combo (BeginCombo/Selectable so we can use std::string names).
static bool deviceCombo(const char* label, const std::vector<DeviceItem>& list,
                        int& idx, const std::function<void(const DeviceItem&)>& select)
{
    const char* preview = (idx >= 0 && idx < (int)list.size()) ? list[idx].name.c_str()
                                                               : "(system default)";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < (int)list.size(); ++i) {
            bool sel = (i == idx);
            if (ImGui::Selectable(list[i].name.c_str(), sel)) {
                idx = i; changed = true;
                if (select) select(list[i]);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void render()
{
    // Center the window when it opens (so it never lands under the SA-MP chat in
    // the top-left); keep the user's drag position while it stays open.
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGuiCond posCond = g_recenter.exchange(false) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(center, posCond, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);   // fixed width, auto height
    ImGui::SetNextWindowBgAlpha(1.0f);                            // opaque (chat won't bleed through)

    ImGui::Begin("Voice Settings", nullptr, ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoScrollbar);

    ImGui::TextDisabled("Press INSERT to close");
    ImGui::Spacing();

    // ---- Volume ----
    ImGui::SeparatorText("Volume");
    if (ImGui::SliderFloat("Master", &g_set.master, 0.0f, 1.0f, "%.2f"))
        if (g_be.setMaster) g_be.setMaster(g_set.master);
    // "##vol" keeps the visible label "Microphone" while giving the slider a
    // distinct ImGui ID from the "Microphone" device combo below (same window).
    if (ImGui::SliderFloat("Microphone##vol", &g_set.mic, 0.0f, 1.0f, "%.2f"))
        if (g_be.setMic) g_be.setMic(g_set.mic);
    if (ImGui::Checkbox("Deafen (mute all incoming)", &g_set.deafen))
        if (g_be.setDeafen) g_be.setDeafen(g_set.deafen);

    // ---- Voice mode ----
    ImGui::SeparatorText("Voice mode");
    bool allowSwitch = g_be.serverAllowsModeSwitch ? g_be.serverAllowsModeSwitch() : true;
    if (allowSwitch) {
        const char* modes[] = { "Push-to-Talk", "Voice Activation" };
        if (ImGui::Combo("Mode", &g_set.mode, modes, 2))
            if (g_be.setMode) g_be.setMode(g_set.mode);
        if (g_set.mode == 1) {
            if (ImGui::SliderFloat("VAD sensitivity", &g_set.vadSensitivity, 0.0f, 1.0f, "%.2f"))
                if (g_be.setVadSensitivity) g_be.setVadSensitivity(g_set.vadSensitivity);
        }
    } else {
        ImGui::TextDisabled("Server enforces Push-to-Talk");
    }

    // PTT key rebind
    char kb[8];
    if (ImGui::Button(g_rebind.load() ? "Press a key..." : "Rebind PTT key"))
        beginPttRebind();
    ImGui::SameLine();
    ImGui::Text("Key: %s", vkLabel(g_set.pttKey, kb, sizeof(kb)));

    if (ImGui::Checkbox("Noise suppression (RNNoise)", &g_set.noiseSuppress))
        if (g_be.setNoiseSuppress) g_be.setNoiseSuppress(g_set.noiseSuppress);

    // ---- Devices ----
    ImGui::SeparatorText("Devices");
    deviceCombo("Microphone##dev", g_inputs,  g_inputIdx,  g_be.selectInput);
    deviceCombo("Output",          g_outputs, g_outputIdx, g_be.selectOutput);
    if (ImGui::Button("Refresh devices")) refreshDevices();

    // ---- Players in range ----
    ImGui::SeparatorText("Players in range");
    std::vector<PlayerItem> players = g_be.audiblePlayers ? g_be.audiblePlayers()
                                                          : std::vector<PlayerItem>{};
    if (players.empty())
        ImGui::TextDisabled("(no one in range)");
    for (auto& p : players) {
        ImGui::PushID((int)p.id);

        bool muted = g_be.isMuted ? g_be.isMuted(p.id) : false;
        if (ImGui::Checkbox("##mute", &muted))
            if (g_be.setMute) g_be.setMute(p.id, muted);

        ImGui::SameLine();
        ImGui::TextUnformatted(p.name.c_str());

        ImGui::SameLine();
        float vol = g_be.getVol ? g_be.getVol(p.id) : 1.0f;
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::SliderFloat("##vol", &vol, 0.0f, 2.0f, "%.1f"))
            if (g_be.setVol) g_be.setVol(p.id, vol);

        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace vc::client::panel
