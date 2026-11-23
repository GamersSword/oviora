// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oviora/remoteplay/Types.h"
#include "oviora/remoteplay/WatchRoute.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace oviora::remoteplay {

// WebRTC recvonly receiver for watching a friend's "İzlet" broadcast through
// oviora-gecit SFU inside the native Chiaki/FFmpeg/D3D12 playback pipeline.
class WatchReceiver final {
public:
    struct Callbacks {
        std::function<void(std::span<const std::uint8_t> annexB, HostTime arrival100ns)> onVideo;
        std::function<void(std::span<const std::uint8_t> opusPacket)> onAudio;
        std::function<void(std::string reason)> onFailed;
    };

    WatchReceiver() = default;
    ~WatchReceiver();

    WatchReceiver(const WatchReceiver&) = delete;
    WatchReceiver& operator=(const WatchReceiver&) = delete;

    bool start(std::shared_ptr<WatchRoute> route, Callbacks callbacks);
    void stop();
    void requestKeyframe();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace oviora::remoteplay
