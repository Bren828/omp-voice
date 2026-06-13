# SA-MP 0.3.7 R5 plugin — classic 5-export model.
# SA-MP server is 32-bit: build this target x86 (Win32 / Linux -m32).
option(VC_SAMP_M32 "Build the SA-MP plugin 32-bit (required by SA-MP server)" ON)
# Opt-in: deliver the auth token over RakNet instead of cmd-UDP. Needs the
# build-specific RakServer ABI + a client receive hook — see samp/raknet/README.md.
option(VC_SAMP_RAKNET "Token delivery over RakNet (opt-in hardening)" OFF)

add_library(VoiceChatSamp MODULE
    src/plugin.cpp
    sdk/amxplugin.cpp)            # classic SDK amx_* thunks
target_include_directories(VoiceChatSamp PRIVATE include sdk)
target_link_libraries(VoiceChatSamp PRIVATE vc_core)

if(VC_SAMP_RAKNET)
    target_compile_definitions(VoiceChatSamp PRIVATE VC_SAMP_RAKNET)
endif()

# Output: VoiceChat.dll / VoiceChat.so (no 'lib' prefix), goes in plugins/
set_target_properties(VoiceChatSamp PROPERTIES
    PREFIX "" OUTPUT_NAME "VoiceChat")

if(WIN32)
    # Export only the 5 required symbols via the .def.
    set_target_properties(VoiceChatSamp PROPERTIES
        LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/voicechat.def")
elseif(VC_SAMP_M32)
    target_compile_options(VoiceChatSamp PRIVATE -m32)
    target_link_options(VoiceChatSamp PRIVATE -m32)
    # prompt §4: prefer the standard (non-static) build on Linux.
endif()
