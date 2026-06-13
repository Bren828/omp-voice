# Stage-A verification tests (opt-in: -DVC_BUILD_TESTS=ON)
add_executable(vc_test_crypto crypto_roundtrip.cpp)
target_link_libraries(vc_test_crypto PRIVATE vc_shared)
add_test(NAME crypto_roundtrip COMMAND vc_test_crypto)

# Stage-E: DSP filters + mix policy (no Opus needed).
add_executable(vc_test_dsp dsp_test.cpp)
target_link_libraries(vc_test_dsp PRIVATE vc_core)
add_test(NAME dsp COMMAND vc_test_dsp)

# Stage-F: incremental spatial grid + nearest-N cap + scale benchmark.
add_executable(vc_test_scale scale_bench.cpp)
target_link_libraries(vc_test_scale PRIVATE vc_core)
add_test(NAME scale COMMAND vc_test_scale)

# e2e needs the relay library (only when the relay is built).
if(VC_BUILD_RELAY)
    add_executable(vc_test_e2e e2e_handshake.cpp)
    target_link_libraries(vc_test_e2e PRIVATE vc_relay)
    add_test(NAME e2e_handshake COMMAND vc_test_e2e)

    # Stage B: relay -> bridge status channel (drives the Pawn callbacks).
    add_executable(vc_test_status e2e_status.cpp)
    target_link_libraries(vc_test_status PRIVATE vc_relay vc_core)
    add_test(NAME e2e_status COMMAND vc_test_status)

    # Stage H: .dll pushes voice.ini config -> relay -> reflected in the Ack limits.
    add_executable(vc_test_config e2e_config.cpp)
    target_link_libraries(vc_test_config PRIVATE vc_relay vc_core)
    add_test(NAME e2e_config COMMAND vc_test_config)
endif()
