// ============================================================
//  VoiceChat client (.asi) — audio playback (WASAPI + Opus)
//  File: client/include/audio_playback.h
//
//  Hybrid client mixing (req. D): proximity voice arrives per-speaker with a
//  server-computed volume + pan; the client decodes, spatialises (stereo pan,
//  occlusion low-pass) and mixes it locally for lowest latency. Bus streams
//  (radio/phone) are already filtered+mixed server-side — just decode and play
//  at the category volume. Each stream has its own decoder + jitter buffer.
// ============================================================
#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;

namespace vc::client {

class AudioPlayback {
public:
    bool init(float masterVol, float proximityVol, float radioVol);
    void shutdown();

    // One audible speaker's proximity frame (client spatialises + mixes).
    void pushProximity(uint16_t speaker, uint16_t seq, uint8_t flags,
                       float volume, float pan, const uint8_t* opus, uint16_t len);
    // A pre-filtered/mixed channel bus frame (play at category volume).
    void pushBus(uint16_t channel, uint8_t filter, uint16_t seq,
                 float volume, const uint8_t* opus, uint16_t len);
    void removeSpeaker(uint16_t speaker);

    // --- live controls (driven by the ImGui control panel) ---
    void setMasterVolume(float v) { m_master.store(v); }
    void setDeafen(bool d)        { m_deafen.store(d); }   // silence all output

    // Per-speaker LOCAL mute + volume (client-side mix only; never tells server).
    void  setSpeakerMute(uint16_t speaker, bool muted);
    bool  isSpeakerMuted(uint16_t speaker);
    void  setSpeakerVolume(uint16_t speaker, float vol);   // 0..2
    float getSpeakerVolume(uint16_t speaker);

    // Proximity speakers with a live stream right now (for the panel's list).
    std::vector<uint16_t> activeSpeakers();

    // Switch the render endpoint live (combo selection). Empty id -> default.
    // Falls back to the default device if the chosen one fails.
    bool restartOnDevice(const std::wstring& deviceId);

private:
    struct Frame {
        uint16_t seq = 0, len = 0;
        uint8_t  data[1276];
        float    volume = 1.f, pan = 0.f;
        bool     occluded = false, isBus = false;
    };
    struct Stream {
        void*  decoder = nullptr;     // OpusDecoder*
        std::deque<Frame> jitter;
        float  lpf = 0.f;             // occlusion low-pass state
        bool   isBus = false;
        // Adaptive jitter buffer: `target` is the depth we prebuffer to before
        // playing; it grows on underruns (network jitter/loss) and slowly shrinks
        // when the link is clean (to cut latency). `playing` gates the initial
        // prebuffer; `starve`/`stable` count consecutive under/healthy ticks.
        int    target  = 3;
        bool   playing = false;
        int    starve  = 0;
        int    stable  = 0;
    };

    void  loop();
    Stream& get(uint32_t id, bool isBus);
    bool  openAndStart(IMMDevice* dev);   // dev==nullptr -> default endpoint
    void  stopStream();                   // tear down the WASAPI client + thread

    IMMDeviceEnumerator* m_enum   = nullptr;
    IMMDevice*           m_device = nullptr;
    IAudioClient*        m_client = nullptr;
    IAudioRenderClient*  m_render = nullptr;

    std::unordered_map<uint32_t, Stream> m_streams;
    std::unordered_map<uint16_t, float>  m_spkVol;    // per-speaker gain (default 1)
    std::unordered_set<uint16_t>         m_spkMute;   // locally muted speakers
    std::mutex          m_mtx;
    std::thread         m_thread;
    std::atomic<bool>   m_run{ false };

    std::atomic<float>  m_master{ 1.f };
    std::atomic<bool>   m_deafen{ false };
    float m_proximity = 1.f, m_radio = 1.f;
};

} // namespace vc::client
