// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The microphone that goes to the PS5 (old Oviora: Ses ve ağ → Mikrofon, the stream menu's
// microphone tile). SDL3 records the chosen input device converted to what the console takes:
// 48 kHz, stereo, 16 bit, in 10 ms blocks of 480 frames, handed to a sink from SDL's audio thread.
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace oviora::remoteplay {
class Microphone {
public:
    using Sink = std::function<void(std::vector<std::int16_t>)>;
    Microphone() = default;
    Microphone(const Microphone&) = delete;
    Microphone& operator=(const Microphone&) = delete;
    ~Microphone();
    // deviceName: one of devices(); empty or not found: Windows' default recording device.
    bool start(const std::string& deviceName, Sink sink);
    void stop();
    bool running() const { return stream_ != nullptr; }
    // The device actually opened ("" for the default one).
    const std::string& openedDevice() const { return opened_; }
    static std::vector<std::string> devices();

private:
    static void callback(void* user, SDL_AudioStream* stream, int additional, int total);
    SDL_AudioStream* stream_ = nullptr;
    bool audioInitialized_ = false;
    std::string opened_;
    std::mutex sinkMutex_;
    Sink sink_;
};
} // namespace oviora::remoteplay
