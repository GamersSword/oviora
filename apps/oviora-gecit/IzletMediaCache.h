// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace oviora::gecit {

enum class IzletCodec {
    H264,
    H265
};

std::optional<IzletCodec> parseIzletCodec(std::string_view text) noexcept;
const char* izletCodecName(IzletCodec codec) noexcept;

struct IzletNalSummary {
    bool valid = false;
    bool hasPicture = false;
    bool hasConfig = false;
    bool isIdr = false;
    bool hasVps = false;
    bool hasSps = false;
    bool hasPps = false;
};

// Inspects Annex-B NAL units for H.264 (SPS=7, PPS=8, IDR=5) or H.265
// (VPS=32, SPS=33, PPS=34, IRAP IDR_W_RADL=19 / IDR_N_LP=20 / CRA_NUT=21).
IzletNalSummary inspectIzletAnnexB(std::span<const std::uint8_t> annexB, IzletCodec codec) noexcept;

// Caches parameter sets (H.264 SPS/PPS; H.265 VPS/SPS/PPS) and retains access
// units since the last keyframe up to `maxGopSeconds` (default 3 s) and
// `maxGopBytes` (default 4 MB) for fast viewer join and PLI recovery.
class IzletGopBuffer {
public:
    static constexpr double kMaxGopSeconds = 3.0;
    static constexpr std::size_t kMaxGopBytes = 4u * 1024u * 1024u;
    static constexpr std::size_t kMaxViewersPerBroadcast = 20;

    struct Frame {
        std::uint64_t seq = 0;
        double mediaTimeSec = 0.0;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        bool isIdr = false;
    };

    struct Snapshot {
        bool valid = false;
        std::uint64_t idrSeq = 0;
        std::vector<Frame> frames;
    };

    explicit IzletGopBuffer(IzletCodec codec,
                            double maxGopSeconds = kMaxGopSeconds,
                            std::size_t maxGopBytes = kMaxGopBytes);

    IzletCodec codec() const noexcept { return codec_; }
    bool hasConfig() const noexcept;
    bool hasValidGop() const noexcept { return hasValidGop_; }
    std::uint64_t lastIdrSeq() const noexcept { return lastIdrSeq_; }
    std::size_t gopBytes() const noexcept { return gopBytes_; }
    std::size_t gopFrameCount() const noexcept { return gop_.size(); }
    const std::vector<std::uint8_t>& cachedConfig() const noexcept { return cachedConfig_; }

    // Processes an incoming Annex-B access unit. Updates cached parameter sets;
    // returns a Frame for live fan-out whenever `annexB` contains picture slices.
    std::optional<Frame> pushAccessUnit(std::span<const std::uint8_t> annexB, double mediaTimeSec);

    Snapshot snapshotGop() const;
    void invalidateGop() noexcept;
    void reset() noexcept;

private:
    void extractParameterSets(std::span<const std::uint8_t> annexB);
    void rebuildCachedConfig();
    bool hasAllConfigNals(const IzletNalSummary& summary) const noexcept;

    IzletCodec codec_;
    double maxGopSeconds_;
    std::size_t maxGopBytes_;

    std::vector<std::uint8_t> vps_;
    std::vector<std::uint8_t> sps_;
    std::vector<std::uint8_t> pps_;
    std::vector<std::uint8_t> cachedConfig_;

    std::vector<Frame> gop_;
    std::size_t gopBytes_ = 0;
    double gopStartSec_ = 0.0;
    bool hasValidGop_ = false;
    std::uint64_t nextSeq_ = 1;
    std::uint64_t lastIdrSeq_ = 0;
};

// Returns true if admitting one more viewer will stay within `maxViewerMbps`.
bool canAcceptViewerBandwidth(double currentOutboundMbps,
                              double estimatedNewViewerMbps,
                              std::size_t maxViewerMbps) noexcept;

} // namespace oviora::gecit
