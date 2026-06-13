// ============================================================
//  VoiceChat client — audio playback implementation (stereo)
//  File: client/src/audio_playback.cpp
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "../include/audio_playback.h"
#include "../../shared/protocol.h"

#if __has_include(<opus/opus.h>)
#  include <opus/opus.h>
#else
#  include <opus.h>
#endif

namespace vc::client {

static constexpr int PB_RATE   = CLIENT_SAMPLE_RATE;                    // 24000
static constexpr int PB_FRAME  = CLIENT_SAMPLE_RATE * FRAME_MS / 1000;  // 480 samples/ch
static constexpr int PB_CH     = 2;                                    // stereo (for pan)

// Adaptive jitter buffer bounds (frames; 1 frame = 20 ms).
static constexpr int JITTER_MIN    = 2;     // floor (~40 ms) on a clean link
static constexpr int JITTER_MAX    = 10;    // ceiling (~200 ms) under heavy jitter
static constexpr int STARVE_RESET  = 10;    // underrun ticks before forcing re-prebuffer
static constexpr int STABLE_SHRINK = 250;   // healthy ticks (~5 s) before shrinking target
static constexpr int JITTER_CAP    = 16;    // hard deque cap (> JITTER_MAX) to bound latency

static uint32_t proxId(uint16_t s) { return s; }
static uint32_t busId(uint16_t c)  { return 0x10000u | c; }

AudioPlayback::Stream& AudioPlayback::get(uint32_t id, bool isBus)
{
    Stream& s = m_streams[id];
    if (!s.decoder) {
        int err = 0;
        s.decoder = opus_decoder_create(PB_RATE, VOICE_CHANNELS, &err);  // mono in
        s.isBus = isBus;
    }
    return s;
}

bool AudioPlayback::init(float masterVol, float proximityVol, float radioVol)
{
    m_master.store(masterVol); m_proximity = proximityVol; m_radio = radioVol;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&m_enum);
    if (FAILED(hr)) return false;
    return openAndStart(nullptr);
}

// Open a render endpoint (dev==nullptr -> default) and spin the mix thread.
// Assumes m_enum is live and no stream is currently running.
bool AudioPlayback::openAndStart(IMMDevice* dev)
{
    HRESULT hr;
    if (dev) { m_device = dev; }            // take ownership of caller's ref
    else {
        hr = m_enum->GetDefaultAudioEndpoint(eRender, eCommunications, &m_device);
        if (FAILED(hr)) { printf("[playback] no output device\n"); return false; }
    }
    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_client);
    if (FAILED(hr)) { m_device->Release(); m_device = nullptr; return false; }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = PB_CH;                 // stereo out (pan)
    fmt.nSamplesPerSec  = PB_RATE;
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = fmt.nChannels * (fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            10'000'000, 0, &fmt, nullptr);
    if (FAILED(hr)) { printf("[playback] Initialize failed 0x%lx\n", hr); return false; }
    hr = m_client->GetService(__uuidof(IAudioRenderClient), (void**)&m_render);
    if (FAILED(hr)) return false;
    if (FAILED(m_client->Start())) return false;

    m_run = true;
    m_thread = std::thread([this] { loop(); });
    printf("[playback] WASAPI render ready (%d Hz stereo)\n", PB_RATE);
    return true;
}

// Stop the mix thread and release the WASAPI client; keeps m_enum + streams.
void AudioPlayback::stopStream()
{
    m_run = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_client) { m_client->Stop(); m_client->Release(); m_client = nullptr; }
    if (m_render) { m_render->Release(); m_render = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}

bool AudioPlayback::restartOnDevice(const std::wstring& deviceId)
{
    if (!m_enum) return false;
    stopStream();
    IMMDevice* dev = nullptr;
    if (!deviceId.empty())
        m_enum->GetDevice(deviceId.c_str(), &dev);   // null on failure -> default
    bool ok = openAndStart(dev);
    if (!ok && dev) {                                 // chosen device failed: default
        printf("[playback] device switch failed; falling back to default\n");
        ok = openAndStart(nullptr);
    }
    return ok;
}

void AudioPlayback::loop()
{
    float mix[PB_FRAME * PB_CH];
    float mono[2880];

    while (m_run) {
        Sleep(FRAME_MS);

        UINT32 bufFrames = 0, padFrames = 0;
        m_client->GetBufferSize(&bufFrames);
        m_client->GetCurrentPadding(&padFrames);
        if (bufFrames - padFrames < (UINT32)PB_FRAME) continue;

        std::memset(mix, 0, sizeof(mix));
        const float master = m_master.load();
        const bool  deafen = m_deafen.load();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            for (auto& [id, st] : m_streams) {
                if (!st.decoder) continue;
                // Adaptive jitter buffer: prebuffer to `target`, then pop one
                // frame/tick. Underruns grow `target` (and after a sustained
                // starve force a re-prebuffer); a long healthy run shrinks it.
                // Empty -> Opus PLC (comfort), not mixed.
                Frame f; bool have = false;
                if (!st.playing && st.jitter.size() >= (size_t)st.target)
                    st.playing = true;
                if (st.playing) {
                    if (!st.jitter.empty()) {
                        f = st.jitter.front(); st.jitter.pop_front(); have = true;
                        st.starve = 0;
                        if ((int)st.jitter.size() + 1 >= st.target) {
                            if (++st.stable >= STABLE_SHRINK && st.target > JITTER_MIN)
                                { st.target--; st.stable = 0; }
                        } else st.stable = 0;
                    } else {
                        if (st.target < JITTER_MAX) st.target++;
                        st.stable = 0;
                        if (++st.starve >= STARVE_RESET) { st.playing = false; st.starve = 0; }
                    }
                }
                int samples = 0;
                if (have)
                    samples = opus_decode_float((OpusDecoder*)st.decoder, f.data, f.len,
                                                mono, 2880, 0);
                else { opus_decode_float((OpusDecoder*)st.decoder, nullptr, 0, mono, PB_FRAME, 0); continue; }
                if (samples <= 0) continue;

                // Per-speaker LOCAL mute/volume (proximity streams only; bus
                // streams are channels, not single speakers). m_mtx is held.
                float spk = 1.f;
                if (!st.isBus) {
                    uint16_t sid = (uint16_t)id;
                    if (m_spkMute.count(sid)) continue;          // locally muted
                    auto vit = m_spkVol.find(sid);
                    if (vit != m_spkVol.end()) spk = vit->second;
                }

                float cat = (st.isBus ? m_radio : m_proximity) * master;
                // Occlusion: gentle one-pole low-pass (muffled through walls/cars).
                float gl, gr;
                {
                    float pan = std::clamp(f.pan, -1.f, 1.f);
                    float v = f.volume * cat * spk;
                    gl = v * (pan <= 0 ? 1.f : 1.f - pan);
                    gr = v * (pan >= 0 ? 1.f : 1.f + pan);
                }
                int nn = std::min(samples, PB_FRAME);
                for (int i = 0; i < nn; ++i) {
                    float s = mono[i];
                    if (f.occluded) { st.lpf += 0.25f * (s - st.lpf); s = st.lpf; }
                    mix[i * 2 + 0] += s * gl;
                    mix[i * 2 + 1] += s * gr;
                }
            }
        }

        if (deafen) std::memset(mix, 0, sizeof(mix));   // hear no one
        else for (int i = 0; i < PB_FRAME * PB_CH; ++i)
            mix[i] = std::clamp(mix[i], -1.f, 1.f);

        BYTE* out = nullptr;
        if (SUCCEEDED(m_render->GetBuffer(PB_FRAME, &out))) {
            std::memcpy(out, mix, sizeof(mix));
            m_render->ReleaseBuffer(PB_FRAME, 0);
        }
    }
}

void AudioPlayback::pushProximity(uint16_t speaker, uint16_t seq, uint8_t flags,
                                  float volume, float pan, const uint8_t* opus, uint16_t len)
{
    if (len > sizeof(Frame::data)) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    Stream& st = get(proxId(speaker), /*isBus=*/false);
    Frame f; f.seq = seq; f.len = len; f.volume = volume; f.pan = pan;
    f.occluded = (flags & AF_OCCLUDED) != 0; f.isBus = false;
    std::memcpy(f.data, opus, len);
    auto it = st.jitter.begin();
    while (it != st.jitter.end() && it->seq < seq) ++it;
    st.jitter.insert(it, f);
    if (st.jitter.size() > JITTER_CAP) st.jitter.pop_front();   // bound latency
}

void AudioPlayback::pushBus(uint16_t channel, uint8_t /*filter*/, uint16_t seq,
                            float volume, const uint8_t* opus, uint16_t len)
{
    if (len > sizeof(Frame::data)) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    Stream& st = get(busId(channel), /*isBus=*/true);
    Frame f; f.seq = seq; f.len = len; f.volume = volume; f.pan = 0.f; f.isBus = true;
    std::memcpy(f.data, opus, len);
    auto it = st.jitter.begin();
    while (it != st.jitter.end() && it->seq < seq) ++it;
    st.jitter.insert(it, f);
    if (st.jitter.size() > JITTER_CAP) st.jitter.pop_front();
}

void AudioPlayback::removeSpeaker(uint16_t speaker)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_streams.find(proxId(speaker));
    if (it != m_streams.end()) {
        if (it->second.decoder) opus_decoder_destroy((OpusDecoder*)it->second.decoder);
        m_streams.erase(it);
    }
}

// --- per-speaker LOCAL mute / volume (panel) ---
void AudioPlayback::setSpeakerMute(uint16_t speaker, bool muted)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (muted) m_spkMute.insert(speaker); else m_spkMute.erase(speaker);
}

bool AudioPlayback::isSpeakerMuted(uint16_t speaker)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_spkMute.count(speaker) != 0;
}

void AudioPlayback::setSpeakerVolume(uint16_t speaker, float vol)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_spkVol[speaker] = vol;
}

float AudioPlayback::getSpeakerVolume(uint16_t speaker)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_spkVol.find(speaker);
    return it != m_spkVol.end() ? it->second : 1.f;
}

std::vector<uint16_t> AudioPlayback::activeSpeakers()
{
    std::vector<uint16_t> out;
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& [id, st] : m_streams)
        if (!st.isBus && id < 0x10000u) out.push_back((uint16_t)id);
    return out;
}

void AudioPlayback::shutdown()
{
    stopStream();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto& [id, st] : m_streams)
            if (st.decoder) opus_decoder_destroy((OpusDecoder*)st.decoder);
        m_streams.clear();
    }
    if (m_enum) { m_enum->Release(); m_enum = nullptr; }
}

} // namespace vc::client
