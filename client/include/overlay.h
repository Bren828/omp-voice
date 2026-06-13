// ============================================================
//  VoiceChat client (.asi) — on-screen overlay (stage G2)
//  File: client/include/overlay.h
//
//  A lightweight "who's talking" HUD. Migrated from the legacy
//  voicechat-asi/ DebugOverlay: a LAYERED, click-through, top-most Win32
//  window drawn with GDI — NO D3D9 hook, NO MinHook, no extra deps.
//
//  It shows: connection status, local PTT/mute state, and the list of
//  players currently audible (proximity) or transmitting on a radio bus.
//  Identity stays token-based: we only know speaker IDs (the SA-MP playerid
//  the relay tags frames with), never names — so it renders "Player <id>".
//
//  Caveat: a layered window draws over the game only in WINDOWED / BORDERLESS
//  mode. True exclusive fullscreen (D3D9) paints over it; switch GTA to
//  windowed/borderless (SilentPatchSA can force this) if the panel is hidden.
//
//  Threading: init()/pump()/toggle()/shutdown() run on the client worker
//  thread (it owns the window + message pump). noteSpeaker()/removeSpeaker()/
//  setStatus() are safe to call from the network thread (state is guarded).
// ============================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vc::client::overlay {

// One rendered HUD line + its color. The SAME snapshot drives both renderers:
// the GDI window (paint) and the D3D9/ImGui HUD (d3d9_overlay::drawHud).
struct HudLine { std::string text; uint8_t r = 205, g = 205, b = 205; };

struct OverlayCfg {
    // Where to pin the HUD:
    //   RadarLeft = bottom-left, sitting just above the minimap/radar (default).
    //   TopLeft   = legacy: offset x/y from the game window's top-left.
    enum Anchor { RadarLeft, TopLeft };

    bool   show   = true;       // create + show the panel on start
    Anchor anchor = RadarLeft;  // default: park it above the radar
    int    x      = 20;         // TopLeft mode: offset from client top-left (px)
    int    y      = 150;
    // Create the layered GDI window? In the ImGui build the D3D9 HUD draws the
    // SAME lines in the SAME spot, so a GDI window is a redundant second copy
    // (they overlap above the radar -> doubled text). Set false there; the data
    // layer (buildHud/noteSpeaker/toggle/g_visible) still works without a window.
    bool   ownWindow = true;
};

// Create the layered window over the game. Safe to call once; a missing game
// window or a failed create just disables the overlay (everything else runs).
void init(const OverlayCfg& cfg);
void shutdown();

// Latest local state (cheap; call every frame).
void setStatus(bool connected, bool ptt, bool muted);

// A decoded frame arrived for this speaker => they are talking right now.
// isBus = a radio/phone channel bus (rendered as "Radio CH<id>").
void noteSpeaker(uint16_t id, bool isBus);
void removeSpeaker(uint16_t id);            // speaker left / stream gone

// Roster: bind a display name to a speaker id (from the relay, G2). When known
// the panel shows the name instead of "Player <id>".
void setName(uint16_t id, const char* name);

void toggle();    // show <-> hide (bind to a key)
void pump();      // reposition + repaint + drain window messages (per loop)
bool active();    // window exists

// Snapshot of what to draw right now: status + active-talker lines, colored.
// Also GCs stale talkers. Returns false (and leaves `out` empty) when hidden.
// Safe to call from the render thread (state is guarded).
bool buildHud(std::vector<HudLine>& out);

// Hand the HUD over to the D3D9/ImGui renderer (so it shows in exclusive
// fullscreen, where the layered GDI window can't). When enabled the GDI window
// is hidden and stops painting; the talker/status data keeps flowing. Idempotent.
void setRenderMode(bool imguiOwnsHud);

} // namespace vc::client::overlay
