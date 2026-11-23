// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace oviora::gecit {

// Caches the latest Annex-B SPS/PPS configuration and prepends it before IDR
// access units when not already present, ensuring the WebRTC H.264 decoder
// always receives parameter sets with every keyframe.
class H264RelayCache {
public:
    void setConfig(std::span<const std::uint8_t> configBytes);
    void reset() noexcept;
    void requireKeyframe() noexcept { waitingForIdr_ = true; }

    bool hasConfig() const noexcept { return !cachedConfig_.empty(); }
    bool waitingForIdr() const noexcept { return waitingForIdr_; }
    std::span<const std::uint8_t> cachedConfig() const noexcept { return cachedConfig_; }

    // Returns Annex-B bytes ready to send over RTP, or std::nullopt if waiting
    // for an IDR / SPS+PPS or if the input is malformed.
    std::optional<std::vector<std::uint8_t>> processAccessUnit(
        std::span<const std::uint8_t> accessUnit,
        bool isIdr,
        bool lostOrRecovered = false);

private:
    std::vector<std::uint8_t> cachedConfig_;
    bool waitingForIdr_ = true;
};

struct AnnexBTestFrame {
    std::vector<std::uint8_t> configBytes; // Non-empty when SPS/PPS were seen for this frame
    std::vector<std::uint8_t> frameBytes;
    bool isIdr = false;
};

// Splits a raw Annex-B H.264 bytestream into per-frame access units (used by `--deneme-kaynak`).
std::vector<AnnexBTestFrame> splitAnnexBFrames(std::span<const std::uint8_t> stream);

} // namespace oviora::gecit
