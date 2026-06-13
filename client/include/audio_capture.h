// ============================================================
//  VoiceChat client (.asi) — microphone capture (WASAPI + Opus)
//  File: client/include/audio_capture.h
//
//  Captures the default communications mic at 24 kHz mono float, encodes
//  Opus frames and hands each to a sink (wired to VoiceClient::sendAudio).
//  Transmit gate is EXCLUSIVE by mode: PTT mode = only while a key is held;
//  VAD mode = only when the mic level passes the threshold (key ignored). The
//  server is authoritative for routing/volume — the client just sends its voice.
// ============================================================
#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <string>

struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;

namespace vc::client {

class AudioCapture {
public:
    using FrameFn = std::function<void(uint16_t seq, uint8_t flags,
                                       const uint8_t* opus, uint16_t len)>;

    bool init(float micVolume, bool vadEnabled, float vadThreshold,
              bool noiseSuppress = true);
    void shutdown();

    void setFrameSink(FrameFn fn) { m_sink = std::move(fn); }
    void setTransmit(bool on)     { m_ptt = on; }     // PTT held?
    void setMuted(bool m)         { m_muted = m; }

    // --- live controls (driven by the ImGui control panel) ---
    void setMicVolume(float v)    { m_micVolume.store(v); }
    void setVad(bool on)          { m_vadEnabled.store(on); }   // PTT vs VAD mode
    void setVadThreshold(float t) { m_vadThreshold.store(t); }  // 0..1 (scaled)
    void setNoiseSuppress(bool on){ m_denoise.store(on); }      // RNNoise on/off

    // Switch the capture endpoint live (combo selection). Empty id -> default.
    // Falls back to the default microphone if the chosen one fails.
    bool restartOnDevice(const std::wstring& deviceId);

private:
    void loop();
    bool openAndStart(IMMDevice* dev);   // dev==nullptr -> default microphone
    void stopStream();                   // tear down the WASAPI client + thread

    IMMDeviceEnumerator* m_enum    = nullptr;
    IMMDevice*           m_device  = nullptr;
    IAudioClient*        m_client  = nullptr;
    IAudioCaptureClient* m_capture = nullptr;
    void*                m_encoder = nullptr;   // OpusEncoder*
    void*                m_rnnoise = nullptr;   // DenoiseState* (RNNoise; null if off)

    std::thread          m_thread;
    std::atomic<bool>    m_run{ false };
    std::atomic<bool>    m_ptt{ false };
    std::atomic<bool>    m_muted{ false };

    std::atomic<float>   m_micVolume{ 1.0f };
    std::atomic<bool>    m_vadEnabled{ false };
    std::atomic<float>   m_vadThreshold{ 0.02f };
    std::atomic<bool>    m_denoise{ true };     // RNNoise noise suppression
    FrameFn m_sink;
};

} // namespace vc::client
