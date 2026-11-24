#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace oviora::source {

// A small picture of the PS5's stream while the connecting screen still shows: the decoder has
// frames some seconds before the picture pipeline is ready (frame generation, NGX warm-up), and the
// connecting screen puts one behind its scene. Only while the UI asks (wanted); the decode thread
// makes one about twice a second (RemotePlaySource), the UI takes it by serial.
class ConnectingPreview {
public:
    struct Picture {
        std::uint64_t serial = 0;
        std::uint32_t width = 0, height = 0;
        std::vector<std::uint8_t> bgra;  // width * height * 4
    };
    static ConnectingPreview& instance() { static ConnectingPreview p; return p; }
    void want(bool on) {
        wanted_.store(on);
        if (!on) { std::lock_guard lock(mutex_); picture_ = {}; }
    }
    bool wanted() const { return wanted_.load(); }
    void set(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>&& bgra) {
        std::lock_guard lock(mutex_);
        picture_.serial++;
        picture_.width = width;
        picture_.height = height;
        picture_.bgra = std::move(bgra);
    }
    // A newer picture than the serial given: true and filled
    bool takeNewer(std::uint64_t serial, Picture& out) {
        std::lock_guard lock(mutex_);
        if (picture_.serial <= serial || picture_.bgra.empty()) return false;
        out = picture_;
        return true;
    }
private:
    std::atomic<bool> wanted_{false};
    std::mutex mutex_;
    Picture picture_;
};

}
