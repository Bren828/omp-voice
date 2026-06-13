// ============================================================
//  VoiceChat — Relay entry point (.exe / ELF)
//  File: relay/src/main.cpp
// ============================================================
#include "../include/relay.h"
#include <iostream>
#include <csignal>

static vc::Relay g_relay;

static void onSig(int) { g_relay.stop(); }

int main()
{
    std::cout << "==============================================\n";
    std::cout << "  VoiceChat Relay v2.0 (token + AEAD)\n";
    std::cout << "==============================================\n";

    std::signal(SIGINT,  onSig);
    std::signal(SIGTERM, onSig);

    // Read voice.ini ([network]/[security]/[proximity]); missing => defaults.
    vc::RelayConfig cfg = vc::RelayConfig::load("voice.ini");
    if (!g_relay.init(cfg))
        return 1;

    g_relay.run();
    std::cout << "[relay] stopped.\n";
    return 0;
}
