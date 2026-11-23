// SPDX-License-Identifier: GPL-3.0-only
#include "IzletMediaCache.h"

#include <algorithm>
#include <cctype>

namespace oviora::gecit {
namespace {

std::size_t matchStartCode(std::span<const std::uint8_t> b, std::size_t pos) noexcept {
    if (pos + 3 <= b.size() && b[pos] == 0 && b[pos + 1] == 0) {
        if (b[pos + 2] == 1) return 3;
        if (pos + 4 <= b.size() && b[pos + 2] == 0 && b[pos + 3] == 1) return 4;
    }
    return 0;
}

struct RawNal {
    std::size_t startOffset = 0;
    std::size_t headerOffset = 0;
    std::size_t endOffset = 0;
    unsigned type = 0;
};

std::vector<RawNal> scanAnnexBNals(std::span<const std::uint8_t> b, IzletCodec codec, bool& ok) {
    ok = false;
    std::vector<RawNal> out;
    std::size_t pos = 0;
    while (pos + 3 <= b.size()) {
        const std::size_t prefix = matchStartCode(b, pos);
        if (!prefix) {
            if (out.empty() && b[pos] != 0) {
                out.clear();
                return out;
            }
            ++pos;
            continue;
        }
        const std::size_t hdr = pos + prefix;
        if (hdr >= b.size() || (b[hdr] & 0x80u) != 0) {
            out.clear();
            return out;
        }
        unsigned type = 0;
        if (codec == IzletCodec::H264) {
            type = b[hdr] & 0x1Fu;
            if (type == 0 || type > 23u) {
                out.clear();
                return out;
            }
        } else {
            if (hdr + 1 >= b.size() || (b[hdr + 1] & 0x07u) == 0) {
                out.clear();
                return out;
            }
            type = (b[hdr] >> 1) & 0x3Fu;
        }
        if (!out.empty()) out.back().endOffset = pos;
        out.push_back(RawNal{pos, hdr, b.size(), type});
        pos = hdr + (codec == IzletCodec::H264 ? 1u : 2u);
    }
    ok = !out.empty();
    return out;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<IzletCodec> parseIzletCodec(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (equalsIgnoreCase(text, "h264")) return IzletCodec::H264;
    if (equalsIgnoreCase(text, "h265") || equalsIgnoreCase(text, "hevc") || equalsIgnoreCase(text, "h265hdr")) {
        return IzletCodec::H265;
    }
    return std::nullopt;
}

const char* izletCodecName(IzletCodec codec) noexcept {
    return codec == IzletCodec::H264 ? "h264" : "h265";
}

IzletNalSummary inspectIzletAnnexB(std::span<const std::uint8_t> annexB, IzletCodec codec) noexcept {
    IzletNalSummary out;
    std::size_t pos = 0;
    bool any = false;
    while (pos + 3 <= annexB.size()) {
        const std::size_t prefix = matchStartCode(annexB, pos);
        if (!prefix) {
            if (!any && annexB[pos] != 0) return {};
            ++pos;
            continue;
        }
        const std::size_t hdr = pos + prefix;
        if (hdr >= annexB.size() || (annexB[hdr] & 0x80u) != 0) return {};
        if (codec == IzletCodec::H264) {
            const unsigned type = annexB[hdr] & 0x1Fu;
            if (type == 0 || type > 23u) return {};
            out.hasSps |= (type == 7u);
            out.hasPps |= (type == 8u);
            out.hasConfig |= (type == 7u || type == 8u);
            out.hasPicture |= (type >= 1u && type <= 5u);
            out.isIdr |= (type == 5u);
            pos = hdr + 1u;
        } else {
            if (hdr + 1 >= annexB.size() || (annexB[hdr + 1] & 0x07u) == 0) return {};
            const unsigned type = (annexB[hdr] >> 1) & 0x3Fu;
            out.hasVps |= (type == 32u);
            out.hasSps |= (type == 33u);
            out.hasPps |= (type == 34u);
            out.hasConfig |= (type == 32u || type == 33u || type == 34u);
            out.hasPicture |= (type <= 31u);
            out.isIdr |= (type == 19u || type == 20u || type == 21u);
            pos = hdr + 2u;
        }
        any = true;
    }
    out.valid = any;
    return out;
}

IzletGopBuffer::IzletGopBuffer(IzletCodec codec, double maxGopSeconds, std::size_t maxGopBytes)
    : codec_(codec),
      maxGopSeconds_(std::max(0.1, maxGopSeconds)),
      maxGopBytes_(std::max<std::size_t>(1024u, maxGopBytes)) {}

bool IzletGopBuffer::hasConfig() const noexcept {
    if (codec_ == IzletCodec::H264) {
        return !sps_.empty() && !pps_.empty();
    }
    return !vps_.empty() && !sps_.empty() && !pps_.empty();
}

bool IzletGopBuffer::hasAllConfigNals(const IzletNalSummary& summary) const noexcept {
    if (codec_ == IzletCodec::H264) {
        return summary.hasSps && summary.hasPps;
    }
    return summary.hasVps && summary.hasSps && summary.hasPps;
}

void IzletGopBuffer::extractParameterSets(std::span<const std::uint8_t> annexB) {
    bool ok = false;
    const auto nals = scanAnnexBNals(annexB, codec_, ok);
    if (!ok) return;

    bool changed = false;
    for (const auto& n : nals) {
        const auto slice = annexB.subspan(n.startOffset, n.endOffset - n.startOffset);
        if (codec_ == IzletCodec::H264) {
            if (n.type == 7u) {
                sps_.assign(slice.begin(), slice.end());
                changed = true;
            } else if (n.type == 8u) {
                pps_.assign(slice.begin(), slice.end());
                changed = true;
            }
        } else {
            if (n.type == 32u) {
                vps_.assign(slice.begin(), slice.end());
                changed = true;
            } else if (n.type == 33u) {
                sps_.assign(slice.begin(), slice.end());
                changed = true;
            } else if (n.type == 34u) {
                pps_.assign(slice.begin(), slice.end());
                changed = true;
            }
        }
    }
    if (changed) {
        rebuildCachedConfig();
    }
}

void IzletGopBuffer::rebuildCachedConfig() {
    cachedConfig_.clear();
    if (!hasConfig()) return;
    cachedConfig_.reserve(vps_.size() + sps_.size() + pps_.size());
    if (codec_ == IzletCodec::H265) {
        cachedConfig_.insert(cachedConfig_.end(), vps_.begin(), vps_.end());
    }
    cachedConfig_.insert(cachedConfig_.end(), sps_.begin(), sps_.end());
    cachedConfig_.insert(cachedConfig_.end(), pps_.begin(), pps_.end());
}

std::optional<IzletGopBuffer::Frame> IzletGopBuffer::pushAccessUnit(
    std::span<const std::uint8_t> annexB,
    double mediaTimeSec) {
    if (annexB.empty()) return std::nullopt;
    const IzletNalSummary info = inspectIzletAnnexB(annexB, codec_);
    if (!info.valid) return std::nullopt;

    if (info.hasConfig) {
        extractParameterSets(annexB);
    }
    if (!info.hasPicture) {
        return std::nullopt;
    }

    const std::uint64_t seq = nextSeq_++;
    if (info.isIdr) {
        const bool selfContained = hasAllConfigNals(info);
        if (!selfContained && !hasConfig()) {
            invalidateGop();
            return std::nullopt;
        }

        auto payload = std::make_shared<std::vector<std::uint8_t>>();
        if (!selfContained) {
            payload->reserve(cachedConfig_.size() + annexB.size());
            payload->insert(payload->end(), cachedConfig_.begin(), cachedConfig_.end());
        } else {
            payload->reserve(annexB.size());
        }
        payload->insert(payload->end(), annexB.begin(), annexB.end());

        Frame f{seq, mediaTimeSec, std::move(payload), true};
        gop_.clear();
        gopBytes_ = 0;
        gopStartSec_ = mediaTimeSec;
        lastIdrSeq_ = seq;
        if (f.bytes->size() <= maxGopBytes_) {
            gop_.push_back(f);
            gopBytes_ = f.bytes->size();
            hasValidGop_ = true;
        } else {
            hasValidGop_ = false;
        }
        return f;
    }

    auto payload = std::make_shared<std::vector<std::uint8_t>>(annexB.begin(), annexB.end());
    Frame f{seq, mediaTimeSec, std::move(payload), false};
    if (hasValidGop_) {
        const double age = mediaTimeSec - gopStartSec_;
        if (age < 0.0 || age > maxGopSeconds_ || (gopBytes_ + f.bytes->size()) > maxGopBytes_) {
            invalidateGop();
        } else {
            gop_.push_back(f);
            gopBytes_ += f.bytes->size();
        }
    }
    return f;
}

IzletGopBuffer::Snapshot IzletGopBuffer::snapshotGop() const {
    Snapshot snap;
    snap.valid = hasValidGop_ && !gop_.empty();
    if (snap.valid) {
        snap.idrSeq = lastIdrSeq_;
        snap.frames = gop_;
    }
    return snap;
}

void IzletGopBuffer::invalidateGop() noexcept {
    gop_.clear();
    gopBytes_ = 0;
    hasValidGop_ = false;
}

void IzletGopBuffer::reset() noexcept {
    vps_.clear();
    sps_.clear();
    pps_.clear();
    cachedConfig_.clear();
    invalidateGop();
    lastIdrSeq_ = 0;
}

bool canAcceptViewerBandwidth(double currentOutboundMbps,
                              double estimatedNewViewerMbps,
                              std::size_t maxViewerMbps) noexcept {
    const double cap = static_cast<double>(maxViewerMbps);
    if (cap <= 0.0) return false;
    if (currentOutboundMbps >= cap) return false;
    if (estimatedNewViewerMbps > 0.0 && (currentOutboundMbps + estimatedNewViewerMbps) > cap) {
        return false;
    }
    return true;
}

} // namespace oviora::gecit
