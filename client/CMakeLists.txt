# .asi client — Windows x86 ONLY (GTA SA is 32-bit).
# Full client: secure transport (token + AEAD) + WASAPI mic capture + Opus +
# stereo playback with pan/occlusion. Needs libopus (vc_opus) + a sodium build
# (VC_NO_SODIUM=OFF) to interop with the v2 server. Stage G2 overlay is a
# layered Win32/GDI window (overlay.cpp) — no D3D9/MinHook dependency.
#
# VC_CLIENT_GUI (default ON) adds the interactive Dear ImGui control panel
# ("Voice - setari") drawn over the game via a D3D9 EndScene hook (MinHook).
# This intentionally reintroduces D3D9/MinHook into the client; the GDI overlay
# stays for the passive HUD. ImGui + MinHook are fetched (like sodium/opus) and
# compiled from source here to avoid their bundled/old CMake scripts.
if(NOT WIN32)
    message(FATAL_ERROR "client (.asi) builds on Windows only")
endif()

option(VC_CLIENT_GUI "Build the ImGui (D3D9) control panel into the .asi" ON)
option(VC_WITH_RNNOISE "Build RNNoise neural noise suppression into the capture path" ON)

add_library(VoiceChatASI MODULE
    src/net_session.cpp         # secure session: token handshake + AEAD (A/B)
    src/voice_client.cpp        # token inbox + reconnect manager (B2)
    src/audio_capture.cpp       # WASAPI mic -> Opus -> sendAudio (C)
    src/audio_playback.cpp      # relay hooks -> Opus -> stereo render (C/D)
    src/overlay.cpp             # stage G2: layered Win32 "who's talking" HUD
    src/dllmain.cpp             # .asi entry: orchestrates the above
)
target_include_directories(VoiceChatASI PRIVATE include)
target_link_libraries(VoiceChatASI PRIVATE vc_shared vc_opus ole32 winmm user32 gdi32)
target_compile_definitions(VoiceChatASI PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
set_target_properties(VoiceChatASI PROPERTIES
    PREFIX "" SUFFIX ".asi" OUTPUT_NAME "VoiceChat")

# ── RNNoise (stage C noise suppression) ─────────────────────────────
# cpuimage/rnnoise: the classic Mozilla/Xiph RNNoise with the model committed
# (src/rnn_data.c) and a header-only FFT (stb_fft) — builds clean on MSVC x86,
# no autotools/model-download step. Fetched + compiled from source here (like
# imgui/minhook) to dodge the bundled build scripts. Operates at 48 kHz / 480-
# sample frames; the capture path resamples around it (see audio_capture.cpp).
if(VC_WITH_RNNOISE)
    include(FetchContent)
    FetchContent_Declare(rnnoise
        GIT_REPOSITORY https://github.com/cpuimage/rnnoise.git
        GIT_TAG        21af48b754fb39b4171da69e71ef5b3f3806a842)
    FetchContent_GetProperties(rnnoise)
    if(NOT rnnoise_POPULATED)
        FetchContent_Populate(rnnoise)
    endif()
    add_library(rnnoise STATIC
        ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
        ${rnnoise_SOURCE_DIR}/src/denoise.c
        ${rnnoise_SOURCE_DIR}/src/pitch.c
        ${rnnoise_SOURCE_DIR}/src/rnn.c
        ${rnnoise_SOURCE_DIR}/src/rnn_data.c)
    target_include_directories(rnnoise
        PUBLIC  ${rnnoise_SOURCE_DIR}/include
        PRIVATE ${rnnoise_SOURCE_DIR}/src)
    # M_PI for MSVC; do NOT define HAVE_CONFIG_H (no generated config.h exists).
    # RNNoise vendors its own copies of Opus's CELT pitch/LPC routines under the
    # SAME symbol names -> they collide with libopus at link time (LNK2005). The
    # calls + definitions all live inside RNNoise's own TUs, so renaming the
    # symbols here (rnn_*) is self-consistent and leaves libopus untouched.
    target_compile_definitions(rnnoise PRIVATE
        _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
        pitch_downsample=rnn_pitch_downsample
        pitch_search=rnn_pitch_search
        remove_doubling=rnn_remove_doubling
        find_best_pitch=rnn_find_best_pitch
        celt_pitch_xcorr=rnn_celt_pitch_xcorr
        celt_inner_prod=rnn_celt_inner_prod
        dual_inner_prod=rnn_dual_inner_prod
        xcorr_kernel=rnn_xcorr_kernel
        _celt_autocorr=rnn_celt_autocorr
        _celt_lpc=rnn_celt_lpc
        celt_fir=rnn_celt_fir
        celt_iir=rnn_celt_iir)
    target_link_libraries(VoiceChatASI PRIVATE rnnoise)
    target_compile_definitions(VoiceChatASI PRIVATE VC_WITH_RNNOISE)
endif()

# ── ImGui control panel (D3D9 + MinHook) ────────────────────────────
if(VC_CLIENT_GUI)
    include(FetchContent)

    # Dear ImGui (no CMakeLists upstream) -> build a static lib from source.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5)
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC d3d9 user32 gdi32 dwmapi imm32)

    # MinHook (bundled CMake targets an unsupported old policy) -> build from
    # source. Client is always x86, so the 32-bit length-disassembler is used.
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG        v1.3.3)
    FetchContent_GetProperties(minhook)
    if(NOT minhook_POPULATED)
        FetchContent_Populate(minhook)
    endif()
    add_library(minhook STATIC
        ${minhook_SOURCE_DIR}/src/buffer.c
        ${minhook_SOURCE_DIR}/src/hook.c
        ${minhook_SOURCE_DIR}/src/trampoline.c
        ${minhook_SOURCE_DIR}/src/hde/hde32.c)
    target_include_directories(minhook
        PUBLIC  ${minhook_SOURCE_DIR}/include
        PRIVATE ${minhook_SOURCE_DIR}/src ${minhook_SOURCE_DIR}/src/hde)

    target_sources(VoiceChatASI PRIVATE
        src/audio_devices.cpp   # WASAPI endpoint enumeration (combos)
        src/voice_panel.cpp     # the "Voice - setari" ImGui window
        src/d3d9_overlay.cpp)   # D3D9 EndScene/Reset hook + ImGui lifecycle
    target_link_libraries(VoiceChatASI PRIVATE imgui minhook d3d9 dinput8 dxguid)
    target_compile_definitions(VoiceChatASI PRIVATE VC_CLIENT_GUI)
endif()

# Reminder: configure this build with -A Win32.
