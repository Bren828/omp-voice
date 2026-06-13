// ============================================================
//  VoiceChat core — Opus codec wrapper (relay-side, req. C1)
//  File: core/include/opus_codec.h
//
//  Only the RELAY needs to decode/re-encode (for the filtered-bus mix in
//  the hybrid model). The client uses libopus directly in /client. This
//  thin wrapper keeps the relay free of raw libopus calls. Implemented in
//  stage E (decode for bus mixing) — declared now so the engine compiles.
// ============================================================
#pragma once

#include <cstdint>
#include <vector>
#include "../../shared/protocol.h"

namespace vc::core {

class OpusDecoderWrap {
public:
    bool init(int sampleRate = SAMPLE_RATE, int channels = VOICE_CHANNELS);
    ~OpusDecoderWrap();
    // Decode one frame to mono float PCM. Returns sample count, 0 on error.
    int decode(const uint8_t* opus, int len, float* pcm, int maxSamples);
private:
    void* m_dec = nullptr;  // OpusDecoder*
};

class OpusEncoderWrap {
public:
    bool init(int sampleRate = SAMPLE_RATE, int channels = VOICE_CHANNELS,
              int bitrate = 24000);
    ~OpusEncoderWrap();
    // Encode one mono float PCM frame. Returns byte count, 0 on error.
    int encode(const float* pcm, int samples, uint8_t* out, int maxBytes);
private:
    void* m_enc = nullptr;  // OpusEncoder*
};

} // namespace vc::core
