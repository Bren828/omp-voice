// ============================================================
//  VoiceChat client (.asi) — D3D9 + ImGui plumbing for the control panel
//  File: client/include/d3d9_overlay.h
//
//  Hooks IDirect3DDevice9::EndScene + Reset (via MinHook) and subclasses the
//  game window's WndProc so Dear ImGui can draw the interactive "Voice -
//  setari" panel (voice_panel) ON TOP of GTA SA — including exclusive
//  fullscreen, unlike the layered-GDI overlay. F7 toggles the panel; while it
//  is open the cursor shows and game input is swallowed.
//
//  This deliberately reintroduces D3D9/MinHook into the v2 client (the GDI
//  overlay stays for the passive "who's talking" HUD). Build-gated by
//  VC_CLIENT_GUI; dllmain calls install() once the game's device exists.
// ============================================================
#pragma once

namespace vc::client::d3d9 {

// Install the EndScene/Reset hooks. Returns false if the game window / a D3D9
// device isn't available yet (dllmain retries). Safe to call until it succeeds.
bool install();
void shutdown();
bool installed();

// Toggle/query the panel. togglePanel() is thread-safe (just flips an atomic);
// the open/close side effects (cursor, device refresh, config save) run on the
// render thread inside the EndScene hook. dllmain drives this from its key loop.
void togglePanel();
bool panelOpen();

} // namespace vc::client::d3d9
