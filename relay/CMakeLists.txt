# relay — transport + crypto shell around vc_core.
# Split into a static lib (so tests can drive it) + a thin exe.
add_library(vc_relay STATIC src/relay.cpp)
target_include_directories(vc_relay PUBLIC include)
target_link_libraries(vc_relay PUBLIC vc_core)

add_executable(voice_relay src/main.cpp)
target_link_libraries(voice_relay PRIVATE vc_relay)
set_target_properties(voice_relay PROPERTIES OUTPUT_NAME "voice_relay")

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(vc_relay PUBLIC Threads::Threads)
endif()
