// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "GatewayConfig.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oviora::gecit {

struct WebRtcSessionCallbacks {
    std::function<void(const std::string& signalJsonDump)> onLocalSignal;
    std::function<void()> onConnected;
    std::function<void(const std::string& reason)> onDisconnected;
    std::function<void()> onPliRequested;
    std::function<void(std::span<const std::uint8_t> data)> onKumanda;
    std::function<void()> onBye;
};

struct WebRtcStatsSnapshot {
    std::uint64_t videoFramesSent = 0;
    std::uint64_t videoBytesSent = 0;
    std::uint64_t audioBytesSent = 0;
    int rttMs = -1;
    bool connected = false;
};

// Manages one rtc::PeerConnection for a single `wsid`: answers the browser's
// SDP offer, negotiates H.264 / Opus payload types, forwards media frames,
// handles PLI/FIR (rate-limited to at most once per 500 ms), and wires the
// `kumanda` and `kontrol` DataChannels.
class WebRtcGatewaySession {
public:
    WebRtcGatewaySession(std::string wsid, const GatewayConfig& config, WebRtcSessionCallbacks callbacks);
    ~WebRtcGatewaySession();

    WebRtcGatewaySession(const WebRtcGatewaySession&) = delete;
    WebRtcGatewaySession& operator=(const WebRtcGatewaySession&) = delete;

    // Handles a browser signal (`{"type":"offer","sdp":...}` or `{"candidate":...,"mid":...}`).
    bool handleRemoteSignal(const nlohmann::json& data, std::string* error = nullptr);

    // Sends an Annex-B H.264 access unit (with SPS/PPS already prepended on IDR).
    bool sendVideoFrame(std::span<const std::uint8_t> annexB);

    // Sends one raw Opus packet (or advances timestamp by `frameSamples` if empty on PLC).
    bool sendAudioPacket(std::span<const std::uint8_t> opusPacket, std::uint32_t frameSamples);

    bool isVideoReady() const noexcept;
    bool hasSentVideoFrame() const noexcept { return videoFramesSent_.load(std::memory_order_relaxed) > 0; }

    void sendState(std::string_view state);
    void sendRumble(std::uint8_t left, std::uint8_t right);
    void sendError(std::string_view message);

    WebRtcStatsSnapshot stats() const;
    void close();

    // Exposed for unit-testing H.264/H.265/Opus offer selection logic without network.
    static int selectH264PayloadType(const rtc::Description::Media& media);
    static int selectH265PayloadType(const rtc::Description::Media& media);
    static int selectOpusPayloadType(const rtc::Description::Media& media);

private:
    struct Gate {
        std::mutex mutex;
        bool open = true;
    };

    bool ensurePeerConnection(std::string* error);
    void onTrackReceived(const std::shared_ptr<rtc::Track>& track);
    void onDataChannelReceived(const std::shared_ptr<rtc::DataChannel>& dc);
    void triggerPli();
    void sendKontrolText(const std::string& jsonText);

    std::string wsid_;
    std::string shortId_;
    std::uint16_t udpBegin_ = 50000;
    std::uint16_t udpEnd_ = 50100;
    std::vector<std::string> stunServers_;
    WebRtcSessionCallbacks callbacks_;

    std::shared_ptr<Gate> gate_ = std::make_shared<Gate>();
    std::mutex pcMutex_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    bool remoteOfferApplied_ = false;
    std::vector<rtc::Candidate> pendingCandidates_;

    std::mutex videoMutex_;
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::RtpPacketizationConfig> videoRtpConfig_;
    std::chrono::steady_clock::time_point videoOrigin_{};
    bool videoOriginSet_ = false;

    std::mutex audioMutex_;
    std::shared_ptr<rtc::Track> audioTrack_;
    std::shared_ptr<rtc::RtpPacketizationConfig> audioRtpConfig_;
    std::uint64_t audioSamplesSent_ = 0;

    std::mutex dcMutex_;
    std::shared_ptr<rtc::DataChannel> kumandaDc_;
    std::shared_ptr<rtc::DataChannel> kontrolDc_;
    std::string lastState_ = "baglaniyor";

    std::mutex pliMutex_;
    std::chrono::steady_clock::time_point lastPli_{};
    bool hasPli_ = false;

    std::atomic<bool> closed_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> videoTrackOpen_{false};
    std::atomic<std::uint64_t> videoFramesSent_{0};
    std::atomic<std::uint64_t> videoBytesSent_{0};
    std::atomic<std::uint64_t> audioBytesSent_{0};
};

} // namespace oviora::gecit
