// SPDX-License-Identifier: GPL-3.0-only
#include "oviora/remoteplay/Microphone.h"
#include "oviora/Log.h"
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <format>

namespace oviora::remoteplay {
namespace {
constexpr int kFrames = 480;                       // 10 ms at 48 kHz
constexpr int kBlockBytes = kFrames * 2 * int(sizeof(std::int16_t));
}

Microphone::~Microphone() { stop(); }

std::vector<std::string> Microphone::devices() {
    std::vector<std::string> out;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        oviora::log::warn("remoteplay-mic", std::format("SDL audio init failed: {}", SDL_GetError()));
        return out;
    }
    int count = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count)) {
        for (int i = 0; i < count; ++i)
            if (const char* name = SDL_GetAudioDeviceName(ids[i]); name && *name) out.emplace_back(name);
        SDL_free(ids);
    }
    const char* driver = SDL_GetCurrentAudioDriver();
    oviora::log::info("remoteplay-mic", std::format("recording devices={} driver={}", out.size(), driver ? driver : "none"));
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return out;
}

bool Microphone::start(const std::string& deviceName, Sink sink) {
    stop();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        oviora::log::warn("remoteplay-mic", std::format("SDL audio init failed: {}", SDL_GetError()));
        return false;
    }
    audioInitialized_ = true;
    {
        std::lock_guard lock(sinkMutex_);
        sink_ = std::move(sink);
    }
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    opened_.clear();
    if (!deviceName.empty()) {
        int count = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count)) {
            for (int i = 0; i < count; ++i)
                if (const char* name = SDL_GetAudioDeviceName(ids[i]); name && deviceName == name) { device = ids[i]; opened_ = deviceName; break; }
            SDL_free(ids);
        }
        if (opened_.empty()) oviora::log::warn("remoteplay-mic", "chosen microphone not found; using the default one");
    }
    // SDL converts whatever the device records to this format.
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
    stream_ = SDL_OpenAudioDeviceStream(device, &spec, &Microphone::callback, this);
    if (!stream_ && device != SDL_AUDIO_DEVICE_DEFAULT_RECORDING) {
        oviora::log::warn("remoteplay-mic", std::format("chosen microphone failed ({}); using the default one", SDL_GetError()));
        opened_.clear();
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, &Microphone::callback, this);
    }
    if (!stream_) {
        oviora::log::warn("remoteplay-mic", std::format("no microphone opened: {}", SDL_GetError()));
        stop();
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        oviora::log::warn("remoteplay-mic", std::format("microphone did not start: {}", SDL_GetError()));
        stop();
        return false;
    }
    oviora::log::info("remoteplay-mic", std::format("recording from {} (48 kHz stereo, 10 ms blocks)", opened_.empty() ? "the default microphone" : "the chosen microphone"));
    return true;
}

void Microphone::stop() {
    // Destroying the stream waits for a running callback; the sink goes after that.
    if (stream_) { SDL_DestroyAudioStream(stream_); stream_ = nullptr; }
    {
        std::lock_guard lock(sinkMutex_);
        sink_ = nullptr;
    }
    if (audioInitialized_) { SDL_QuitSubSystem(SDL_INIT_AUDIO); audioInitialized_ = false; }
    opened_.clear();
}

void Microphone::callback(void* user, SDL_AudioStream* stream, int, int) {
    auto& self = *static_cast<Microphone*>(user);
    while (SDL_GetAudioStreamAvailable(stream) >= kBlockBytes) {
        std::vector<std::int16_t> block(size_t(kFrames) * 2);
        if (SDL_GetAudioStreamData(stream, block.data(), kBlockBytes) != kBlockBytes) break;
        std::lock_guard lock(self.sinkMutex_);
        if (self.sink_) self.sink_(std::move(block));
    }
}
} // namespace oviora::remoteplay
