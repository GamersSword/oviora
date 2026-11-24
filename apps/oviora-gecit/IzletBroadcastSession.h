// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "GatewayConfig.h"
#include "IzletMediaCache.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace oviora::gecit {

struct IzletBroadcastCallbacks {
    std::function<void(const std::string& yid, const std::string& jsonStr)> onPublisherSignal;
    std::function<void(const std::string& yid, const std::string& state, const std::string& err)> onPublisherState;
    std::function<void(const std::string& yid, const std::string& reason)> onPublisherEnded;

    std::function<void(const std::string& iid, const std::string& jsonStr)> onViewerSignal;
    std::function<void(const std::string& iid, const std::string& state, const std::string& err)> onViewerState;
    std::function<void(const std::string& yid, const std::string& iid, const std::string& reason)> onViewerEnded;
};

struct IzletBroadcastStats {
    int outboundKbps = 0;
    int viewers = 0;
    double outboundMbps = 0.0;
    double publisherInputMbps = 0.0;
    int lostPackets = 0;      // video packets lost from the player since the last stats
    int publisherKbps = 0;    // what the player sends (its upload)
};

// SFU broadcast node for one `yid`:
//   - 1 recvonly publisher PeerConnection (H.264 or H.265 + Opus + `kontrol` DC)
//   - up to 20 sendonly viewer PeerConnections (each with its own sender queue/thread)
//   - caches parameter sets & last IDR GOP (max 3 s / 4 MB) for fast viewer startup
class IzletBroadcastSession : public std::enable_shared_from_this<IzletBroadcastSession> {
public:
    IzletBroadcastSession(std::string yid,
                          IzletCodec codec,
                          const GatewayConfig& config,
                          IzletBroadcastCallbacks callbacks);
    ~IzletBroadcastSession();

    IzletBroadcastSession(const IzletBroadcastSession&) = delete;
    IzletBroadcastSession& operator=(const IzletBroadcastSession&) = delete;

    const std::string& yid() const noexcept { return yid_; }
    IzletCodec codec() const noexcept { return codec_; }

    bool handlePublisherSignal(const nlohmann::json& data, std::string* error = nullptr);
    bool addViewer(const std::string& iid, std::string* error = nullptr);
    bool handleViewerSignal(const std::string& iid, const nlohmann::json& data, std::string* error = nullptr);
    void removeViewer(const std::string& iid, const std::string& reason, bool notifyPhp);

    bool hasViewer(const std::string& iid) const;
    std::size_t viewerCount() const;
    double estimatedInputMbps() const noexcept;
    IzletBroadcastStats collectStats(double elapsedSec);

    void closeAll(bool notifyPhp);

private:
    struct Viewer;

    bool ensurePublisherPc(std::string* error);
    void onPublisherTrack(const std::shared_ptr<rtc::Track>& track);
    void onPublisherDataChannel(const std::shared_ptr<rtc::DataChannel>& dc);
    void onPublisherVideoFrame(const rtc::binary& data, const rtc::FrameInfo& info);
    void onPublisherAudioFrame(const rtc::binary& data, const rtc::FrameInfo& info);
    void requestPublisherIdr();

    bool ensureViewerPc(const std::shared_ptr<Viewer>& viewer, std::string* error);
    void onViewerTrack(const std::shared_ptr<Viewer>& viewer, const std::shared_ptr<rtc::Track>& track);
    void onViewerVideoReady(const std::shared_ptr<Viewer>& viewer);
    void onViewerPli(const std::shared_ptr<Viewer>& viewer);
    void closeViewerInternal(const std::shared_ptr<Viewer>& viewer, bool notifyPhp, const std::string& finalState, const std::string& err);

    std::string yid_;
    std::string shortYid_;
    IzletCodec codec_;
    std::uint16_t udpBegin_ = 50000;
    std::uint16_t udpEnd_ = 50100;
    std::vector<std::string> stunServers_;
    IzletBroadcastCallbacks callbacks_;

    std::atomic<bool> closed_{false};
    // A video packet from the player was lost on its way here: the frames after it are broken until the next key frame.
    // Viewers get none of them (the picture holds for a moment instead of smearing); a key frame is asked for at once.
    std::atomic<bool> ingestBroken_{false};
    std::atomic<std::uint64_t> ingestLost_{0};
    std::uint64_t lastStatsLost_ = 0;
public:
    void onIngestLoss(unsigned lost);
private:
    std::atomic<bool> pubCanliSent_{false};

    std::mutex pubPcMutex_;
    std::shared_ptr<rtc::PeerConnection> pubPc_;
    bool pubOfferApplied_ = false;
    std::vector<rtc::Candidate> pubPendingCandidates_;

    std::mutex pubTrackMutex_;
    std::shared_ptr<rtc::Track> pubVideoTrack_;
    std::shared_ptr<rtc::Track> pubAudioTrack_;
    std::shared_ptr<rtc::DataChannel> pubKontrolDc_;

    std::mutex idrMutex_;
    std::chrono::steady_clock::time_point lastIdrReq_{};
    bool hasIdrReq_ = false;
    bool pendingKontrolIdr_ = false;
    unsigned consecutiveIdrReqs_ = 0;

    mutable std::mutex mediaMutex_;
    IzletGopBuffer gopBuffer_;
    std::chrono::steady_clock::time_point broadcastOrigin_{};
    bool broadcastOriginSet_ = false;
    double lastVideoTimeSec_ = -0.001;
    bool hasAudioTs_ = false;
    std::uint32_t lastAudioRtpTs_ = 0;
    std::unordered_map<std::string, std::shared_ptr<Viewer>> viewers_;

    // Publisher bitrate estimator + viewer outbound stats delta
    std::atomic<std::uint64_t> pubBytesTotal_{0};
    std::chrono::steady_clock::time_point lastRateSampleAt_ = std::chrono::steady_clock::now();
    std::uint64_t lastRateSamplePubBytes_ = 0;
    std::uint64_t lastStatsPubBytes_ = 0;
    std::uint64_t lastStatsOutBytes_ = 0;
    std::atomic<double> estimatedPubMbps_{0.0};
};

} // namespace oviora::gecit
