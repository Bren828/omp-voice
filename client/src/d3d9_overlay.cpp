// ============================================================
//  VoiceChat client — D3D9 + ImGui plumbing implementation
//  File: client/src/d3d9_overlay.cpp
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <atomic>
#include <cstdio>
#include <vector>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d9.lib")

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "../include/d3d9_overlay.h"
#include "../include/voice_panel.h"
#include "../include/overlay.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace vc::client::d3d9 {

using EndScene_t = HRESULT(__stdcall*)(IDirect3DDevice9*);
using Reset_t    = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static EndScene_t s_origEndScene = nullptr;
static Reset_t    s_origReset    = nullptr;
static HWND s_hwnd = nullptr;
static WNDPROC s_origWndProc = nullptr;
static bool s_installed = false;
static bool s_imguiReady = false;
static std::atomic<bool> s_panelOpen{ false };
static bool s_lastOpen = false;

static std::atomic<IDirectInputDevice8A*> s_mouseDev{ nullptr };
static std::atomic<long> s_dx{ 0 }, s_dy{ 0 }, s_dz{ 0 };
static long s_curX = 0, s_curY = 0;
static bool s_centerCursor = false;

static const char* kGameWndClass = "Grand theft auto San Andreas";

static bool isGameInputMsg(UINT msg)
{
    switch (msg) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
            return true;
        default:
            return false;
    }
}

void togglePanel() { s_panelOpen.store(!s_panelOpen.load()); }
bool panelOpen() { return s_panelOpen.load(); }

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (s_panelOpen.load()) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        if (isGameInputMsg(msg)) return TRUE;
    }
    return CallWindowProc(s_origWndProc, h, msg, w, l);
}

static void applyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(9, 6);
    s.ItemSpacing = ImVec2(10, 11);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c = s.Colors;
    const ImVec4 accent = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    const ImVec4 accentD = ImVec4(0.18f, 0.43f, 0.78f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.13f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBgActive] = accentD;
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.26f, 0.33f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.31f, 0.40f, 1.00f);
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.36f, 0.69f, 1.00f, 1.00f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Button] = accentD;
    c[ImGuiCol_ButtonHovered] = accent;
    c[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.69f, 1.00f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.30f, 0.45f, 1.00f);
    c[ImGuiCol_HeaderHovered] = accentD;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Border] = ImVec4(0.26f, 0.31f, 0.40f, 0.60f);
}

static void ensureImGui(IDirect3DDevice9* dev)
{
    if (s_imguiReady) return;

    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(dev->GetCreationParameters(&cp)) && cp.hFocusWindow)
        s_hwnd = cp.hFocusWindow;
    if (!s_hwnd) s_hwnd = FindWindowA(kGameWndClass, nullptr);
    if (!s_hwnd) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigDebugHighlightIdConflicts = false;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    const char* fonts[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        "C:\\Windows\\Fonts\\arial.ttf"
    };
    ImFont* f = nullptr;
    for (const char* path : fonts) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        f = io.Fonts->AddFontFromFileTTF(path, 20.0f);
        if (f) break;
    }
    if (!f) io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();
    applyTheme();
    ImGui_ImplWin32_Init(s_hwnd);
    ImGui_ImplDX9_Init(dev);
    s_origWndProc = (WNDPROC)SetWindowLongPtr(s_hwnd, GWLP_WNDPROC, (LONG_PTR)wndProc);
    s_imguiReady = true;
    printf("[gui] ImGui ready (hwnd=%p)\n", (void*)s_hwnd);
}

static void drawHud()
{
    std::vector<overlay::HudLine> lines;
    if (!overlay::buildHud(lines) || lines.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float cw = io.DisplaySize.x, ch = io.DisplaySize.y;
    if (cw <= 0.0f || ch <= 0.0f) return;

    float botMargin = ch * 0.02f;
    float radarH = ch * 0.22f;
    if (radarH < 120) radarH = 120;
    if (radarH > 260) radarH = 260;
    float leftMargin = cw * 0.015f;
    if (leftMargin < 12) leftMargin = 12;
    const float yBottom = ch - botMargin - radarH;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(ImVec2(leftMargin, yBottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);

    if (ImGui::Begin("##vc_hud", nullptr, flags)) {
        // Keep HUD text at the real font size. The previous 0.80 scale made
        // CONNECTED/OFFLINE smaller and visually blurry.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lh = ImGui::GetTextLineHeight() + 3.0f;
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        float maxW = 0.0f;
        float y = cur.y;

        for (const overlay::HudLine& l : lines) {
            if (l.text.empty()) {
                y += lh * 0.4f;
                continue;
            }
            const ImVec2 p(cur.x, y);
            dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 210), l.text.c_str());
            dl->AddText(p, IM_COL32(l.r, l.g, l.b, 255), l.text.c_str());
            float w = ImGui::CalcTextSize(l.text.c_str()).x;
            if (w > maxW) maxW = w;
            y += lh;
        }

        ImGui::Dummy(ImVec2(maxW + 2.0f, y - cur.y));
    }
    ImGui::End();
}

static HRESULT __stdcall hookedEndScene(IDirect3DDevice9* dev)
{
    static bool first = true;
    if (first) {
        first = false;
        printf("[gui] EndScene hook firing (dev=%p)\n", (void*)dev);
    }

    ensureImGui(dev);
    if (!s_imguiReady) return s_origEndScene(dev);

    overlay::setRenderMode(true);

    bool open = s_panelOpen.load();
    if (open != s_lastOpen) {
        if (open) {
            panel::onShow();
            s_centerCursor = true;
            s_dx.store(0); s_dy.store(0); s_dz.store(0);
        } else {
            panel::onHide();
        }
        s_lastOpen = open;
    }
    ImGui::GetIO().MouseDrawCursor = open;

    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    if (open) {
        ImGuiIO& io = ImGui::GetIO();
        if (s_centerCursor) {
            s_curX = (long)(io.DisplaySize.x * 0.5f);
            s_curY = (long)(io.DisplaySize.y * 0.5f);
            s_centerCursor = false;
        }
        s_curX += s_dx.exchange(0);
        s_curY += s_dy.exchange(0);
        if (s_curX < 0) s_curX = 0;
        if (s_curY < 0) s_curY = 0;
        if (s_curX > (long)io.DisplaySize.x) s_curX = (long)io.DisplaySize.x;
        if (s_curY > (long)io.DisplaySize.y) s_curY = (long)io.DisplaySize.y;
        io.AddMousePosEvent((float)s_curX, (float)s_curY);
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
        long wheel = s_dz.exchange(0);
        if (wheel) io.AddMouseWheelEvent(0.0f, (float)wheel / 120.0f);
    }

    ImGui::NewFrame();
    drawHud();
    if (open) panel::render();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    if (sb) {
        sb->Apply();
        sb->Release();
    }
    return s_origEndScene(dev);
}

static HRESULT __stdcall hookedReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    if (s_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = s_origReset(dev, pp);
    if (s_imguiReady && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

using DIGetDeviceState_t = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPVOID);
using DIGetDeviceData_t = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD,
                                               LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static DIGetDeviceState_t s_origGetDeviceState = nullptr;
static DIGetDeviceData_t s_origGetDeviceData = nullptr;

static HRESULT __stdcall hookedGetDeviceState(IDirectInputDevice8A* dev, DWORD cb, LPVOID data)
{
    HRESULT hr = s_origGetDeviceState(dev, cb, data);
    if (data && (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2))) {
        if (!s_mouseDev.exchange(dev))
            printf("[gui] DI mouse device identified (%p)\n", (void*)dev);
        if (s_panelOpen.load() && SUCCEEDED(hr)) {
            auto* ms = reinterpret_cast<DIMOUSESTATE*>(data);
            s_dx.fetch_add(ms->lX);
            s_dy.fetch_add(ms->lY);
            s_dz.fetch_add(ms->lZ);
            ZeroMemory(data, cb);
        }
    }
    return hr;
}

static HRESULT __stdcall hookedGetDeviceData(IDirectInputDevice8A* dev, DWORD cbObj,
                                             LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut,
                                             DWORD flags)
{
    HRESULT hr = s_origGetDeviceData(dev, cbObj, rgdod, pdwInOut, flags);
    if (s_panelOpen.load() && dev == s_mouseDev.load() && pdwInOut && SUCCEEDED(hr)) {
        if (rgdod && cbObj >= sizeof(DIDEVICEOBJECTDATA_DX3) && !(flags & DIGDD_PEEK)) {
            for (DWORD i = 0; i < *pdwInOut; ++i) {
                auto* d = reinterpret_cast<const DIDEVICEOBJECTDATA*>(
                    reinterpret_cast<const BYTE*>(rgdod) + (size_t)i * cbObj);
                switch (d->dwOfs) {
                    case DIMOFS_X: s_dx.fetch_add((LONG)d->dwData); break;
                    case DIMOFS_Y: s_dy.fetch_add((LONG)d->dwData); break;
                    case DIMOFS_Z: s_dz.fetch_add((LONG)d->dwData); break;
                    default: break;
                }
            }
        }
        *pdwInOut = 0;
    }
    return hr;
}

static bool hookDInputMouse()
{
    HMODULE dinput = GetModuleHandleA("dinput8.dll");
    if (!dinput) dinput = LoadLibraryA("dinput8.dll");
    if (!dinput) return false;

    using DI8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = (DI8Create_t)GetProcAddress(dinput, "DirectInput8Create");
    if (!create) return false;

    IDirectInput8A* di = nullptr;
    if (FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
                      IID_IDirectInput8A, (void**)&di, nullptr)) || !di)
        return false;

    IDirectInputDevice8A* dev = nullptr;
    HRESULT hr = di->CreateDevice(GUID_SysMouse, &dev, nullptr);
    if (FAILED(hr) || !dev) {
        di->Release();
        return false;
    }

    void** vmt = *reinterpret_cast<void***>(dev);
    void* gsAddr = vmt[9];
    void* gdAddr = vmt[10];
    dev->Release();
    di->Release();

    if (MH_CreateHook(gsAddr, &hookedGetDeviceState,
                      reinterpret_cast<void**>(&s_origGetDeviceState)) != MH_OK)
        return false;

    if (MH_CreateHook(gdAddr, &hookedGetDeviceData,
                      reinterpret_cast<void**>(&s_origGetDeviceData)) != MH_OK)
        printf("[gui] MH_CreateHook(GetDeviceData) failed\n");

    printf("[gui] DirectInput mouse gate hooked (GetDeviceState=%p)\n", gsAddr);
    return true;
}

bool install()
{
    if (s_installed) return true;

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) return false;

    HWND game = FindWindowA(kGameWndClass, nullptr);
    if (!game) return false;

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;

    HWND dummyWnd = CreateWindowExA(0, "STATIC", "", WS_OVERLAPPED,
                                    0, 0, 8, 8, nullptr, nullptr,
                                    GetModuleHandleA(nullptr), nullptr);
    if (!dummyWnd) {
        d3d->Release();
        return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = dummyWnd;
    pp.BackBufferWidth = 8;
    pp.BackBufferHeight = 8;
    pp.BackBufferCount = 1;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    struct Attempt { D3DDEVTYPE type; DWORD vp; };
    const Attempt attempts[] = {
        { D3DDEVTYPE_HAL, D3DCREATE_HARDWARE_VERTEXPROCESSING },
        { D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING },
        { D3DDEVTYPE_NULLREF, D3DCREATE_SOFTWARE_VERTEXPROCESSING },
        { D3DDEVTYPE_NULLREF, D3DCREATE_HARDWARE_VERTEXPROCESSING }
    };

    IDirect3DDevice9* dummy = nullptr;
    HRESULT hr = E_FAIL;
    for (const Attempt& a : attempts) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, a.type, dummyWnd,
                               a.vp | D3DCREATE_NOWINDOWCHANGES, &pp, &dummy);
        if (SUCCEEDED(hr) && dummy) break;
    }

    if (FAILED(hr) || !dummy) {
        DestroyWindow(dummyWnd);
        d3d->Release();
        printf("[gui] dummy CreateDevice failed (hr=0x%08lX)\n", (unsigned long)hr);
        return false;
    }

    void** vmt = *reinterpret_cast<void***>(dummy);
    void* endSceneAddr = vmt[42];
    void* resetAddr = vmt[16];
    dummy->Release();
    DestroyWindow(dummyWnd);
    d3d->Release();

    MH_STATUS es = MH_CreateHook(endSceneAddr, &hookedEndScene,
                                 reinterpret_cast<void**>(&s_origEndScene));
    if (es != MH_OK) {
        printf("[gui] MH_CreateHook(EndScene) -> %d\n", (int)es);
        return false;
    }

    MH_STATUS rs = MH_CreateHook(resetAddr, &hookedReset,
                                 reinterpret_cast<void**>(&s_origReset));
    if (rs != MH_OK)
        printf("[gui] MH_CreateHook(Reset) -> %d\n", (int)rs);

    hookDInputMouse();

    MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    if (en != MH_OK) {
        printf("[gui] MH_EnableHook -> %d\n", (int)en);
        return false;
    }

    s_installed = true;
    printf("[gui] D3D9 hooks installed (EndScene=%p Reset=%p)\n", endSceneAddr, resetAddr);
    return true;
}

void shutdown()
{
    if (!s_installed) return;

    s_panelOpen.store(false);
    MH_DisableHook(MH_ALL_HOOKS);

    if (s_imguiReady) {
        if (s_hwnd && s_origWndProc)
            SetWindowLongPtr(s_hwnd, GWLP_WNDPROC, (LONG_PTR)s_origWndProc);
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        s_imguiReady = false;
    }

    MH_Uninitialize();
    s_installed = false;
}

bool installed() { return s_installed; }

} // namespace vc::client::d3d9
