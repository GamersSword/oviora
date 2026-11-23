// SPDX-License-Identifier: GPL-3.0-only
#include "WatchReceiver.h"

#include "oviora/Log.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace oviora::remoteplay {
namespace {

void initRtcLoggingOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        rtc::InitLogger(rtc::LogLevel::None, nullptr);
    });
}

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trimCopy(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string normalizeCandidateAttr(std::string cand) {
    if (cand.rfind("a=", 0) == 0) cand.erase(0, 2);
    while (!cand.empty() && (cand.back() == '\r' || cand.back() == '\n' || cand.back() == ' ')) {
        cand.pop_back();
    }
    return cand;
}

bool parseWatchCodec(std::string_view raw, Codec& out) {
    const std::string low = asciiLower(trimCopy(raw));
    if (low == "h264") {
        out = Codec::H264;
        return true;
    }
    if (low == "h265" || low == "hevc" || low == "h265hdr") {
        out = Codec::H265;
        return true;
    }
    return false;
}

std::vector<rtc::IceServer> parseIceServers(const std::vector<std::string>& servers) {
    std::vector<rtc::IceServer> out;
    auto addUrl = [&](const std::string& raw, const std::string& user, const std::string& pass) {
        const std::string url = trimCopy(raw);
        if (url.empty()) return;
        try {
            rtc::IceServer srv(url);
            if (!user.empty()) srv.username = user;
            if (!pass.empty()) srv.password = pass;
            out.push_back(std::move(srv));
        } catch (const std::exception&) {
        }
    };
    for (const auto& entry : servers) {
        const std::string trimmed = trimCopy(entry);
        if (trimmed.empty()) continue;
        if (trimmed.front() == '{') {
            const auto j = nlohmann::json::parse(trimmed, nullptr, false);
            if (!j.is_object()) continue;
            const std::string user = j.value("username", std::string{});
            const std::string pass = j.value("credential", std::string{});
            if (const auto it = j.find("urls"); it != j.end()) {
                if (it->is_string()) {
                    addUrl(it->get<std::string>(), user, pass);
                } else if (it->is_array()) {
                    for (const auto& u : *it) {
                        if (u.is_string()) addUrl(u.get<std::string>(), user, pass);
                    }
                }
            }
        } else {
            addUrl(trimmed, {}, {});
        }
    }
    return out;
}

} // namespace

struct WatchReceiver::Impl : std::enable_shared_from_this<Impl> {
    std::shared_ptr<WatchRoute> route;
    Callbacks callbacks;
    Codec codec = Codec::H264;

    std::atomic<bool> stopping{false};
    std::atomic<bool> failedReported{false};
    std::atomic<bool> disconnected{false};
    std::mutex disconnectMutex;
    std::chrono::steady_clock::time_point disconnectedAt{};

    std::mutex pcMutex;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;
    bool remoteAnswerApplied = false;
    std::vector<rtc::Candidate> pendingCandidates;

    std::mutex pliMutex;
    std::chrono::steady_clock::time_point lastPli{};
    bool hasPli = false;

    std::jthread signalThread;

    void reportFailure(std::string reason) {
        if (stopping.load(std::memory_order_acquire)) return;
        if (failedReported.exchange(true, std::memory_order_acq_rel)) return;
        if (route && route->ended.load(std::memory_order_acquire)) {
            const std::string routeReason = route->getEndReason();
            if (!routeReason.empty()) reason = routeReason;
        }
        if (reason.empty()) {
            reason = "Yayına bağlanılamadı.";
        }
        oviora::log::warn("watch-receiver", std::format("session ended: {}", reason));
        if (callbacks.onFailed) {
            callbacks.onFailed(std::move(reason));
        }
    }

    void handleRemoteJson(const std::string& rawJson) {
        if (stopping.load(std::memory_order_acquire)) return;
        const auto doc = nlohmann::json::parse(rawJson, nullptr, false);
        if (!doc.is_object()) return;

        if (const auto typeIt = doc.find("type"); typeIt != doc.end() && typeIt->is_string()) {
            if (typeIt->get<std::string>() == "answer") {
                const auto sdpIt = doc.find("sdp");
                if (sdpIt == doc.end() || !sdpIt->is_string()) return;
                const std::string sdp = sdpIt->get<std::string>();
                if (sdp.empty()) return;

                std::lock_guard lock(pcMutex);
                if (!pc || stopping.load(std::memory_order_relaxed)) return;
                try {
                    pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
                    remoteAnswerApplied = true;
                    for (const auto& c : pendingCandidates) {
                        try {
                            pc->addRemoteCandidate(c);
                        } catch (const std::exception&) {
                        }
                    }
                    pendingCandidates.clear();
                    oviora::log::info("watch-receiver", "remote SDP answer applied");
                } catch (const std::exception& e) {
                    oviora::log::warn("watch-receiver", std::format("setRemoteDescription failed: {}", e.what()));
                    reportFailure("Yayına bağlanılamadı.");
                }
                return;
            }
        }

        if (const auto candIt = doc.find("candidate"); candIt != doc.end() && candIt->is_string()) {
            const std::string candStr = normalizeCandidateAttr(candIt->get<std::string>());
            if (candStr.empty()) return;
            std::string mid;
            if (const auto m = doc.find("mid"); m != doc.end() && m->is_string()) mid = m->get<std::string>();
            else if (const auto sm = doc.find("sdpMid"); sm != doc.end() && sm->is_string()) mid = sm->get<std::string>();
            if (mid.empty()) mid = "0";

            std::lock_guard lock(pcMutex);
            if (!pc || stopping.load(std::memory_order_relaxed)) return;
            try {
                rtc::Candidate cand(candStr, mid);
                if (remoteAnswerApplied) {
                    pc->addRemoteCandidate(cand);
                } else {
                    pendingCandidates.push_back(std::move(cand));
                }
            } catch (const std::exception&) {
            }
        }
    }

    void signalLoop(std::stop_token st) {
        while (!st.stop_requested() && !stopping.load(std::memory_order_acquire)) {
            if (route) {
                std::string msg;
                while (!st.stop_requested() && !stopping.load(std::memory_order_acquire) && route->popRemote(msg)) {
                    handleRemoteJson(msg);
                }
                if (route->ended.load(std::memory_order_acquire)) {
                    std::string reason = route->getEndReason();
                    if (reason.empty()) reason = "Yayın bitti.";
                    reportFailure(std::move(reason));
                    break;
                }
            }

            if (disconnected.load(std::memory_order_acquire)) {
                std::chrono::steady_clock::time_point since{};
                {
                    std::lock_guard lock(disconnectMutex);
                    since = disconnectedAt;
                }
                if (std::chrono::steady_clock::now() - since >= std::chrono::seconds(5)) {
                    reportFailure("Yayına bağlanılamadı.");
                    break;
                }
            }

            if (!route) break;
            std::unique_lock lock(route->mutex);
            route->cv.wait_for(lock, std::chrono::milliseconds(20));
        }
    }
};

WatchReceiver::~WatchReceiver() {
    stop();
}

bool WatchReceiver::start(std::shared_ptr<WatchRoute> route, Callbacks callbacks) {
    stop();
    if (!route) return false;

    Codec parsedCodec = Codec::H264;
    if (!parseWatchCodec(route->codec, parsedCodec)) {
        oviora::log::warn("watch-receiver", std::format("unsupported watch codec: {}", route->codec));
        return false;
    }

    initRtcLoggingOnce();

    auto impl = std::make_shared<Impl>();
    impl->route = std::move(route);
    impl->callbacks = std::move(callbacks);
    impl->codec = parsedCodec;

    rtc::Configuration cfg;
    cfg.disableAutoNegotiation = true;
    cfg.maxMessageSize = 64 * 1024;
    cfg.iceServers = parseIceServers(impl->route->iceServers);

    std::string offerSdp;
    try {
        auto pc = std::make_shared<rtc::PeerConnection>(cfg);
        std::weak_ptr<Impl> weak = impl;

        pc->onStateChange([weak](rtc::PeerConnection::State state) {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire)) return;
            const char* label = "unknown";
            switch (state) {
            case rtc::PeerConnection::State::New: label = "new"; break;
            case rtc::PeerConnection::State::Connecting: label = "connecting"; break;
            case rtc::PeerConnection::State::Connected:
                label = "connected";
                sp->disconnected.store(false, std::memory_order_release);
                break;
            case rtc::PeerConnection::State::Disconnected:
                label = "disconnected";
                {
                    std::lock_guard lock(sp->disconnectMutex);
                    sp->disconnectedAt = std::chrono::steady_clock::now();
                }
                sp->disconnected.store(true, std::memory_order_release);
                break;
            case rtc::PeerConnection::State::Failed:
                label = "failed";
                sp->reportFailure("Yayına bağlanılamadı.");
                break;
            case rtc::PeerConnection::State::Closed:
                label = "closed";
                sp->reportFailure("Yayına bağlanılamadı.");
                break;
            }
            oviora::log::info("watch-receiver", std::format("pc state={}", label));
        });

        pc->onLocalCandidate([weak](rtc::Candidate cand) {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire) || !sp->route) return;
            const std::string candStr = normalizeCandidateAttr(cand.candidate());
            if (candStr.empty()) return;
            if (sp->route->sendSignal) {
                const nlohmann::json msg = {{"candidate", candStr}, {"mid", cand.mid()}};
                sp->route->sendSignal(msg.dump());
            }
        });

        // Video track (recvonly, broadcast's codec only)
        constexpr std::uint8_t kVideoPt = 96;
        rtc::Description::Video videoDesc("0", rtc::Description::Direction::RecvOnly);
        if (parsedCodec == Codec::H264) {
            videoDesc.addH264Codec(kVideoPt);
        } else {
            videoDesc.addH265Codec(kVideoPt);
        }
        auto videoTrack = pc->addTrack(videoDesc);

        std::shared_ptr<rtc::MediaHandler> videoDepkt = (parsedCodec == Codec::H264)
            ? std::static_pointer_cast<rtc::MediaHandler>(
                  std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence))
            : std::static_pointer_cast<rtc::MediaHandler>(
                  std::make_shared<rtc::H265RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
        videoDepkt->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        videoTrack->setMediaHandler(videoDepkt);

        videoTrack->onOpen([weak]() {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire)) return;
            oviora::log::info("watch-receiver", "video track open");
        });
        videoTrack->onFrame([weak](rtc::binary data, rtc::FrameInfo) {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire) || data.empty()) return;
            if (sp->callbacks.onVideo) {
                sp->callbacks.onVideo(
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()),
                    monotonic100ns());
            }
        });

        // Audio track (recvonly, Opus)
        constexpr std::uint8_t kAudioPt = 111;
        rtc::Description::Audio audioDesc("1", rtc::Description::Direction::RecvOnly);
        audioDesc.addOpusCodec(kAudioPt);
        auto audioTrack = pc->addTrack(audioDesc);

        auto audioDepkt = std::make_shared<rtc::OpusRtpDepacketizer>();
        audioDepkt->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        audioTrack->setMediaHandler(audioDepkt);

        audioTrack->onOpen([weak]() {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire)) return;
            oviora::log::info("watch-receiver", "audio track open");
        });
        audioTrack->onFrame([weak](rtc::binary data, rtc::FrameInfo) {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire) || data.empty()) return;
            if (sp->callbacks.onAudio) {
                sp->callbacks.onAudio(
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
            }
        });

        pc->setLocalDescription(rtc::Description::Type::Offer);
        const auto local = pc->localDescription();
        if (!local) {
            pc->close();
            return false;
        }
        offerSdp = std::string(*local);

        {
            std::lock_guard lock(impl->pcMutex);
            impl->pc = std::move(pc);
            impl->videoTrack = std::move(videoTrack);
            impl->audioTrack = std::move(audioTrack);
        }
    } catch (const std::exception& e) {
        oviora::log::error("watch-receiver", std::format("start failed: {}", e.what()));
        return false;
    }

    impl_ = impl;
    impl->signalThread = std::jthread([impl](std::stop_token st) {
        impl->signalLoop(st);
    });

    oviora::log::info("watch-receiver", std::format("started codec={} iceServers={}",
                                                    parsedCodec == Codec::H264 ? "h264" : "h265",
                                                    cfg.iceServers.size()));

    if (impl->route->sendSignal) {
        const nlohmann::json offerMsg = {{"type", "offer"}, {"sdp", offerSdp}};
        impl->route->sendSignal(offerMsg.dump());
    }
    return true;
}

void WatchReceiver::stop() {
    auto impl = std::move(impl_);
    if (!impl) return;

    impl->stopping.store(true, std::memory_order_release);
    if (impl->route) {
        impl->route->cv.notify_all();
    }
    if (impl->signalThread.joinable()) {
        impl->signalThread.request_stop();
        impl->signalThread.join();
    }

    std::shared_ptr<rtc::Track> vt, at;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard lock(impl->pcMutex);
        vt = std::move(impl->videoTrack);
        at = std::move(impl->audioTrack);
        pc = std::move(impl->pc);
        impl->pendingCandidates.clear();
        impl->remoteAnswerApplied = false;
    }
    if (vt) vt->resetCallbacks();
    if (at) at->resetCallbacks();
    if (pc) {
        pc->resetCallbacks();
        try {
            pc->close();
        } catch (const std::exception&) {
        }
    }
}

void WatchReceiver::requestKeyframe() {
    auto impl = impl_;
    if (!impl || impl->stopping.load(std::memory_order_acquire)) return;

    {
        std::lock_guard lock(impl->pliMutex);
        const auto now = std::chrono::steady_clock::now();
        if (impl->hasPli && (now - impl->lastPli) < std::chrono::seconds(2)) {
            return;
        }
        impl->hasPli = true;
        impl->lastPli = now;
    }

    std::shared_ptr<rtc::Track> vt;
    {
        std::lock_guard lock(impl->pcMutex);
        vt = impl->videoTrack;
    }
    if (vt && vt->isOpen()) {
        try {
            vt->requestKeyframe();
            oviora::log::info("watch-receiver", "sent RTCP PLI keyframe request");
        } catch (const std::exception&) {
        }
    }
}

} // namespace oviora::remoteplay
