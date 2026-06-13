// ============================================================
//  VoiceChat core — Opus codec wrapper implementation
//  File: core/src/opus_codec.cpp
//
//  Guarded by VC_WITH_OPUS so the skeleton links even before libopus is
//  fetched. Stage E flips it on for the relay bus mixer.
// ============================================================
#include "../include/opus_codec.h"

#ifdef VC_WITH_OPUS
#  if __has_include(<opus/opus.h>)
#    include <opus/opus.h>          // system / vcpkg layout
#  else
#    include <opus.h>               // FetchContent (xiph/opus) build tree
#  endif
#endif

namespace vc::core {

bool OpusDecoderWrap::init(int sr, int ch)
{
#ifdef VC_WITH_OPUS
    int err = 0;
    m_dec = opus_decoder_create(sr, ch, &err);
    return err == OPUS_OK && m_dec;
#else
    (void)sr; (void)ch; return false;   // codec not built yet (skeleton)
#endif
}

OpusDecoderWrap::~OpusDecoderWrap()
{
#ifdef VC_WITH_OPUS
    if (m_dec) opus_decoder_destroy((OpusDecoder*)m_dec);
#endif
}

int OpusDecoderWrap::decode(const uint8_t* opus, int len, float* pcm, int maxSamples)
{
#ifdef VC_WITH_OPUS
    if (!m_dec) return 0;
    int n = opus_decode_float((OpusDecoder*)m_dec, opus, len, pcm, maxSamples, 0);
    return n > 0 ? n : 0;
#else
    (void)opus; (void)len; (void)pcm; (void)maxSamples; return 0;
#endif
}

bool OpusEncoderWrap::init(int sr, int ch, int bitrate)
{
#ifdef VC_WITH_OPUS
    int err = 0;
    m_enc = opus_encoder_create(sr, ch, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !m_enc) return false;
    opus_encoder_ctl((OpusEncoder*)m_enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl((OpusEncoder*)m_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    return true;
#else
    (void)sr; (void)ch; (void)bitrate; return false;
#endif
}

OpusEncoderWrap::~OpusEncoderWrap()
{
#ifdef VC_WITH_OPUS
    if (m_enc) opus_encoder_destroy((OpusEncoder*)m_enc);
#endif
}

int OpusEncoderWrap::encode(const float* pcm, int samples, uint8_t* out, int maxBytes)
{
#ifdef VC_WITH_OPUS
    if (!m_enc) return 0;
    int n = opus_encode_float((OpusEncoder*)m_enc, pcm, samples, out, maxBytes);
    return n > 0 ? n : 0;
#else
    (void)pcm; (void)samples; (void)out; (void)maxBytes; return 0;
#endif
}

} // namespace vc::core
