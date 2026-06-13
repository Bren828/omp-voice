// ============================================================
//  VoiceChat — Relay config (subset of voice.ini the relay needs)
//  File: relay/include/config.h
//
//  The relay reads voice.ini directly at boot for the values it needs
//  BEFORE any IPC exists (ports, security, proximity tuning) — these are the
//  authoritative DEFAULTS. Stage H (done): the .dll also reads voice.ini and
//  pushes the gameplay policy over CtrlConfig, which overrides these at runtime
//  and is reflected in subsequent handshake Acks. The boot read here is the
//  pre-IPC fallback; the .dll push is the authoritative gameplay source.
// ============================================================
#pragma once

#include <string>
#include "../../shared/ini.h"
#include "../../shared/protocol.h"

namespace vc {

struct RelayConfig {
    // [network]
    uint16_t controlPort = DEFAULT_PORT_CONTROL;
    uint16_t audioPort   = DEFAULT_PORT_AUDIO;
    int      maxPlayers  = 200;

    // [security]
    bool     encryptAudio    = true;   // honored only in a sodium build
    bool     antiReplay      = true;
    bool     ipSecondaryCheck= true;

    // [audio]
    int      bitrate = 24000;
    bool     normalize = true;
    float    limiterCeiling = 0.95f;

    // [proximity] (also used by stage D; free to load now)
    float    whisper = 8.f, normal = 18.f, shout = 40.f;
    float    falloffExp = 2.f;
    bool     occlusion = true;
    float    occludeAtten = 0.45f;
    float    panStrength = 0.8f;
    int      nearestN = 8;     // J9: max proximity speakers per listener (0=unlimited)

    // [radio] (stage E: filtered-bus DSP)
    float    bandpassLowHz = 300.f, bandpassHighHz = 3000.f;
    bool     squelchBeep = true;
    float    staticLevel = 0.05f;
    bool     halfDuplexGarble = true;

    // [behavior] (stage E mixing/safety)
    bool     suppressDoublePlay = true;   // J5
    bool     speakerphone = true;         // J6 server-wide enable (per-player via native)
    // mix policy default lives per-player (pushed via CtrlMixPolicy)

    // Client policy limits advertised in the handshake Ack (stage H). Sourced
    // from existing keys: [audio] vad_allowed/default_mode, [proximity]
    // max_streams_per_client.
    bool     vadAllowed = true;           // may clients use VAD vs PTT-only
    int      defaultMode = 0;             // 0 = PTT, 1 = VAD (server default)
    int      maxConcurrentStreams = 8;    // cap on simultaneous decoded streams

    // [logging]
    std::string logFile = "voice_relay.log";

    static RelayConfig load(const std::string& path = "voice.ini")
    {
        RelayConfig c;
        Ini ini;
        if (!ini.load(path)) {
            std::fprintf(stderr, "[voice.ini] not found — using zero-config defaults\n");
            return c;
        }
        c.controlPort = (uint16_t)ini.getInt("network", "control_port", c.controlPort, 1, 65535);
        c.audioPort   = (uint16_t)ini.getInt("network", "audio_port",   c.audioPort,   1, 65535);
        c.maxPlayers  = ini.getInt("network", "max_players", c.maxPlayers, 1, MAX_PLAYERS);

        c.encryptAudio     = ini.getBool("security", "encrypt_audio",     c.encryptAudio);
        c.antiReplay       = ini.getBool("security", "anti_replay",       c.antiReplay);
        c.ipSecondaryCheck = ini.getBool("security", "ip_secondary_check",c.ipSecondaryCheck);

        c.bitrate        = ini.getInt  ("audio", "bitrate", c.bitrate, 6000, 64000);
        c.normalize      = ini.getBool ("audio", "normalize", c.normalize);
        c.limiterCeiling = ini.getFloat("audio", "limiter_ceiling", c.limiterCeiling, 0.1f, 1.f);

        c.whisper    = ini.getFloat("proximity", "range_whisper", c.whisper, 0.5f, 1000.f);
        c.normal     = ini.getFloat("proximity", "range_normal",  c.normal,  0.5f, 1000.f);
        c.shout      = ini.getFloat("proximity", "range_shout",   c.shout,   0.5f, 1000.f);
        c.falloffExp = ini.getFloat("proximity", "falloff_exp",   c.falloffExp, 0.2f, 8.f);
        c.occlusion  = ini.getBool ("proximity", "occlusion",     c.occlusion);
        c.occludeAtten = ini.getFloat("proximity", "occlusion_atten", c.occludeAtten, 0.f, 1.f);
        c.panStrength  = ini.getFloat("proximity", "pan_strength",    c.panStrength,  0.f, 1.f);
        c.nearestN     = ini.getInt  ("proximity", "nearest_n",       c.nearestN, 0, 64);

        c.bandpassLowHz   = ini.getFloat("radio", "bandpass_low_hz",  c.bandpassLowHz,  20.f, 8000.f);
        c.bandpassHighHz  = ini.getFloat("radio", "bandpass_high_hz", c.bandpassHighHz, 100.f, 20000.f);
        c.squelchBeep     = ini.getBool ("radio", "squelch_beep",     c.squelchBeep);
        c.staticLevel     = ini.getFloat("radio", "static_level",     c.staticLevel, 0.f, 1.f);
        c.halfDuplexGarble= ini.getBool ("radio", "half_duplex_garble", c.halfDuplexGarble);

        c.suppressDoublePlay = ini.getBool("behavior", "suppress_double_play", c.suppressDoublePlay);
        c.speakerphone       = ini.getBool("behavior", "speakerphone",         c.speakerphone);

        // [policy] limits the client receives in the handshake Ack (stage H).
        // These reuse the EXISTING voice.ini keys (no duplicate section):
        // [audio] vad_allowed/default_mode + [proximity] max_streams_per_client.
        c.vadAllowed  = ini.getBool("audio", "vad_allowed", c.vadAllowed);
        std::string mode = ini.str("audio", "default_mode", c.defaultMode ? "vad" : "ptt");
        c.defaultMode = (mode == "vad" || mode == "1") ? 1 : 0;
        c.maxConcurrentStreams = ini.getInt("proximity", "max_streams_per_client",
                                            c.maxConcurrentStreams, 1, 32);

        c.logFile = ini.str("logging", "log_file", c.logFile);
        return c;
    }
};

} // namespace vc
