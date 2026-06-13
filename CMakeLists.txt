# ============================================================
#  VoiceChat — root build
#  Single tree, dual platform (open.mp + SA-MP), Win + Linux.
#
#  Bitness matters and differs per target:
#    * client (.asi)  -> x86 (GTA SA is 32-bit)               [own build]
#    * samp plugin    -> x86 (SA-MP 0.3.7 server is 32-bit)
#    * omp component  -> x64 (open.mp server is 64-bit)
#    * relay (.exe)   -> native (x64 fine; standalone process)
#  Because cross-bitness in one configure is painful, build the server
#  pieces you need per configure. See docs/BUILD.md.
# ============================================================
cmake_minimum_required(VERSION 3.16)
project(VoiceChat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # Linux .so: standard, non-static (per prompt §4)

# ── Options ─────────────────────────────────────────────────
option(VC_BUILD_RELAY  "Build the standalone voice relay (.exe/ELF)" ON)
option(VC_BUILD_SAMP   "Build the SA-MP 0.3.7 R5 plugin"             ON)
option(VC_BUILD_OMP    "Build the open.mp component"                 OFF)
option(VC_BUILD_CLIENT "Build the .asi client (Windows x86 only)"    OFF)
option(VC_WITH_OMP_SDK "open.mp SDK present in omp/sdk"              OFF)
option(VC_WITH_OPUS    "Link libopus (relay bus mixing, stage E)"   OFF)
option(VC_OPUS_FETCH   "If libopus isn't installed, fetch+build it (FetchContent)" ON)
option(VC_NO_SODIUM    "Compile crypto as PASS-THROUGH (NO security; testing only)" ON)
option(VC_SODIUM_FETCH "If libsodium isn't installed, fetch+build it (FetchContent)" ON)
option(VC_BUILD_TESTS  "Build the verification tests"                ON)

if(VC_NO_SODIUM)
    message(WARNING "VC_NO_SODIUM=ON: encryption is DISABLED (pass-through). "
                    "Set -DVC_NO_SODIUM=OFF and provide libsodium for production.")
endif()

# ── shared (header-only + crypto.cpp) ───────────────────────
add_library(vc_shared STATIC shared/crypto.cpp)
target_include_directories(vc_shared PUBLIC shared)
if(VC_NO_SODIUM)
    target_compile_definitions(vc_shared PUBLIC VC_NO_SODIUM)
else()
    # 1) Prefer a system / vcpkg libsodium.
    find_library(SODIUM_LIB NAMES sodium libsodium)
    find_path(SODIUM_INC sodium.h)
    if(SODIUM_LIB AND SODIUM_INC)
        message(STATUS "libsodium: system (${SODIUM_LIB})")
        target_include_directories(vc_shared PUBLIC ${SODIUM_INC})
        target_link_libraries(vc_shared PUBLIC ${SODIUM_LIB})
    elseif(VC_SODIUM_FETCH)
        # 2) Otherwise fetch + build it portably (Windows + Linux).
        message(STATUS "libsodium: fetching via libsodium-cmake")
        include(FetchContent)
        set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
        FetchContent_Declare(Sodium
            GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
            GIT_TAG        9b2848dfc1b917a9410f0de9d81059b26cbfaa8d)
        FetchContent_MakeAvailable(Sodium)
        target_link_libraries(vc_shared PUBLIC sodium)
    else()
        message(FATAL_ERROR "libsodium not found and VC_SODIUM_FETCH=OFF. "
                            "Install it (vcpkg: 'vcpkg install libsodium'; "
                            "apt: 'libsodium-dev') or set -DVC_NO_SODIUM=ON.")
    endif()
endif()
if(WIN32)
    target_link_libraries(vc_shared PUBLIC ws2_32)
endif()

# ── libopus (shared by the relay bus mixer AND the .asi capture/playback) ──
# Resolved once into an INTERFACE target `vc_opus`. The relay/core uses it for
# the filtered-bus mix (stage E); the client uses it for mic encode + playback.
if(VC_WITH_OPUS OR VC_BUILD_CLIENT)
    add_library(vc_opus INTERFACE)
    find_library(OPUS_LIB NAMES opus)
    find_path(OPUS_INC NAMES opus/opus.h opus.h)
    if(OPUS_LIB AND OPUS_INC)
        message(STATUS "libopus: system (${OPUS_LIB})")
        target_include_directories(vc_opus INTERFACE ${OPUS_INC})
        target_link_libraries(vc_opus INTERFACE ${OPUS_LIB})
    elseif(VC_OPUS_FETCH)
        message(STATUS "libopus: fetching via xiph/opus")
        include(FetchContent)
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING        OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS       OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(Opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG        v1.5.2)
        FetchContent_MakeAvailable(Opus)
        target_link_libraries(vc_opus INTERFACE opus)
    else()
        message(FATAL_ERROR "libopus not found and VC_OPUS_FETCH=OFF. Install it "
                            "(vcpkg: 'vcpkg install opus'; apt: 'libopus-dev') "
                            "or set -DVC_WITH_OPUS=OFF and -DVC_BUILD_CLIENT=OFF.")
    endif()
endif()

# ── core (platform-independent engine) ──────────────────────
add_library(vc_core STATIC
    core/src/registry.cpp
    core/src/spatial_grid.cpp
    core/src/proximity.cpp
    core/src/dsp.cpp
    core/src/mixer.cpp
    core/src/bus_router.cpp
    core/src/engine.cpp
    core/src/control_link.cpp
    core/src/opus_codec.cpp)
target_include_directories(vc_core PUBLIC core/include shared)
target_link_libraries(vc_core PUBLIC vc_shared)
if(VC_WITH_OPUS)
    target_compile_definitions(vc_core PUBLIC VC_WITH_OPUS)
    target_link_libraries(vc_core PUBLIC vc_opus)
endif()

# ── subdirectories ──────────────────────────────────────────
if(VC_BUILD_RELAY)
    add_subdirectory(relay)
endif()
if(VC_BUILD_SAMP)
    add_subdirectory(samp)
endif()
if(VC_BUILD_OMP)
    add_subdirectory(omp)
endif()
if(VC_BUILD_CLIENT)
    add_subdirectory(client)
endif()
if(VC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
