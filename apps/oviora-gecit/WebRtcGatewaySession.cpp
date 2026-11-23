// SPDX-License-Identifier: GPL-3.0-only
#include "WebRtcGatewaySession.h"

#include "DataChannelProtocol.h"
#include "oviora/Log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <mutex>

namespace oviora::gecit {
namespace {

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    return asciiLower(haystack).find(asciiLower(needle)) != std::string::npos;
}

void initRtcLoggingOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Keep libdatachannel quiet by default so SDP/ICE IP addresses never enter gecit.log.
        const bool debug = GetEnvironmentVariableA("OVIORA_GECIT_RTC_DEBUG", nullptr, 0) > 0;
        rtc::InitLogger(debug ? rtc::LogLevel::Warning : rtc::LogLevel::None,
                        [](rtc::LogLevel level, std::string text) {
                            if (level <= rtc::LogLevel::Error) {
                                oviora::log::warn("gecit-rtc", text);
                            }
                        });
    });
}

void stripOtherPayloadTypes(rtc::Description::Media& media, int keepPt) {
    const auto allPts = media.payloadTypes();
    for (int pt : allPts) {
        if (pt != keepPt && media.hasPayloadType(pt)) {
            media.removeRtpMap(pt);
        }
    }
}

std::string normalizeCandidateAttr(std::string cand) {
    if (cand.rfind("a=", 0) == 0) cand.erase(0, 2);
    while (!cand.empty() && (cand.back() == '\r' || cand.back() == '\n' || cand.back() == ' ')) {
        cand.pop_back();
    }
    return cand;
}

} // namespace

int WebRtcGatewaySession::selectH264PayloadType(const rtc::Description::Media& media) {
    int bestPt = -1;
    int bestScore = -1;
    for (int pt : media.payloadTypes()) {
        if (!media.hasPayloadType(pt)) continue;
        const auto* rm = media.rtpMap(pt);
        if (!rm || asciiLower(rm->format) != "h264") continue;

        bool packetizationMode1 = false;
        std::string profileLevelId;
        for (const auto& fmtp : rm->fmtps) {
            const std::string low = asciiLower(fmtp);
            if (low.find("packetization-mode=1") != std::string::npos) {
                packetizationMode1 = true;
            }
            const auto pos = low.find("profile-level-id=");
            if (pos != std::string::npos) {
                profileLevelId = low.substr(pos + 17, 6);
            }
        }

        int score = packetizationMode1 ? 1000 : 100;
        if (profileLevelId == "42e01f") score += 90;
        else if (profileLevelId == "640c1f") score += 88;
        else if (profileLevelId.rfind("640", 0) == 0) score += 80;
        else if (profileLevelId.rfind("42e", 0) == 0) score += 75;
        else if (profileLevelId.rfind("4d0", 0) == 0) score += 70;
        else if (profileLevelId.rfind("420", 0) == 0) score += 60;

        if (score > bestScore) {
            bestScore = score;
            bestPt = pt;
        }
    }
    return bestPt;
}

int WebRtcGatewaySession::selectH265PayloadType(const rtc::Description::Media& media) {
    for (int pt : media.payloadTypes()) {
        if (!media.hasPayloadType(pt)) continue;
        const auto* rm = media.rtpMap(pt);
        if (!rm) continue;
        const std::string fmt = asciiLower(rm->format);
        if (fmt == "h265" || fmt == "hevc") return pt;
    }
    return -1;
}

int WebRtcGatewaySession::selectOpusPayloadType(const rtc::Description::Media& media) {
    for (int pt : media.payloadTypes()) {
        if (!media.hasPayloadType(pt)) continue;
        const auto* rm = media.rtpMap(pt);
        if (rm && asciiLower(rm->format) == "opus") return pt;
    }
    return -1;
}

WebRtcGatewaySession::WebRtcGatewaySession(std::string wsid,
                                           const GatewayConfig& config,
                                           WebRtcSessionCallbacks callbacks)
    : wsid_(std::move(wsid)),
      shortId_(shortWsid(wsid_)),
      udpBegin_(config.udpBegin),
      udpEnd_(config.udpEnd),
      stunServers_(config.stunServers),
      callbacks_(std::move(callbacks)) {
    initRtcLoggingOnce();
}

WebRtcGatewaySession::~WebRtcGatewaySession() {
    close();
}

bool WebRtcGatewaySession::ensurePeerConnection(std::string* error) {
    std::lock_guard lock(pcMutex_);
    if (pc_) return true;
    if (closed_.load()) {
        if (error) *error = "oturum kapatildi";
        return false;
    }

    rtc::Configuration cfg;
    cfg.disableAutoNegotiation = true;
    cfg.portRangeBegin = udpBegin_;
    cfg.portRangeEnd = udpEnd_;
    cfg.maxMessageSize = 64 * 1024;
    for (const auto& stun : stunServers_) {
        if (!stun.empty()) cfg.iceServers.emplace_back(stun);
    }

    try {
        pc_ = std::make_shared<rtc::PeerConnection>(cfg);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }

    pc_->onStateChange([this, gate = gate_](rtc::PeerConnection::State state) {
        std::lock_guard cbLock(gate->mutex);
        if (!gate->open || closed_.load()) return;

        const char* label = "unknown";
        switch (state) {
        case rtc::PeerConnection::State::New: label = "new"; break;
        case rtc::PeerConnection::State::Connecting: label = "connecting"; break;
        case rtc::PeerConnection::State::Connected: label = "connected"; break;
        case rtc::PeerConnection::State::Disconnected: label = "disconnected"; break;
        case rtc::PeerConnection::State::Failed: label = "failed"; break;
        case rtc::PeerConnection::State::Closed: label = "closed"; break;
        }
        oviora::log::info("gecit-webrtc", std::format("wsid={} state={}", shortId_, label));

        if (state == rtc::PeerConnection::State::Connected) {
            connected_.store(true);
            if (callbacks_.onConnected) callbacks_.onConnected();
        } else if (state == rtc::PeerConnection::State::Failed ||
                   state == rtc::PeerConnection::State::Closed ||
                   state == rtc::PeerConnection::State::Disconnected) {
            connected_.store(false);
            if (callbacks_.onDisconnected) callbacks_.onDisconnected(label);
        }
    });

    pc_->onLocalCandidate([this, gate = gate_](rtc::Candidate cand) {
        std::lock_guard cbLock(gate->mutex);
        if (!gate->open || closed_.load()) return;
        const std::string candStr = normalizeCandidateAttr(cand.candidate());
        if (candStr.empty()) return;
        if (callbacks_.onLocalSignal) {
            const nlohmann::json msg = {{"candidate", candStr}, {"mid", cand.mid()}};
            callbacks_.onLocalSignal(msg.dump());
        }
    });

    pc_->onTrack([this, gate = gate_](std::shared_ptr<rtc::Track> track) {
        std::lock_guard cbLock(gate->mutex);
        if (!gate->open || closed_.load() || !track) return;
        onTrackReceived(track);
    });

    pc_->onDataChannel([this, gate = gate_](std::shared_ptr<rtc::DataChannel> dc) {
        std::lock_guard cbLock(gate->mutex);
        if (!gate->open || closed_.load() || !dc) return;
        onDataChannelReceived(dc);
    });

    return true;
}

void WebRtcGatewaySession::onTrackReceived(const std::shared_ptr<rtc::Track>& track) {
    auto desc = track->description();
    const std::string mediaType = asciiLower(desc.type());

    if (mediaType == "video") {
        const int pt = selectH264PayloadType(desc);
        if (pt < 0) {
            oviora::log::warn("gecit-webrtc", std::format("wsid={} offer missing H264", shortId_));
            desc.markRemoved();
            track->setDescription(desc);
            return;
        }

        stripOtherPayloadTypes(desc, pt);
        desc.setDirection(rtc::Description::Direction::SendOnly);
        constexpr rtc::SSRC kVideoSsrc = 0x56494431u;
        desc.addSSRC(kVideoSsrc, "oviora-video", "oviora-stream", "video0");
        track->setDescription(desc);

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc, "oviora-video", static_cast<std::uint8_t>(pt), rtc::H264RtpPacketizer::ClockRate);
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfig);
        auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
        auto nack = std::make_shared<rtc::RtcpNackResponder>();
        auto pli = std::make_shared<rtc::PliHandler>([this, gate = gate_]() {
            std::lock_guard cbLock(gate->mutex);
            if (!gate->open || closed_.load()) return;
            triggerPli();
        });
        packetizer->addToChain(srReporter);
        packetizer->addToChain(nack);
        packetizer->addToChain(pli);
        track->setMediaHandler(packetizer);

        track->onOpen([this, gate = gate_]() {
            std::lock_guard cbLock(gate->mutex);
            if (!gate->open || closed_.load()) return;
            videoTrackOpen_.store(true, std::memory_order_release);
            oviora::log::info("gecit-webrtc", std::format("wsid={} video track open", shortId_));
            triggerPli();
        });

        {
            std::lock_guard vLock(videoMutex_);
            videoTrack_ = track;
            videoRtpConfig_ = rtpConfig;
            videoOriginSet_ = false;
        }
        oviora::log::info("gecit-webrtc", std::format("wsid={} negotiated H264 pt={}", shortId_, pt));
    } else if (mediaType == "audio") {
        const int pt = selectOpusPayloadType(desc);
        if (pt < 0) {
            oviora::log::warn("gecit-webrtc", std::format("wsid={} offer missing Opus", shortId_));
            desc.markRemoved();
            track->setDescription(desc);
            return;
        }

        stripOtherPayloadTypes(desc, pt);
        desc.setDirection(rtc::Description::Direction::SendOnly);
        constexpr rtc::SSRC kAudioSsrc = 0x41554431u;
        desc.addSSRC(kAudioSsrc, "oviora-audio", "oviora-stream", "audio0");
        track->setDescription(desc);

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            kAudioSsrc, "oviora-audio", static_cast<std::uint8_t>(pt), rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
        auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
        auto nack = std::make_shared<rtc::RtcpNackResponder>();
        packetizer->addToChain(srReporter);
        packetizer->addToChain(nack);
        track->setMediaHandler(packetizer);

        track->onOpen([this, gate = gate_]() {
            std::lock_guard cbLock(gate->mutex);
            if (!gate->open || closed_.load()) return;
            oviora::log::info("gecit-webrtc", std::format("wsid={} audio track open", shortId_));
        });

        {
            std::lock_guard aLock(audioMutex_);
            audioTrack_ = track;
            audioRtpConfig_ = rtpConfig;
            audioSamplesSent_ = 0;
        }
        oviora::log::info("gecit-webrtc", std::format("wsid={} negotiated Opus pt={}", shortId_, pt));
    } else {
        desc.markRemoved();
        track->setDescription(desc);
    }
}

void WebRtcGatewaySession::onDataChannelReceived(const std::shared_ptr<rtc::DataChannel>& dc) {
    const std::string label = dc->label();
    if (label == "kumanda") {
        {
            std::lock_guard dLock(dcMutex_);
            kumandaDc_ = dc;
        }
        dc->onOpen([this, gate = gate_]() {
            std::lock_guard cbLock(gate->mutex);
            if (!gate->open || closed_.load()) return;
            oviora::log::info("gecit-webrtc", std::format("wsid={} dc kumanda open", shortId_));
        });
        dc->onMessage([this, gate = gate_](rtc::message_variant msg) {
            std::lock_guard cbLock(gate->mutex);
            if (!gate->open || closed_.load()) return;
            if (const auto* bin = std::get_if<rtc::binary>(&msg)) {
                if (callbacks_.onKumanda) {
                    callbacks_.onKumanda({reinterpret_cast<const std::uint8_t*>(bin->data()), bin->size()});
                }
            }
        });
    } else if (label == "kontrol") {
        {
            std::lock_guard dLock(dcMutex_);
            kontrolDc_ = dc;
        }
        dc->onOpen([this, gate = gate_]() {
            std::lock_guard cbLock(gate->mutex);
            if (!gate->open || closed_.load()) return;
            oviora::log::info("gecit-webrtc", std::format("wsid={} dc kontrol open", shortId_));
            std::string stateToSend;
            {
                std::lock_guard dLock(dcMutex_);
                stateToSend = lastState_;
            }
            if (!stateToSend.empty()) {
                sendKontrolText(makeKontrolStateJson(stateToSend));
            }
        });
        dc->onMessage([this, gate = gate_](rtc::message_variant msg) {
            std::lock_guard cbLock(gate->mutex);
            if (!gate->open || closed_.load()) return;
            std::string text;
            if (const auto* s = std::get_if<std::string>(&msg)) {
                text = *s;
            } else if (const auto* bin = std::get_if<rtc::binary>(&msg)) {
                text.assign(reinterpret_cast<const char*>(bin->data()), bin->size());
            } else {
                return;
            }
            const auto kind = parseKontrolMessage(text);
            if (kind == KontrolIncomingType::Idr) {
                triggerPli();
            } else if (kind == KontrolIncomingType::Bye) {
                if (callbacks_.onBye) callbacks_.onBye();
            }
        });
    } else {
        oviora::log::warn("gecit-webrtc", std::format("wsid={} unexpected DataChannel ignored", shortId_));
    }
}

bool WebRtcGatewaySession::handleRemoteSignal(const nlohmann::json& data, std::string* error) {
    if (!data.is_object()) {
        if (error) *error = "gecersiz sinyal JSON";
        return false;
    }
    if (!ensurePeerConnection(error)) return false;

    if (const auto typeIt = data.find("type"); typeIt != data.end() && typeIt->is_string()) {
        const std::string type = typeIt->get<std::string>();
        if (type == "offer") {
            const auto sdpIt = data.find("sdp");
            if (sdpIt == data.end() || !sdpIt->is_string()) {
                if (error) *error = "offer sdp eksik";
                return false;
            }
            const std::string offerSdp = sdpIt->get<std::string>();
            std::string answerSdp;
            {
                std::lock_guard lock(pcMutex_);
                if (!pc_ || closed_.load()) return false;
                try {
                    pc_->setRemoteDescription(rtc::Description(offerSdp, rtc::Description::Type::Offer));
                    remoteOfferApplied_ = true;
                    for (const auto& c : pendingCandidates_) {
                        try {
                            pc_->addRemoteCandidate(c);
                        } catch (const std::exception&) {
                        }
                    }
                    pendingCandidates_.clear();
                    pc_->setLocalDescription(rtc::Description::Type::Answer);
                    const auto local = pc_->localDescription();
                    if (!local) {
                        if (error) *error = "answer olusturulamadi";
                        return false;
                    }
                    answerSdp = std::string(*local);
                } catch (const std::exception& e) {
                    if (error) *error = e.what();
                    return false;
                }
            }
            if (callbacks_.onLocalSignal) {
                const nlohmann::json msg = {{"type", "answer"}, {"sdp", answerSdp}};
                callbacks_.onLocalSignal(msg.dump());
            }
            oviora::log::info("gecit-webrtc", std::format("wsid={} SDP answer sent", shortId_));
            return true;
        }
    }

    if (const auto candIt = data.find("candidate"); candIt != data.end() && candIt->is_string()) {
        std::string candStr = normalizeCandidateAttr(candIt->get<std::string>());
        if (candStr.empty()) return true; // End-of-candidates marker.
        std::string mid;
        if (const auto m = data.find("mid"); m != data.end() && m->is_string()) mid = m->get<std::string>();
        else if (const auto sm = data.find("sdpMid"); sm != data.end() && sm->is_string()) mid = sm->get<std::string>();
        if (mid.empty()) mid = "0";

        std::lock_guard lock(pcMutex_);
        if (!pc_ || closed_.load()) return false;
        try {
            rtc::Candidate cand(candStr, mid);
            if (remoteOfferApplied_) {
                pc_->addRemoteCandidate(cand);
            } else {
                pendingCandidates_.push_back(std::move(cand));
            }
        } catch (const std::exception&) {
            // Malformed or unsupported remote candidate (e.g. mDNS/TCP) is non-fatal.
        }
        return true;
    }

    return true;
}

bool WebRtcGatewaySession::sendVideoFrame(std::span<const std::uint8_t> annexB) {
    if (closed_.load() || annexB.empty()) return false;
    std::lock_guard vLock(videoMutex_);
    if (!videoTrack_ || !videoTrack_->isOpen()) return false;

    const auto now = std::chrono::steady_clock::now();
    if (!videoOriginSet_) {
        videoOrigin_ = now;
        videoOriginSet_ = true;
    }
    const std::chrono::duration<double> elapsed = now - videoOrigin_;
    rtc::binary payload(annexB.size());
    std::memcpy(payload.data(), annexB.data(), annexB.size());
    try {
        videoTrack_->sendFrame(std::move(payload), elapsed);
        videoFramesSent_.fetch_add(1, std::memory_order_relaxed);
        videoBytesSent_.fetch_add(annexB.size(), std::memory_order_relaxed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool WebRtcGatewaySession::sendAudioPacket(std::span<const std::uint8_t> opusPacket, std::uint32_t frameSamples) {
    if (closed_.load()) return false;
    std::lock_guard aLock(audioMutex_);
    const std::uint32_t step = frameSamples > 0 ? frameSamples : 480u;
    const std::uint64_t samplePos = audioSamplesSent_;
    audioSamplesSent_ += step;

    if (!audioTrack_ || !audioTrack_->isOpen() || opusPacket.empty()) return false;

    const std::chrono::duration<double> timestampSec(static_cast<double>(samplePos) / 48000.0);
    rtc::binary payload(opusPacket.size());
    std::memcpy(payload.data(), opusPacket.data(), opusPacket.size());
    try {
        audioTrack_->sendFrame(std::move(payload), timestampSec);
        audioBytesSent_.fetch_add(opusPacket.size(), std::memory_order_relaxed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool WebRtcGatewaySession::isVideoReady() const noexcept {
    if (closed_.load()) return false;
    return videoTrackOpen_.load(std::memory_order_acquire);
}

void WebRtcGatewaySession::triggerPli() {
    {
        std::lock_guard pLock(pliMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (hasPli_ && (now - lastPli_) < std::chrono::milliseconds(500)) {
            return;
        }
        hasPli_ = true;
        lastPli_ = now;
    }
    if (callbacks_.onPliRequested) {
        callbacks_.onPliRequested();
    }
}

void WebRtcGatewaySession::sendKontrolText(const std::string& jsonText) {
    if (closed_.load()) return;
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard dLock(dcMutex_);
        dc = kontrolDc_;
    }
    if (!dc || !dc->isOpen()) return;
    try {
        dc->send(jsonText);
    } catch (const std::exception&) {
    }
}

void WebRtcGatewaySession::sendState(std::string_view state) {
    {
        std::lock_guard dLock(dcMutex_);
        lastState_ = std::string(state);
    }
    sendKontrolText(makeKontrolStateJson(state));
}

void WebRtcGatewaySession::sendRumble(std::uint8_t left, std::uint8_t right) {
    sendKontrolText(makeKontrolRumbleJson(left, right));
}

void WebRtcGatewaySession::sendError(std::string_view message) {
    sendKontrolText(makeKontrolErrorJson(message));
}

WebRtcStatsSnapshot WebRtcGatewaySession::stats() const {
    WebRtcStatsSnapshot s;
    s.videoFramesSent = videoFramesSent_.load(std::memory_order_relaxed);
    s.videoBytesSent = videoBytesSent_.load(std::memory_order_relaxed);
    s.audioBytesSent = audioBytesSent_.load(std::memory_order_relaxed);
    s.connected = connected_.load(std::memory_order_relaxed);
    std::lock_guard cbLock(gate_->mutex);
    if (gate_->open && pc_) {
        if (const auto rtt = pc_->rtt()) {
            s.rttMs = static_cast<int>(rtt->count());
        }
    }
    return s;
}

void WebRtcGatewaySession::close() {
    if (closed_.exchange(true)) return;
    connected_.store(false);
    videoTrackOpen_.store(false);
    {
        std::lock_guard cbLock(gate_->mutex);
        gate_->open = false;
    }

    std::shared_ptr<rtc::DataChannel> kumanda, kontrol;
    {
        std::lock_guard dLock(dcMutex_);
        kumanda = std::move(kumandaDc_);
        kontrol = std::move(kontrolDc_);
    }
    if (kumanda) kumanda->resetCallbacks();
    if (kontrol) kontrol->resetCallbacks();

    std::shared_ptr<rtc::Track> vTrack, aTrack;
    {
        std::lock_guard vLock(videoMutex_);
        vTrack = std::move(videoTrack_);
        videoRtpConfig_.reset();
    }
    {
        std::lock_guard aLock(audioMutex_);
        aTrack = std::move(audioTrack_);
        audioRtpConfig_.reset();
    }
    if (vTrack) vTrack->resetCallbacks();
    if (aTrack) aTrack->resetCallbacks();

    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard lock(pcMutex_);
        pc = std::move(pc_);
        pendingCandidates_.clear();
    }
    if (pc) {
        pc->resetCallbacks();
        try {
            pc->close();
        } catch (const std::exception&) {
        }
    }
}

} // namespace oviora::gecit
