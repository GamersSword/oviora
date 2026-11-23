// SPDX-License-Identifier: GPL-3.0-only
#include "H264RelayCache.h"

#include "oviora/remoteplay/Types.h"

namespace oviora::gecit {
namespace {

struct NalRange {
    std::size_t startOffset = 0; // Offset of start code
    std::size_t payloadOffset = 0; // Offset of NAL header byte
    std::size_t endOffset = 0;
    std::uint8_t type = 0;
};

std::size_t matchStartCode(std::span<const std::uint8_t> b, std::size_t pos) noexcept {
    if (pos + 3 <= b.size() && b[pos] == 0 && b[pos + 1] == 0) {
        if (b[pos + 2] == 1) return 3;
        if (pos + 4 <= b.size() && b[pos + 2] == 0 && b[pos + 3] == 1) return 4;
    }
    return 0;
}

std::vector<NalRange> findNals(std::span<const std::uint8_t> b) {
    std::vector<NalRange> out;
    std::size_t pos = 0;
    while (pos < b.size()) {
        const std::size_t prefix = matchStartCode(b, pos);
        if (!prefix) {
            ++pos;
            continue;
        }
        const std::size_t hdr = pos + prefix;
        if (hdr >= b.size()) break;
        if (!out.empty()) out.back().endOffset = pos;
        NalRange r;
        r.startOffset = pos;
        r.payloadOffset = hdr;
        r.endOffset = b.size();
        r.type = static_cast<std::uint8_t>(b[hdr] & 0x1Fu);
        out.push_back(r);
        pos = hdr + 1;
    }
    return out;
}

void appendSpan(std::vector<std::uint8_t>& dst, std::span<const std::uint8_t> src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

} // namespace

void H264RelayCache::setConfig(std::span<const std::uint8_t> configBytes) {
    if (configBytes.empty()) return;
    const auto info = oviora::remoteplay::inspectAnnexB(configBytes, oviora::remoteplay::Codec::H264);
    if (!info.valid || !info.hasConfig) return;
    cachedConfig_.assign(configBytes.begin(), configBytes.end());
}

void H264RelayCache::reset() noexcept {
    cachedConfig_.clear();
    waitingForIdr_ = true;
}

std::optional<std::vector<std::uint8_t>> H264RelayCache::processAccessUnit(
    std::span<const std::uint8_t> accessUnit,
    bool isIdr,
    bool lostOrRecovered) {
    if (accessUnit.empty()) return std::nullopt;
    if (lostOrRecovered) {
        waitingForIdr_ = true;
    }
    const auto info = oviora::remoteplay::inspectAnnexB(accessUnit, oviora::remoteplay::Codec::H264);
    if (!info.valid || !info.hasPicture) return std::nullopt;

    const bool effectiveIdr = isIdr || info.idr;
    if (waitingForIdr_ && !effectiveIdr) {
        return std::nullopt;
    }

    if (effectiveIdr && !info.hasConfig) {
        if (cachedConfig_.empty()) {
            waitingForIdr_ = true;
            return std::nullopt;
        }
        std::vector<std::uint8_t> combined;
        combined.reserve(cachedConfig_.size() + accessUnit.size());
        combined.insert(combined.end(), cachedConfig_.begin(), cachedConfig_.end());
        combined.insert(combined.end(), accessUnit.begin(), accessUnit.end());
        waitingForIdr_ = false;
        return combined;
    }

    waitingForIdr_ = false;
    return std::vector<std::uint8_t>(accessUnit.begin(), accessUnit.end());
}

std::vector<AnnexBTestFrame> splitAnnexBFrames(std::span<const std::uint8_t> stream) {
    std::vector<AnnexBTestFrame> frames;
    const auto nals = findNals(stream);
    if (nals.empty()) return frames;

    std::vector<std::uint8_t> pendingConfig;
    std::vector<std::uint8_t> currentAu;
    bool currentHasSlice = false;
    bool currentIsIdr = false;

    const auto flushAu = [&]() {
        if (!currentHasSlice || currentAu.empty()) return;
        AnnexBTestFrame f;
        f.configBytes = pendingConfig;
        f.frameBytes = std::move(currentAu);
        f.isIdr = currentIsIdr;
        frames.push_back(std::move(f));
        currentAu.clear();
        pendingConfig.clear();
        currentHasSlice = false;
        currentIsIdr = false;
    };

    for (const auto& nal : nals) {
        const auto slice = stream.subspan(nal.startOffset, nal.endOffset - nal.startOffset);
        if (nal.type == 9 /* AUD */) {
            flushAu();
            continue;
        }
        if (nal.type == 7 /* SPS */ || nal.type == 8 /* PPS */) {
            if (currentHasSlice) flushAu();
            appendSpan(pendingConfig, slice);
            continue;
        }
        const bool isSlice = (nal.type >= 1 && nal.type <= 5);
        if (isSlice && currentHasSlice) {
            const bool firstSliceOfPic = (nal.payloadOffset + 1 < nal.endOffset) &&
                ((stream[nal.payloadOffset + 1] & 0x80u) != 0);
            if (firstSliceOfPic || nal.type == 5) {
                flushAu();
            }
        }
        appendSpan(currentAu, slice);
        if (isSlice) {
            currentHasSlice = true;
            if (nal.type == 5) currentIsIdr = true;
        }
    }
    flushAu();
    return frames;
}

} // namespace oviora::gecit
