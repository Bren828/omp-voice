// ============================================================
//  VoiceChat client (.asi) — WASAPI endpoint enumeration
//  File: client/include/audio_devices.h
//
//  Lists the active capture (microphone) / render (output) endpoints so the
//  ImGui control panel can show them in the device combos. The wide `id` is
//  the stable endpoint id passed back to AudioCapture/AudioPlayback to switch
//  the live device; `name` is the human-friendly label for the UI.
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace vc::client {

struct AudioDevice {
    std::wstring id;     // endpoint id (stable; -> restartOnDevice)
    std::string  name;   // friendly name (UTF-8, for the combo)
};

// Enumerate active endpoints. capture=true -> microphones (eCapture),
// false -> outputs (eRender). Safe to call any time; returns {} on failure.
std::vector<AudioDevice> enumerateDevices(bool capture);

} // namespace vc::client
