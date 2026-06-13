# open.mp component — second bridge over the same vc_core.
# open.mp server is 64-bit. Ships in components/.
add_library(VoiceChatOmp MODULE src/component.cpp)
target_include_directories(VoiceChatOmp PRIVATE include)
target_link_libraries(VoiceChatOmp PRIVATE vc_core)
set_target_properties(VoiceChatOmp PROPERTIES
    PREFIX "" OUTPUT_NAME "VoiceChat")

if(VC_WITH_OMP_SDK)
    target_compile_definitions(VoiceChatOmp PRIVATE VC_WITH_OMP_SDK)
    target_include_directories(VoiceChatOmp PRIVATE sdk)   # git clone open.mp-sdk -> omp/sdk
    # NOTE: only the PUBLIC component API is used; never internal headers
    # of other components (prompt §4 — internals aren't stable).
else()
    message(STATUS "omp: building inert stub (VC_WITH_OMP_SDK=OFF). See omp/README.md")
endif()
