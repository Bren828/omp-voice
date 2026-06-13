// ============================================================
//  VoiceChat client — microphone capture implementation
//  File: client/src/audio_capture.cpp
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "../include/audio_capture.h"
#include "../../shared/protocol.h"

#if __has_include(<opus/opus.h>)
#  include <opus/opus.h>
#else
#  include <opus.h>
#endif

#ifdef VC_WITH_RNNOISE
#  include "rnnoise.h"
#  include <cmath>
#endif

namespace vc::client {

// Opus always runs at the wire rate (24 kHz). WASAPI captures at CAP_RATE: when
// RNNoise is built in we capture at 48 kHz (RNNoise's native rate) and decimate
// 2:1 down to 24 kHz after denoising; otherwise we capture at 24 kHz directly.
static constexpr int OPUS_RATE = CLIENT_SAMPLE_RATE;                    // 24000
static constexpr int CAP_FRAME = CLIENT_SAMPLE_RATE * FRAME_MS / 1000;  // 480 @24k

#ifdef VC_WITH_RNNOISE
static constexpr int CAP_RATE = 48000;
static constexpr int RN_FRAME = 480;   // RNNoise frame = 10 ms @ 48 kHz

// 2:1 anti-aliasing decimator (48 kHz -> 24 kHz). Windowed-sinc FIR low-pass
// (cutoff ~10.8 kHz, below the 12 kHz target Nyquist) + drop every other sample.
// Keeps inter-frame history so block boundaries are seamless.
struct Decimator2 {
    static constexpr int NTAPS = 33;
    float h[NTAPS];
    float hist[NTAPS - 1];     // in[-1], in[-2], ... (most-recent first)

    Decimator2() {
        constexpr double PI = 3.14159265358979323846;
        const double M = NTAPS - 1, wc = 0.225;     // 10.8kHz / 48kHz
        double sum = 0;
        for (int k = 0; k < NTAPS; ++k) {
            double m = k - M / 2.0;
            double sinc = (m == 0.0) ? 2.0 * wc
                                     : std::sin(2.0 * PI * wc * m) / (PI * m);
            double win  = 0.5 - 0.5 * std::cos(2.0 * PI * k / M);   // Hann
            h[k] = (float)(sinc * win);
            sum += h[k];
        }
        for (int k = 0; k < NTAPS; ++k) h[k] /= (float)sum;          // unity DC
        for (int k = 0; k < NTAPS - 1; ++k) hist[k] = 0.f;
    }
    // nIn must be even; writes nIn/2 samples to out.
    void process(const float* in, int nIn, float* out) {
        auto at = [&](int i) -> float { return i >= 0 ? in[i] : hist[-i - 1]; };
        for (int j = 0; j < nIn / 2; ++j) {
            float acc = 0.f;
            const int n0 = 2 * j;
            for (int k = 0; k < NTAPS; ++k) acc += h[k] * at(n0 - k);
            out[j] = acc;
        }
        for (int k = 0; k < NTAPS - 1; ++k) hist[k] = in[nIn - 1 - k];
    }
};
#else
static constexpr int CAP_RATE = CLIENT_SAMPLE_RATE;   // 24000
#endif

bool AudioCapture::init(float micVolume, bool vadEnabled, float vadThreshold,
                        bool noiseSuppress)
{
    m_micVolume.store(micVolume); m_vadEnabled.store(vadEnabled);
    m_vadThreshold.store(vadThreshold); m_denoise.store(noiseSuppress);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&m_enum);
    if (FAILED(hr)) return false;

    int err = 0;
    OpusEncoder* enc = opus_encoder_create(OPUS_RATE, VOICE_CHANNELS, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc) { printf("[capture] opus encoder %s\n", opus_strerror(err)); return false; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    m_encoder = enc;

#ifdef VC_WITH_RNNOISE
    m_rnnoise = rnnoise_create();
    if (!m_rnnoise) printf("[capture] RNNoise create failed — running without it\n");
    else            printf("[capture] RNNoise ready (48k denoise -> 24k)\n");
#endif

    return openAndStart(nullptr);
}

// Open a capture endpoint (dev==nullptr -> default) and spin the encode thread.
// Assumes m_enum + m_encoder are live and no stream is currently running.
bool AudioCapture::openAndStart(IMMDevice* dev)
{
    HRESULT hr;
    if (dev) { m_device = dev; }            // take ownership of caller's ref
    else {
        hr = m_enum->GetDefaultAudioEndpoint(eCapture, eCommunications, &m_device);
        if (FAILED(hr)) { printf("[capture] no microphone\n"); return false; }
    }
    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_client);
    if (FAILED(hr)) { m_device->Release(); m_device = nullptr; return false; }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = VOICE_CHANNELS;       // mono
    fmt.nSamplesPerSec  = CAP_RATE;             // 24 kHz
    fmt.wBitsPerSample  = 32;                   // float
    fmt.nBlockAlign     = fmt.nChannels * (fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    // AUTOCONVERTPCM lets Windows resample the native device format (often
    // 48 kHz stereo) down to our 24 kHz mono.
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            10'000'000, 0, &fmt, nullptr);
    if (FAILED(hr)) { printf("[capture] Initialize failed 0x%lx\n", hr); return false; }

    hr = m_client->GetService(__uuidof(IAudioCaptureClient), (void**)&m_capture);
    if (FAILED(hr)) return false;

    m_run = true;
    m_thread = std::thread([this] { loop(); });
    printf("[capture] WASAPI + Opus ready (%d Hz mono)\n", CAP_RATE);
    return true;
}

// Stop the encode thread and release the WASAPI client; keeps m_enum + encoder.
void AudioCapture::stopStream()
{
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_capture) { m_capture->Release(); m_capture = nullptr; }
    if (m_client)  { m_client->Release();  m_client  = nullptr; }
    if (m_device)  { m_device->Release();  m_device  = nullptr; }
}

bool AudioCapture::restartOnDevice(const std::wstring& deviceId)
{
    if (!m_enum) return false;
    stopStream();
    IMMDevice* dev = nullptr;
    if (!deviceId.empty())
        m_enum->GetDevice(deviceId.c_str(), &dev);   // null on failure -> default
    bool ok = openAndStart(dev);
    if (!ok && dev) {                                 // chosen device failed: default
        printf("[capture] device switch failed; falling back to default\n");
        ok = openAndStart(nullptr);
    }
    return ok;
}

void AudioCapture::loop()
{
    m_client->Start();
    auto* enc = (OpusEncoder*)m_encoder;
    float    pcm[CAP_FRAME];        // 24 kHz Opus-frame accumulator (480 samples)
    int      have = 0;
    uint16_t seq = 0;
    uint8_t  opus[MAX_OPUS_BYTES];

    // Gate + encode + send one finished 24 kHz frame in `pcm`.
    // The mode is EXCLUSIVE (set by the panel's "Voice mode"):
    //   VAD mode  -> transmit only when the mic level passes the threshold;
    //                the PTT key is IGNORED (true open-mic).
    //   PTT mode  -> transmit only while a PTT key is held; no auto-open.
    auto emit = [&] {
        if (m_muted.load()) return;          // self-mute blocks everything
        bool ptt = false, vadOpen = false;
        if (m_vadEnabled.load()) {
            double e = 0; for (int i = 0; i < CAP_FRAME; ++i) e += (double)pcm[i] * pcm[i];
            vadOpen = std::sqrt(e / CAP_FRAME) > m_vadThreshold.load();
            if (!vadOpen) return;            // VAD closed -> drop frame
        } else {
            ptt = m_ptt.load();
            if (!ptt) return;                // PTT key up -> drop frame
        }
        int bytes = opus_encode_float(enc, pcm, CAP_FRAME, opus, MAX_OPUS_BYTES);
        if (bytes > 0 && m_sink) {
            uint8_t fl = (uint8_t)((ptt ? AF_PTT : 0) | (vadOpen ? AF_VAD_ACTIVE : 0));
            m_sink(seq++, fl, opus, (uint16_t)bytes);
        }
    };

#ifdef VC_WITH_RNNOISE
    auto*     den = (DenoiseState*)m_rnnoise;
    Decimator2 decim;
    float     rn48[RN_FRAME];       // 48 kHz RNNoise-frame accumulator (480 = 10 ms)
    int       rnHave = 0;
    float     dec24[RN_FRAME / 2];  // decimated output (240 @ 24 kHz)
#endif

    while (m_run) {
        UINT32 pkt = 0;
        if (FAILED(m_capture->GetNextPacketSize(&pkt)) || pkt == 0) { Sleep(2); continue; }

        BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
        if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) continue;

        const float* in = reinterpret_cast<const float*>(data);
        bool  silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        float micVol = m_micVolume.load();
        int   nin    = (int)frames;

#ifdef VC_WITH_RNNOISE
        // 48 kHz -> RNNoise (10 ms frames) -> decimate 2:1 -> 24 kHz Opus frames.
        for (int s = 0; s < nin; ++s) {
            rn48[rnHave++] = silent ? 0.f : in[s] * micVol;
            if (rnHave < RN_FRAME) continue;
            rnHave = 0;

            if (den && m_denoise.load()) {
                // RNNoise works in int16-magnitude floats (~ +/-32768).
                for (int k = 0; k < RN_FRAME; ++k) rn48[k] *= 32768.f;
                rnnoise_process_frame(den, rn48, rn48);
                for (int k = 0; k < RN_FRAME; ++k) rn48[k] *= (1.f / 32768.f);
            }
            decim.process(rn48, RN_FRAME, dec24);
            for (int k = 0; k < RN_FRAME / 2; ++k) {
                pcm[have++] = dec24[k];
                if (have < CAP_FRAME) continue;
                have = 0; emit();
            }
        }
#else
        // 24 kHz direct path (no RNNoise build).
        int idx = 0;
        while (nin > 0) {
            int take = std::min(nin, CAP_FRAME - have);
            for (int i = 0; i < take; ++i)
                pcm[have + i] = silent ? 0.f : in[idx + i] * micVol;
            have += take; idx += take; nin -= take;
            if (have < CAP_FRAME) continue;
            have = 0; emit();
        }
#endif
        m_capture->ReleaseBuffer(frames);
    }
    m_client->Stop();
}

void AudioCapture::shutdown()
{
    stopStream();
    if (m_encoder) { opus_encoder_destroy((OpusEncoder*)m_encoder); m_encoder = nullptr; }
#ifdef VC_WITH_RNNOISE
    if (m_rnnoise) { rnnoise_destroy((DenoiseState*)m_rnnoise); m_rnnoise = nullptr; }
#endif
    if (m_enum)    { m_enum->Release(); m_enum = nullptr; }
}

} // namespace vc::client
