// ============================================================
//  VoiceChat client (.asi) — on-screen overlay (stage G2)
//  File: client/src/overlay.cpp   — see overlay.h for the contract.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <cstdio>

#include "../include/overlay.h"

namespace vc::client::overlay {
namespace {

// A speaker is "talking" if a frame arrived within this window. The relay only
// forwards audible proximity speakers / active bus members, so a fresh frame is
// a reliable "talking now" signal. ~350ms rides over Opus frame gaps + jitter.
constexpr uint64_t kActiveMs   = 350;
constexpr uint32_t kRepaintMs  = 100;     // ~10 Hz; enough for a HUD
constexpr int      kWidth      = 200;
constexpr int      kHeight     = 230;
constexpr uint32_t kBusBit     = 0x10000; // map-key flag to split bus vs proximity

// Color-key for the layered window: every pixel painted in this exact color is
// rendered fully transparent, so the HUD has NO background box — only the text
// shows over the game. Picked as pure magenta because no text/shadow uses it.
constexpr COLORREF kKeyColor   = RGB(255, 0, 255);

struct Talker { uint64_t lastMs = 0; bool bus = false; };

std::mutex                          g_mtx;
std::unordered_map<uint32_t, Talker> g_talkers;   // key: id | (bus?kBusBit:0)
std::atomic<bool>                   g_connected{ false };
std::atomic<bool>                   g_ptt{ false };
std::atomic<bool>                   g_muted{ false };
std::atomic<bool>                   g_imguiMode{ false }; // D3D9/ImGui owns the HUD

HWND  g_wnd     = nullptr;
HWND  g_game    = nullptr;
HFONT g_font    = nullptr;
bool  g_visible = true;
OverlayCfg g_cfg;
uint32_t g_lastPaint = 0;

inline uint64_t nowMs() { return GetTickCount64(); }

// Find this process's main game window (most robust: largest visible top-level
// window owned by the GTA process the .asi is injected into — class/title vary
// with SA-MP/open.mp builds).
struct EnumCtx { HWND best = nullptr; long area = 0; DWORD pid = 0; };
BOOL CALLBACK enumProc(HWND h, LPARAM lp)
{
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; if (!GetWindowRect(h, &r)) return TRUE;
    long area = (long)(r.right - r.left) * (r.bottom - r.top);
    if (area > c->area) { c->area = area; c->best = h; }
    return TRUE;
}
HWND findGameWindow()
{
    EnumCtx c; c.pid = GetCurrentProcessId();
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&c));
    return c.best;
}

// Screen-space top-left where the HUD window should sit. RadarLeft pins it in
// the bottom-left, directly above the GTA SA radar/minimap; TopLeft keeps the
// legacy offset-from-top-left behaviour. Falls back to raw cfg coords if the
// game window isn't known yet.
POINT computeOrigin()
{
    POINT org{ g_cfg.x, g_cfg.y };
    if (!g_game || !IsWindow(g_game)) return org;   // screen coords as-is

    RECT cr{}; GetClientRect(g_game, &cr);
    const int ch = cr.bottom - cr.top;
    const int cw = cr.right  - cr.left;

    if (g_cfg.anchor == OverlayCfg::RadarLeft && ch > 0 && cw > 0) {
        // The SA radar is anchored bottom-left and scales with screen height.
        // Reserve a band for it and drop the panel so its bottom edge lands just
        // above the radar's top. These are proportional estimates that track the
        // default HUD across resolutions.
        int botMargin = (int)(ch * 0.02f);
        int radarH    = (int)(ch * 0.22f);
        if (radarH < 120) radarH = 120;
        if (radarH > 260) radarH = 260;
        int leftMargin = (int)(cw * 0.015f);
        if (leftMargin < 12) leftMargin = 12;

        org.x = leftMargin;
        org.y = ch - botMargin - radarH - kHeight;
        if (org.y < 0) org.y = 0;
    } else {
        org.x = g_cfg.x;
        org.y = g_cfg.y;
    }
    ClientToScreen(g_game, &org);
    return org;
}

void paint(HWND hWnd)
{
    PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc; GetClientRect(hWnd, &rc);

    // Fill with the color-key: these pixels become transparent (no background
    // box). Only the text drawn on top of it remains visible over the game.
    HBRUSH bg = CreateSolidBrush(kKeyColor);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (g_font) SelectObject(hdc, g_font);
    SetBkMode(hdc, TRANSPARENT);

    std::vector<HudLine> lines;
    buildHud(lines);

    int y = 6;
    for (const HudLine& l : lines) {
        // With no background box, text rides directly over the scene — draw a
        // black drop shadow first so it stays readable over bright terrain.
        RECT tr = { 8, y, rc.right - 6, y + 18 };
        RECT sh = { tr.left + 1, tr.top + 1, tr.right + 1, tr.bottom + 1 };
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextA(hdc, l.text.c_str(), -1, &sh, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(hdc, RGB(l.r, l.g, l.b));
        DrawTextA(hdc, l.text.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        y += 17;
    }
    EndPaint(hWnd, &ps);
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT)      { paint(h); return 0; }
    if (m == WM_ERASEBKGND) return 1;                 // no flicker
    return DefWindowProcA(h, m, w, l);
}

} // namespace

void init(const OverlayCfg& cfg)
{
    g_cfg = cfg;
    g_visible = cfg.show;

    // No GDI window requested (ImGui build): the D3D9 HUD is the sole renderer.
    // Keep the data layer live (buildHud/noteSpeaker/toggle all work on g_wnd ==
    // nullptr) but create no window, so the two HUDs can't overlap.
    if (!cfg.ownWindow) {
        printf("[overlay] data-only (ImGui HUD renders; no GDI window)\n");
        return;
    }

    for (int i = 0; i < 40 && !g_game; ++i) {         // game window may lag on load
        g_game = findGameWindow();
        if (!g_game) Sleep(250);
    }

    WNDCLASSEXA wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "VoiceChatOverlayV2";
    RegisterClassExA(&wc);

    POINT org = computeOrigin();

    g_wnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        "VoiceChatOverlayV2", "", WS_POPUP,
        org.x, org.y, kWidth, kHeight,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!g_wnd) { printf("[overlay] CreateWindow failed (%lu)\n", GetLastError()); return; }

    // Color-key transparency: kKeyColor pixels disappear, leaving only the text.
    SetLayeredWindowAttributes(g_wnd, kKeyColor, 0, LWA_COLORKEY);
    // NON-antialiased so glyph edges don't blend toward the key color (which
    // would leave a magenta fringe around every letter).
    g_font = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

    // Respect an ImGui takeover that happened while we were waiting for the
    // game window (the D3D9 hook may beat this init) — never double-render.
    ShowWindow(g_wnd, (g_visible && !g_imguiMode.load()) ? SW_SHOWNOACTIVATE : SW_HIDE);
    printf("[overlay] ready (%s)%s\n", g_visible ? "shown" : "hidden",
           g_game ? "" : " — game window not found, using screen coords");
}

void shutdown()
{
    if (g_wnd)  { DestroyWindow(g_wnd); g_wnd = nullptr; }
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    UnregisterClassA("VoiceChatOverlayV2", GetModuleHandleA(nullptr));
}

void setStatus(bool connected, bool ptt, bool muted)
{
    g_connected.store(connected); g_ptt.store(ptt); g_muted.store(muted);
}

void noteSpeaker(uint16_t id, bool isBus)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    Talker& t = g_talkers[(uint32_t)id | (isBus ? kBusBit : 0u)];
    t.lastMs = nowMs(); t.bus = isBus;
}

void removeSpeaker(uint16_t id)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_talkers.erase((uint32_t)id);
    g_talkers.erase((uint32_t)id | kBusBit);
}

bool buildHud(std::vector<HudLine>& out)
{
    if (!g_visible) return false;

    const bool connected = g_connected.load();
    const bool ptt = g_ptt.load();
    const bool muted = g_muted.load();

    out.push_back({ "[ VoiceChat ]", 120, 200, 255 });          // title
    if (connected) out.push_back({ "Status: CONNECTED", 120, 230, 120 });
    else           out.push_back({ "Status: OFFLINE",   230, 120, 120 });
    if (muted)     out.push_back({ "! MUTED !",       255,  80,  80 });
    else if (ptt)  out.push_back({ ">> TRANSMIT <<",   80, 255,  80 });
    else           out.push_back({ "PTT: hold to talk",205, 205, 205 });
    out.push_back({ "" });
    out.push_back({ "Talking:" });

    std::vector<HudLine> who;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const uint64_t t = nowMs();
        for (auto it = g_talkers.begin(); it != g_talkers.end(); ) {
            if (t - it->second.lastMs > kActiveMs) { it = g_talkers.erase(it); continue; }
            uint16_t id = (uint16_t)(it->first & 0xFFFF);
            char buf[48];
            if (it->second.bus) { std::snprintf(buf, sizeof(buf), "  Radio CH%u", id);
                                  who.push_back({ buf, 255, 200, 90 }); }       // radio
            else if (id & 0x8000) {  // J6 speakerphone synthetic id (SPEAKERPHONE_ID_BIT)
                                  std::snprintf(buf, sizeof(buf), "  Phone P%u", id & 0x7FFF);
                                  who.push_back({ buf, 200, 160, 255 }); }      // phone-on-speaker
            else                { std::snprintf(buf, sizeof(buf), "  Player %u", id);
                                  who.push_back({ buf, 120, 230, 120 }); }      // proximity
            ++it;
        }
    }
    if (who.empty()) out.push_back({ "  (nobody)" });
    else for (auto& w : who) out.push_back(std::move(w));
    return true;
}

void setRenderMode(bool imguiOwnsHud)
{
    if (g_imguiMode.exchange(imguiOwnsHud) == imguiOwnsHud) return;  // no change
    if (imguiOwnsHud && g_wnd) ShowWindow(g_wnd, SW_HIDE);           // ImGui takes over
    else if (g_wnd) ShowWindow(g_wnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void toggle()
{
    g_visible = !g_visible;
    // ImGui HUD reads g_visible via buildHud(); only the GDI window needs the
    // explicit show/hide (and only when it owns the HUD).
    if (g_wnd && !g_imguiMode.load())
        ShowWindow(g_wnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void pump()
{
    if (!g_wnd) return;

    // When the D3D9/ImGui renderer owns the HUD, the GDI window stays hidden;
    // we only keep the message queue drained so it doesn't back up.
    if (g_imguiMode.load()) {
        // Self-heal on the owning thread: NOTHING may leave this window visible
        // while ImGui draws the HUD, or the two render on top of each other
        // (doubled/overwritten text). Covers any show/hide race regardless of
        // which thread or path caused it.
        if (IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_HIDE);
        MSG m; while (PeekMessageA(&m, g_wnd, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        return;
    }

    // Follow the game window (handles windowed-mode moves + res changes) and
    // stay on top, re-pinned above the radar each frame.
    if (g_game && IsWindow(g_game)) {
        POINT org = computeOrigin();
        SetWindowPos(g_wnd, HWND_TOPMOST, org.x, org.y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
    }

    if (g_visible) {
        uint32_t t = GetTickCount();
        if (t - g_lastPaint >= kRepaintMs) { InvalidateRect(g_wnd, nullptr, FALSE); g_lastPaint = t; }
    }

    MSG msg;
    while (PeekMessageA(&msg, g_wnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
}

bool active() { return g_wnd != nullptr; }

} // namespace vc::client::overlay
