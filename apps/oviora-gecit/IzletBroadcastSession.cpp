// SPDX-License-Identifier: GPL-3.0-only
#include "IzletBroadcastSession.h"

#include "DataChannelProtocol.h"
#include "WebRtcGatewaySession.h"
#include "oviora/Log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oviora::gecit {
namespace {

class GecitSleepTimer {
public:
#ifdef _WIN32
    GecitSleepTimer() {
        handle_ = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!handle_) {
            handle_ = ::CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
    }
    ~GecitSleepTimer() {
        if (handle_) ::CloseHandle(handle_);
    }
    GecitSleepTimer(const GecitSleepTimer&) = delete;
    GecitSleepTimer& operator=(const GecitSleepTimer&) = delete;

    void waitMicroseconds(std::int64_t usec) const {
        if (usec <= 0) return;
        if (handle_) {
            LARGE_INTEGER due{};
            due.QuadPart = -(usec * 10LL);
            if (::SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE)) {
                ::WaitForSingleObject(handle_, INFINITE);
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(usec));
    }

private:
    HANDLE handle_ = nullptr;
#else
    void waitMicroseconds(std::int64_t usec) const {
        if (usec > 0) std::this_thread::sleep_for(std::chrono::microseconds(usec));
    }
#endif
};

constexpr std::size_t kMaxViewerVideoQueueFrames = 180;
constexpr std::size_t kMaxViewerVideoQueueBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxViewerAudioQueuePackets = 100;

std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string normalizeCandidateAttr(std::string cand) {
    if (cand.rfind("a=", 0) == 0) cand.erase(0, 2);
    while (!cand.empty() && (cand.back() == '\r' || cand.back() == '\n' || cand.back() == ' ')) {
        cand.pop_back();
    }
    return cand;
}

void stripOtherPayloadTypes(rtc::Description::Media& media, int keepPt) {
    const auto allPts = media.payloadTypes();
    for (int pt : allPts) {
        if (pt != keepPt && media.hasPayloadType(pt)) {
            media.removeRtpMap(pt);
        }
    }
}

struct QueuedViewerVideo {
    std::uint64_t seq = 0;
    double mediaTimeSec = 0.0;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    bool isIdr = false;
    bool isPliReplay = false;
};

struct QueuedViewerAudio {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::uint32_t frameSamples = 960;
};

} // namespace

struct IzletBroadcastSession::Viewer {
    explicit Viewer(std::string viewerIid)
        : iid(std::move(viewerIid)), shortIid(shortWsid(iid)) {}

    std::string iid;
    std::string shortIid;

    std::atomic<bool> closed{false};
    std::atomic<bool> videoReady{false};
    std::atomic<bool> canliSent{false};
    std::atomic<bool> needsIdr{false};
    std::atomic<std::uint64_t> bytesSent{0};

    std::mutex pcMutex;
    std::shared_ptr<rtc::PeerConnection> pc;
    bool remoteOfferApplied = false;
    std::vector<rtc::Candidate> pendingCandidates;

    std::mutex trackMutex;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;

    std::mutex pliMutex;
    std::chrono::steady_clock::time_point lastPli{};
    bool hasPli = false;
    std::uint64_t lastPliReplayedIdrSeq = 0;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    bool waitingForIdr = true;
    std::uint64_t lastEnqueuedSeq = 0;
    std::deque<QueuedViewerVideo> videoQueue;
    std::size_t videoQueueBytes = 0;
    std::deque<QueuedViewerAudio> audioQueue;
    std::jthread senderThread;

    void enqueueVideoLocked(const IzletGopBuffer::Frame& f, bool isPliReplay) {
        if (!f.bytes || f.bytes->empty()) return;
        if (waitingForIdr && !f.isIdr) {
            return;
        }

        if (videoQueue.size() >= kMaxViewerVideoQueueFrames ||
            videoQueueBytes + f.bytes->size() > kMaxViewerVideoQueueBytes) {
            // Drop older frames for this slow viewer only; keep from the latest IDR in queue if present.
            auto latestIdr = std::find_if(videoQueue.rbegin(), videoQueue.rend(),
                                          [](const QueuedViewerVideo& q) { return q.isIdr; });
            if (latestIdr != videoQueue.rend() && !f.isIdr) {
                auto eraseEnd = latestIdr.base() - 1;
                for (auto it = videoQueue.begin(); it != eraseEnd; ++it) {
                    videoQueueBytes -= it->bytes ? it->bytes->size() : 0;
                }
                videoQueue.erase(videoQueue.begin(), eraseEnd);
            } else {
                videoQueue.clear();
                videoQueueBytes = 0;
                if (!f.isIdr) {
                    waitingForIdr = true;
                    needsIdr.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        }

        waitingForIdr = false;
        videoQueueBytes += f.bytes->size();
        videoQueue.push_back(QueuedViewerVideo{f.seq, f.mediaTimeSec, f.bytes, f.isIdr, isPliReplay});
        queueCv.notify_one();
    }
};

IzletBroadcastSession::IzletBroadcastSession(std::string yid,
                                             IzletCodec codec,
                                             const GatewayConfig& config,
                                             IzletBroadcastCallbacks callbacks)
    : yid_(std::move(yid)),
      shortYid_(shortWsid(yid_)),
      codec_(codec),
      udpBegin_(config.udpBegin),
      udpEnd_(config.udpEnd),
      stunServers_(config.stunServers),
      callbacks_(std::move(callbacks)),
      gopBuffer_(codec) {}

IzletBroadcastSession::~IzletBroadcastSession() {
    closeAll(false);
}

bool IzletBroadcastSession::ensurePublisherPc(std::string* error) {
    std::lock_guard lock(pubPcMutex_);
    if (pubPc_) return true;
    if (closed_.load()) {
        if (error) *error = "yayin kapatildi";
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
        pubPc_ = std::make_shared<rtc::PeerConnection>(cfg);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }

    std::weak_ptr<IzletBroadcastSession> weak = shared_from_this();

    pubPc_->onStateChange([weak](rtc::PeerConnection::State state) {
        auto self = weak.lock();
        if (!self || self->closed_.load()) return;
        const char* label = "unknown";
        switch (state) {
        case rtc::PeerConnection::State::New: label = "new"; break;
        case rtc::PeerConnection::State::Connecting: label = "connecting"; break;
        case rtc::PeerConnection::State::Connected: label = "connected"; break;
        case rtc::PeerConnection::State::Disconnected: label = "disconnected"; break;
        case rtc::PeerConnection::State::Failed: label = "failed"; break;
        case rtc::PeerConnection::State::Closed: label = "closed"; break;
        }
        oviora::log::info("izlet-sfu", std::format("yid={} pub state={}", self->shortYid_, label));
        if (state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed ||
            state == rtc::PeerConnection::State::Disconnected) {
            if (self->callbacks_.onPublisherEnded) {
                self->callbacks_.onPublisherEnded(self->yid_, label);
            }
        }
    });

    pubPc_->onLocalCandidate([weak](rtc::Candidate cand) {
        auto self = weak.lock();
        if (!self || self->closed_.load()) return;
        const std::string candStr = normalizeCandidateAttr(cand.candidate());
        if (candStr.empty()) return;
        if (self->callbacks_.onPublisherSignal) {
            const nlohmann::json msg = {{"candidate", candStr}, {"mid", cand.mid()}};
            self->callbacks_.onPublisherSignal(self->yid_, msg.dump());
        }
    });

    pubPc_->onTrack([weak](std::shared_ptr<rtc::Track> track) {
        auto self = weak.lock();
        if (!self || self->closed_.load() || !track) return;
        self->onPublisherTrack(track);
    });

    pubPc_->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> dc) {
        auto self = weak.lock();
        if (!self || self->closed_.load() || !dc) return;
        self->onPublisherDataChannel(dc);
    });

    return true;
}

namespace {
// Watches the player's video RTP sequence numbers (before the depacketizer). A gap still open after 100 ms
// (not a late or reordered packet) is a loss: reported to the session.
class IngestLossWatch final : public rtc::MediaHandler {
public:
    explicit IngestLossWatch(std::weak_ptr<IzletBroadcastSession> session) : session_(std::move(session)) {
        // Tests only: OVIORA_GECIT_DENEME_KAYIP=<n> drops one video packet in n (a lossy line from the player)
        char v[16] = {};
        std::size_t n = 0;
        if (getenv_s(&n, v, sizeof v, "OVIORA_GECIT_DENEME_KAYIP") == 0 && n > 1) dropEvery_ = std::strtoul(v, nullptr, 10);
    }
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        const auto now = std::chrono::steady_clock::now();
        unsigned lost = 0;
        if (dropEvery_ > 1)
            std::erase_if(messages, [this](const rtc::message_ptr& m) {
                return m && m->size() >= 12 && (std::uint8_t(m->at(1)) & 0x7Fu) >= 96 && ++dropCount_ % dropEvery_ == 0;
            });
        for (const auto& m : messages) {
            if (!m || m->type != rtc::Message::Binary || m->size() < 12) continue;
            const auto* b = reinterpret_cast<const std::uint8_t*>(m->data());
            if ((b[0] >> 6) != 2) continue;
            const unsigned pt = b[1] & 0x7Fu;
            if (pt >= 64 && pt <= 95) continue;  // RTCP (200..207 with the marker bit folded in)
            const std::uint16_t seq = std::uint16_t((b[2] << 8) | b[3]);
            if (!started_) {
                started_ = true;
                highest_ = seq;
                continue;
            }
            const auto diff = std::int16_t(std::uint16_t(seq - highest_));
            if (diff > 0) {
                if (diff > 1 && diff < 1000)
                    for (std::uint16_t s = std::uint16_t(highest_ + 1); s != seq && missing_.size() < 2000; ++s)
                        missing_.push_back({s, now});
                highest_ = seq;
            } else {
                // Late or reordered: no longer missing
                for (auto it = missing_.begin(); it != missing_.end(); ++it)
                    if (it->seq == seq) { missing_.erase(it); break; }
            }
        }
        while (!missing_.empty() && now - missing_.front().at > std::chrono::milliseconds(150)) {
            missing_.erase(missing_.begin());
            ++lost;
        }
        if (lost)
            if (auto s = session_.lock()) s->onIngestLoss(lost);
    }

private:
    struct Missing { std::uint16_t seq; std::chrono::steady_clock::time_point at; };
    std::weak_ptr<IzletBroadcastSession> session_;
    bool started_ = false;
    std::uint16_t highest_ = 0;
    std::vector<Missing> missing_;
    unsigned long dropEvery_ = 0, dropCount_ = 0;
};
}

void IzletBroadcastSession::onIngestLoss(unsigned lost) {
    if (closed_.load()) return;
    const std::uint64_t total = ingestLost_.fetch_add(lost) + lost;
    if (!ingestBroken_.exchange(true)) {
        oviora::log::info("izlet-sfu", std::format("yid={} {} video packet(s) lost from the player (total {}): holding viewers until a key frame",
                                                  shortYid_, lost, total));
        std::lock_guard lock(mediaMutex_);
        gopBuffer_.invalidateGop();
    }
    requestPublisherIdr();
}

void IzletBroadcastSession::onPublisherTrack(const std::shared_ptr<rtc::Track>& track) {
    auto desc = track->description();
    const std::string mediaType = asciiLower(desc.type());
    std::weak_ptr<IzletBroadcastSession> weak = shared_from_this();

    if (mediaType == "video") {
        const int pt = (codec_ == IzletCodec::H264)
            ? WebRtcGatewaySession::selectH264PayloadType(desc)
            : WebRtcGatewaySession::selectH265PayloadType(desc);
        if (pt < 0) {
            oviora::log::warn("izlet-sfu", std::format("yid={} publisher offer missing {}",
                                                      shortYid_, izletCodecName(codec_)));
            desc.markRemoved();
            track->setDescription(desc);
            return;
        }

        stripOtherPayloadTypes(desc, pt);
        desc.setDirection(rtc::Description::Direction::RecvOnly);
        track->setDescription(desc);

        std::shared_ptr<rtc::MediaHandler> depkt = (codec_ == IzletCodec::H264)
            ? std::static_pointer_cast<rtc::MediaHandler>(
                  std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence))
            : std::static_pointer_cast<rtc::MediaHandler>(
                  std::make_shared<rtc::H265RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
        depkt->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        depkt->addToChain(std::make_shared<IngestLossWatch>(weak));
        track->setMediaHandler(depkt);

        track->onOpen([weak, shortYid = shortYid_]() {
            oviora::log::info("izlet-sfu", std::format("yid={} pub video track open", shortYid));
        });
        track->onFrame([weak](rtc::binary data, rtc::FrameInfo info) {
            auto self = weak.lock();
            if (!self || self->closed_.load()) return;
            self->onPublisherVideoFrame(data, info);
        });

        {
            std::lock_guard lock(pubTrackMutex_);
            pubVideoTrack_ = track;
        }
        oviora::log::info("izlet-sfu", std::format("yid={} pub video codec={} pt={}",
                                                  shortYid_, izletCodecName(codec_), pt));
    } else if (mediaType == "audio") {
        const int pt = WebRtcGatewaySession::selectOpusPayloadType(desc);
        if (pt < 0) {
            desc.markRemoved();
            track->setDescription(desc);
            return;
        }

        stripOtherPayloadTypes(desc, pt);
        desc.setDirection(rtc::Description::Direction::RecvOnly);
        track->setDescription(desc);

        auto depkt = std::make_shared<rtc::OpusRtpDepacketizer>();
        depkt->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        track->setMediaHandler(depkt);

        track->onFrame([weak](rtc::binary data, rtc::FrameInfo info) {
            auto self = weak.lock();
            if (!self || self->closed_.load()) return;
            self->onPublisherAudioFrame(data, info);
        });

        {
            std::lock_guard lock(pubTrackMutex_);
            pubAudioTrack_ = track;
        }
        oviora::log::info("izlet-sfu", std::format("yid={} pub audio Opus pt={}", shortYid_, pt));
    } else {
        desc.markRemoved();
        track->setDescription(desc);
    }
}

void IzletBroadcastSession::onPublisherDataChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    if (dc->label() != "kontrol") return;
    {
        std::lock_guard lock(pubTrackMutex_);
        pubKontrolDc_ = dc;
    }
    std::weak_ptr<IzletBroadcastSession> weak = shared_from_this();
    dc->onOpen([weak]() {
        auto self = weak.lock();
        if (!self || self->closed_.load()) return;
        bool sendNow = false;
        {
            std::lock_guard lock(self->idrMutex_);
            sendNow = std::exchange(self->pendingKontrolIdr_, false);
        }
        if (sendNow) {
            std::shared_ptr<rtc::DataChannel> d;
            {
                std::lock_guard pLock(self->pubTrackMutex_);
                d = self->pubKontrolDc_;
            }
            if (d && d->isOpen()) {
                try {
                    d->send(makeKontrolIdrJson());
                } catch (const std::exception&) {
                }
            }
        }
    });
}

void IzletBroadcastSession::requestPublisherIdr() {
    if (closed_.load()) return;
    {
        std::lock_guard lock(idrMutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto cooldown = (consecutiveIdrReqs_ == 0)
            ? std::chrono::milliseconds(2000)
            : (consecutiveIdrReqs_ == 1 ? std::chrono::milliseconds(4000) : std::chrono::milliseconds(6000));
        if (hasIdrReq_ && (now - lastIdrReq_) < cooldown) {
            return;
        }
        hasIdrReq_ = true;
        lastIdrReq_ = now;
        if (consecutiveIdrReqs_ < 10) {
            ++consecutiveIdrReqs_;
        }
    }

    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> vt;
    {
        std::lock_guard lock(pubTrackMutex_);
        dc = pubKontrolDc_;
        vt = pubVideoTrack_;
    }

    bool sentDc = false;
    if (dc && dc->isOpen()) {
        try {
            dc->send(makeKontrolIdrJson());
            sentDc = true;
        } catch (const std::exception&) {
        }
    }
    if (!sentDc) {
        std::lock_guard lock(idrMutex_);
        pendingKontrolIdr_ = true;
        if (vt) {
            try {
                vt->requestKeyframe();
            } catch (const std::exception&) {
            }
        }
    }
}

void IzletBroadcastSession::onPublisherVideoFrame(const rtc::binary& data, const rtc::FrameInfo&) {
    if (closed_.load() || data.empty()) return;
    const std::uint64_t totalPubBytes = pubBytesTotal_.fetch_add(data.size(), std::memory_order_relaxed) + data.size();

    const std::span<const std::uint8_t> span(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());

    // After a loss only a key frame goes on (the ones before it would show as smeared blocks)
    if (ingestBroken_.load()) {
        if (!inspectIzletAnnexB(span, codec_).isIdr) {
            requestPublisherIdr();
            return;
        }
        ingestBroken_.store(false);
        {
            std::lock_guard lock(idrMutex_);
            consecutiveIdrReqs_ = 0;
        }
        oviora::log::info("izlet-sfu", std::format("yid={} key frame after the loss: viewers go on", shortYid_));
    }

    bool triggerIdr = false;
    bool gotIdr = false;
    {
        std::lock_guard lock(mediaMutex_);
        const auto now = std::chrono::steady_clock::now();
        const double rateSec = std::chrono::duration<double>(now - lastRateSampleAt_).count();
        if (rateSec >= 1.0) {
            const std::uint64_t delta = totalPubBytes >= lastRateSamplePubBytes_ ? (totalPubBytes - lastRateSamplePubBytes_) : 0;
            estimatedPubMbps_.store((double(delta) * 8.0 / 1000000.0) / rateSec, std::memory_order_relaxed);
            lastRateSampleAt_ = now;
            lastRateSamplePubBytes_ = totalPubBytes;
        }
        if (!broadcastOriginSet_) {
            broadcastOrigin_ = now;
            broadcastOriginSet_ = true;
        }
        double mediaSec = std::chrono::duration<double>(now - broadcastOrigin_).count();
        if (mediaSec <= lastVideoTimeSec_) {
            mediaSec = lastVideoTimeSec_ + 0.0001;
        }
        lastVideoTimeSec_ = mediaSec;

        const auto frame = gopBuffer_.pushAccessUnit(span, mediaSec);
        if (!frame) return;
        gotIdr = frame->isIdr;

        if (!pubCanliSent_.exchange(true)) {
            oviora::log::info("izlet-sfu", std::format("yid={} first video frame received (canli)", shortYid_));
            if (callbacks_.onPublisherState) {
                callbacks_.onPublisherState(yid_, "canli", "");
            }
        }

        for (const auto& [_, viewer] : viewers_) {
            if (viewer->closed.load(std::memory_order_relaxed) ||
                !viewer->videoReady.load(std::memory_order_acquire)) {
                continue;
            }
            std::lock_guard vLock(viewer->queueMutex);
            if (frame->seq <= viewer->lastEnqueuedSeq) continue;
            viewer->lastEnqueuedSeq = frame->seq;
            viewer->enqueueVideoLocked(*frame, false);
            if (viewer->needsIdr.exchange(false, std::memory_order_relaxed)) {
                triggerIdr = true;
            }
        }
    }

    if (gotIdr) {
        std::lock_guard lock(idrMutex_);
        consecutiveIdrReqs_ = 0;
    }

    if (triggerIdr) {
        requestPublisherIdr();
    }
}

void IzletBroadcastSession::onPublisherAudioFrame(const rtc::binary& data, const rtc::FrameInfo& info) {
    if (closed_.load() || data.empty()) return;
    pubBytesTotal_.fetch_add(data.size(), std::memory_order_relaxed);

    auto bytes = std::make_shared<std::vector<std::uint8_t>>(data.size());
    std::memcpy(bytes->data(), data.data(), data.size());

    std::lock_guard lock(mediaMutex_);
    std::uint32_t frameSamples = 960;
    if (hasAudioTs_) {
        const std::uint32_t delta = info.timestamp - lastAudioRtpTs_;
        if (delta >= 120 && delta <= 2880) {
            frameSamples = delta;
        }
    }
    hasAudioTs_ = true;
    lastAudioRtpTs_ = info.timestamp;

    for (const auto& [_, viewer] : viewers_) {
        if (viewer->closed.load(std::memory_order_relaxed) ||
            !viewer->videoReady.load(std::memory_order_acquire)) {
            continue;
        }
        std::lock_guard vLock(viewer->queueMutex);
        while (viewer->audioQueue.size() >= kMaxViewerAudioQueuePackets) {
            viewer->audioQueue.pop_front();
        }
        viewer->audioQueue.push_back(QueuedViewerAudio{bytes, frameSamples});
        viewer->queueCv.notify_one();
    }
}

bool IzletBroadcastSession::handlePublisherSignal(const nlohmann::json& data, std::string* error) {
    if (!data.is_object()) {
        if (error) *error = "gecersiz sinyal JSON";
        return false;
    }
    if (!ensurePublisherPc(error)) return false;

    if (const auto typeIt = data.find("type"); typeIt != data.end() && typeIt->is_string()) {
        if (typeIt->get<std::string>() == "offer") {
            const auto sdpIt = data.find("sdp");
            if (sdpIt == data.end() || !sdpIt->is_string()) {
                if (error) *error = "offer sdp eksik";
                return false;
            }
            const std::string offerSdp = sdpIt->get<std::string>();
            std::string answerSdp;
            {
                std::lock_guard lock(pubPcMutex_);
                if (!pubPc_ || closed_.load()) return false;
                try {
                    pubPc_->setRemoteDescription(rtc::Description(offerSdp, rtc::Description::Type::Offer));
                    pubOfferApplied_ = true;
                    for (const auto& c : pubPendingCandidates_) {
                        try {
                            pubPc_->addRemoteCandidate(c);
                        } catch (const std::exception&) {
                        }
                    }
                    pubPendingCandidates_.clear();
                    pubPc_->setLocalDescription(rtc::Description::Type::Answer);
                    const auto local = pubPc_->localDescription();
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
            if (callbacks_.onPublisherSignal) {
                const nlohmann::json msg = {{"type", "answer"}, {"sdp", answerSdp}};
                callbacks_.onPublisherSignal(yid_, msg.dump());
            }
            oviora::log::info("izlet-sfu", std::format("yid={} pub SDP answer sent", shortYid_));
            return true;
        }
    }

    if (const auto candIt = data.find("candidate"); candIt != data.end() && candIt->is_string()) {
        const std::string candStr = normalizeCandidateAttr(candIt->get<std::string>());
        if (candStr.empty()) return true;
        std::string mid;
        if (const auto m = data.find("mid"); m != data.end() && m->is_string()) mid = m->get<std::string>();
        else if (const auto sm = data.find("sdpMid"); sm != data.end() && sm->is_string()) mid = sm->get<std::string>();
        if (mid.empty()) mid = "0";

        std::lock_guard lock(pubPcMutex_);
        if (!pubPc_ || closed_.load()) return false;
        try {
            rtc::Candidate cand(candStr, mid);
            if (pubOfferApplied_) {
                pubPc_->addRemoteCandidate(cand);
            } else {
                pubPendingCandidates_.push_back(std::move(cand));
            }
        } catch (const std::exception&) {
        }
        return true;
    }

    return true;
}

bool IzletBroadcastSession::addViewer(const std::string& iid, std::string* error) {
    if (closed_.load()) {
        if (error) *error = "Yayın sona erdi.";
        return false;
    }

    auto viewer = std::make_shared<Viewer>(iid);
    {
        std::lock_guard lock(mediaMutex_);
        if (viewers_.contains(iid)) {
            return true;
        }
        if (viewers_.size() >= IzletGopBuffer::kMaxViewersPerBroadcast) {
            if (error) *error = "Yayın izleyici kapasitesi dolu (en fazla 20 izleyici).";
            return false;
        }
        viewers_.emplace(iid, viewer);
    }

    std::weak_ptr<IzletBroadcastSession> weakSelf = shared_from_this();
    viewer->senderThread = std::jthread([weakSelf, viewer](std::stop_token st) {
        GecitSleepTimer sleepTimer;
        bool videoTsOriginSet = false;
        double videoTsOrigin = 0.0;
        double lastSentVideoSec = -0.001;
        std::uint64_t audioSamplesSent = 0;

        while (!st.stop_requested() && !viewer->closed.load(std::memory_order_acquire)) {
            std::deque<QueuedViewerVideo> vBatch;
            std::deque<QueuedViewerAudio> aBatch;
            {
                std::unique_lock lock(viewer->queueMutex);
                viewer->queueCv.wait(lock, [&] {
                    return st.stop_requested() || viewer->closed.load(std::memory_order_relaxed) ||
                           !viewer->videoQueue.empty() || !viewer->audioQueue.empty();
                });
                if (st.stop_requested() || viewer->closed.load(std::memory_order_relaxed)) break;
                vBatch.swap(viewer->videoQueue);
                viewer->videoQueueBytes = 0;
                aBatch.swap(viewer->audioQueue);
            }

            std::shared_ptr<rtc::Track> vTrack, aTrack;
            {
                std::lock_guard pLock(viewer->trackMutex);
                vTrack = viewer->videoTrack;
                aTrack = viewer->audioTrack;
            }

            const bool catchUpReplay = vBatch.size() > 2;
            for (std::size_t idx = 0; idx < vBatch.size(); ++idx) {
                const auto& vf = vBatch[idx];
                if (st.stop_requested() || viewer->closed.load(std::memory_order_relaxed)) break;
                if (!vTrack || !vTrack->isOpen() || !vf.bytes || vf.bytes->empty()) continue;

                if (!videoTsOriginSet) {
                    videoTsOrigin = vf.mediaTimeSec;
                    videoTsOriginSet = true;
                }
                double relSec = vf.mediaTimeSec - videoTsOrigin;
                if (relSec <= lastSentVideoSec) {
                    relSec = lastSentVideoSec + 0.001;
                }
                lastSentVideoSec = relSec;

                rtc::binary payload(vf.bytes->size());
                std::memcpy(payload.data(), vf.bytes->data(), vf.bytes->size());
                try {
                    vTrack->sendFrame(std::move(payload), std::chrono::duration<double>(relSec));
                    viewer->bytesSent.fetch_add(vf.bytes->size(), std::memory_order_relaxed);
                    if (!viewer->canliSent.exchange(true)) {
                        if (auto self = weakSelf.lock(); self && self->callbacks_.onViewerState) {
                            self->callbacks_.onViewerState(viewer->iid, "canli", "");
                        }
                    }
                } catch (const std::exception&) {
                }
                if (catchUpReplay && idx + 1 < vBatch.size()) {
                    sleepTimer.waitMicroseconds(1500);
                }
            }

            for (const auto& ap : aBatch) {
                if (st.stop_requested() || viewer->closed.load(std::memory_order_relaxed)) break;
                const std::uint32_t step = ap.frameSamples > 0 ? ap.frameSamples : 960u;
                const std::uint64_t pos = audioSamplesSent;
                audioSamplesSent += step;
                if (!aTrack || !aTrack->isOpen() || !ap.bytes || ap.bytes->empty()) continue;

                rtc::binary payload(ap.bytes->size());
                std::memcpy(payload.data(), ap.bytes->data(), ap.bytes->size());
                try {
                    aTrack->sendFrame(std::move(payload), std::chrono::duration<double>(double(pos) / 48000.0));
                    viewer->bytesSent.fetch_add(ap.bytes->size(), std::memory_order_relaxed);
                } catch (const std::exception&) {
                }
            }
        }
    });

    oviora::log::info("izlet-sfu", std::format("yid={} viewer joined iid={}", shortYid_, viewer->shortIid));
    return true;
}

bool IzletBroadcastSession::ensureViewerPc(const std::shared_ptr<Viewer>& viewer, std::string* error) {
    std::lock_guard lock(viewer->pcMutex);
    if (viewer->pc) return true;
    if (viewer->closed.load() || closed_.load()) {
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
        viewer->pc = std::make_shared<rtc::PeerConnection>(cfg);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }

    std::weak_ptr<IzletBroadcastSession> weakSelf = shared_from_this();
    std::weak_ptr<Viewer> weakViewer = viewer;

    viewer->pc->onStateChange([weakSelf, weakViewer](rtc::PeerConnection::State state) {
        auto self = weakSelf.lock();
        auto v = weakViewer.lock();
        if (!self || !v || self->closed_.load() || v->closed.load()) return;
        const char* label = "unknown";
        switch (state) {
        case rtc::PeerConnection::State::New: label = "new"; break;
        case rtc::PeerConnection::State::Connecting: label = "connecting"; break;
        case rtc::PeerConnection::State::Connected: label = "connected"; break;
        case rtc::PeerConnection::State::Disconnected: label = "disconnected"; break;
        case rtc::PeerConnection::State::Failed: label = "failed"; break;
        case rtc::PeerConnection::State::Closed: label = "closed"; break;
        }
        oviora::log::info("izlet-sfu", std::format("yid={} iid={} state={}", self->shortYid_, v->shortIid, label));
        if (state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed ||
            state == rtc::PeerConnection::State::Disconnected) {
            if (self->callbacks_.onViewerEnded) {
                self->callbacks_.onViewerEnded(self->yid_, v->iid, label);
            }
        }
    });

    viewer->pc->onLocalCandidate([weakSelf, weakViewer](rtc::Candidate cand) {
        auto self = weakSelf.lock();
        auto v = weakViewer.lock();
        if (!self || !v || self->closed_.load() || v->closed.load()) return;
        const std::string candStr = normalizeCandidateAttr(cand.candidate());
        if (candStr.empty()) return;
        if (self->callbacks_.onViewerSignal) {
            const nlohmann::json msg = {{"candidate", candStr}, {"mid", cand.mid()}};
            self->callbacks_.onViewerSignal(v->iid, msg.dump());
        }
    });

    viewer->pc->onTrack([weakSelf, weakViewer](std::shared_ptr<rtc::Track> track) {
        auto self = weakSelf.lock();
        auto v = weakViewer.lock();
        if (!self || !v || self->closed_.load() || v->closed.load() || !track) return;
        self->onViewerTrack(v, track);
    });

    return true;
}

void IzletBroadcastSession::onViewerTrack(const std::shared_ptr<Viewer>& viewer,
                                          const std::shared_ptr<rtc::Track>& track) {
    auto desc = track->description();
    const std::string mediaType = asciiLower(desc.type());
    std::weak_ptr<IzletBroadcastSession> weakSelf = shared_from_this();
    std::weak_ptr<Viewer> weakViewer = viewer;

    if (mediaType == "video") {
        const int pt = (codec_ == IzletCodec::H264)
            ? WebRtcGatewaySession::selectH264PayloadType(desc)
            : WebRtcGatewaySession::selectH265PayloadType(desc);
        if (pt < 0) {
            oviora::log::warn("izlet-sfu", std::format("yid={} iid={} viewer missing {}",
                                                      shortYid_, viewer->shortIid, izletCodecName(codec_)));
            desc.markRemoved();
            track->setDescription(desc);
            if (callbacks_.onViewerState) {
                callbacks_.onViewerState(viewer->iid, "hata", "Tarayıcı bu yayın codec'ini desteklemiyor.");
            }
            return;
        }

        stripOtherPayloadTypes(desc, pt);
        desc.setDirection(rtc::Description::Direction::SendOnly);
        constexpr rtc::SSRC kVideoSsrc = 0x56494432u;
        desc.addSSRC(kVideoSsrc, "oviora-izlet-v", "oviora-izlet", "video0");
        track->setDescription(desc);

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc, "oviora-izlet-v", static_cast<std::uint8_t>(pt), rtc::RtpPacketizer::VideoClockRate);
        std::shared_ptr<rtc::RtpPacketizer> packetizer;
        if (codec_ == IzletCodec::H264) {
            packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, rtpConfig);
        } else {
            packetizer = std::make_shared<rtc::H265RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, rtpConfig);
        }
        auto sr = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
        auto nack = std::make_shared<rtc::RtcpNackResponder>(4096);
        auto pli = std::make_shared<rtc::PliHandler>([weakSelf, weakViewer]() {
            auto self = weakSelf.lock();
            auto v = weakViewer.lock();
            if (!self || !v || self->closed_.load() || v->closed.load()) return;
            self->onViewerPli(v);
        });
        packetizer->addToChain(sr);
        packetizer->addToChain(nack);
        packetizer->addToChain(pli);
        track->setMediaHandler(packetizer);

        track->onOpen([weakSelf, weakViewer]() {
            auto self = weakSelf.lock();
            auto v = weakViewer.lock();
            if (!self || !v || self->closed_.load() || v->closed.load()) return;
            self->onViewerVideoReady(v);
        });

        {
            std::lock_guard lock(viewer->trackMutex);
            viewer->videoTrack = track;
        }
    } else if (mediaType == "audio") {
        const int pt = WebRtcGatewaySession::selectOpusPayloadType(desc);
        if (pt < 0) {
            desc.markRemoved();
            track->setDescription(desc);
            return;
        }

        stripOtherPayloadTypes(desc, pt);
        desc.setDirection(rtc::Description::Direction::SendOnly);
        constexpr rtc::SSRC kAudioSsrc = 0x41554432u;
        desc.addSSRC(kAudioSsrc, "oviora-izlet-a", "oviora-izlet", "audio0");
        track->setDescription(desc);

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            kAudioSsrc, "oviora-izlet-a", static_cast<std::uint8_t>(pt), rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
        auto sr = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
        auto nack = std::make_shared<rtc::RtcpNackResponder>();
        packetizer->addToChain(sr);
        packetizer->addToChain(nack);
        track->setMediaHandler(packetizer);

        {
            std::lock_guard lock(viewer->trackMutex);
            viewer->audioTrack = track;
        }
    } else {
        desc.markRemoved();
        track->setDescription(desc);
    }
}

void IzletBroadcastSession::onViewerVideoReady(const std::shared_ptr<Viewer>& viewer) {
    bool needIdr = false;
    std::size_t replayed = 0;
    {
        std::lock_guard mLock(mediaMutex_);
        const auto snap = gopBuffer_.snapshotGop();
        std::lock_guard pLock(viewer->pliMutex);
        std::lock_guard qLock(viewer->queueMutex);
        viewer->videoReady.store(true, std::memory_order_release);
        if (snap.valid && !snap.frames.empty()) {
            viewer->lastPliReplayedIdrSeq = snap.idrSeq;
            viewer->waitingForIdr = false;
            for (const auto& f : snap.frames) {
                viewer->enqueueVideoLocked(f, false);
                viewer->lastEnqueuedSeq = f.seq;
            }
            replayed = snap.frames.size();
        } else {
            viewer->waitingForIdr = true;
            needIdr = true;
        }
    }
    oviora::log::info("izlet-sfu", std::format("yid={} iid={} video track open gopFrames={} needIdr={}",
                                              shortYid_, viewer->shortIid, replayed, needIdr));
    if (needIdr) {
        requestPublisherIdr();
    }
}

void IzletBroadcastSession::onViewerPli(const std::shared_ptr<Viewer>& viewer) {
    {
        std::lock_guard pLock(viewer->pliMutex);
        const auto now = std::chrono::steady_clock::now();
        if (viewer->hasPli && (now - viewer->lastPli) < std::chrono::milliseconds(500)) {
            return;
        }
        viewer->hasPli = true;
        viewer->lastPli = now;
    }

    bool needPublisherIdr = false;
    {
        std::lock_guard mLock(mediaMutex_);
        const auto snap = gopBuffer_.snapshotGop();
        std::lock_guard pLock(viewer->pliMutex);
        if (snap.valid && !snap.frames.empty() && snap.idrSeq != viewer->lastPliReplayedIdrSeq) {
            viewer->lastPliReplayedIdrSeq = snap.idrSeq;
            std::lock_guard qLock(viewer->queueMutex);
            viewer->waitingForIdr = false;
            for (const auto& f : snap.frames) {
                viewer->enqueueVideoLocked(f, true);
                if (f.seq > viewer->lastEnqueuedSeq) viewer->lastEnqueuedSeq = f.seq;
            }
        } else {
            std::lock_guard qLock(viewer->queueMutex);
            if (!snap.valid || snap.frames.empty()) {
                viewer->waitingForIdr = true;
            }
            needPublisherIdr = true;
        }
    }
    if (needPublisherIdr) {
        requestPublisherIdr();
    }
}

bool IzletBroadcastSession::handleViewerSignal(const std::string& iid,
                                               const nlohmann::json& data,
                                               std::string* error) {
    if (!data.is_object()) {
        if (error) *error = "gecersiz sinyal JSON";
        return false;
    }
    std::shared_ptr<Viewer> viewer;
    {
        std::lock_guard lock(mediaMutex_);
        const auto it = viewers_.find(iid);
        if (it == viewers_.end()) {
            if (error) *error = "izleyici bulunamadi";
            return false;
        }
        viewer = it->second;
    }

    if (!ensureViewerPc(viewer, error)) return false;

    if (const auto typeIt = data.find("type"); typeIt != data.end() && typeIt->is_string()) {
        if (typeIt->get<std::string>() == "offer") {
            const auto sdpIt = data.find("sdp");
            if (sdpIt == data.end() || !sdpIt->is_string()) {
                if (error) *error = "offer sdp eksik";
                return false;
            }
            const std::string offerSdp = sdpIt->get<std::string>();
            std::string answerSdp;
            {
                std::lock_guard lock(viewer->pcMutex);
                if (!viewer->pc || viewer->closed.load()) return false;
                try {
                    viewer->pc->setRemoteDescription(rtc::Description(offerSdp, rtc::Description::Type::Offer));
                    viewer->remoteOfferApplied = true;
                    for (const auto& c : viewer->pendingCandidates) {
                        try {
                            viewer->pc->addRemoteCandidate(c);
                        } catch (const std::exception&) {
                        }
                    }
                    viewer->pendingCandidates.clear();
                    viewer->pc->setLocalDescription(rtc::Description::Type::Answer);
                    const auto local = viewer->pc->localDescription();
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
            if (callbacks_.onViewerSignal) {
                const nlohmann::json msg = {{"type", "answer"}, {"sdp", answerSdp}};
                callbacks_.onViewerSignal(iid, msg.dump());
            }
            return true;
        }
    }

    if (const auto candIt = data.find("candidate"); candIt != data.end() && candIt->is_string()) {
        const std::string candStr = normalizeCandidateAttr(candIt->get<std::string>());
        if (candStr.empty()) return true;
        std::string mid;
        if (const auto m = data.find("mid"); m != data.end() && m->is_string()) mid = m->get<std::string>();
        else if (const auto sm = data.find("sdpMid"); sm != data.end() && sm->is_string()) mid = sm->get<std::string>();
        if (mid.empty()) mid = "0";

        std::lock_guard lock(viewer->pcMutex);
        if (!viewer->pc || viewer->closed.load()) return false;
        try {
            rtc::Candidate cand(candStr, mid);
            if (viewer->remoteOfferApplied) {
                viewer->pc->addRemoteCandidate(cand);
            } else {
                viewer->pendingCandidates.push_back(std::move(cand));
            }
        } catch (const std::exception&) {
        }
        return true;
    }

    return true;
}

void IzletBroadcastSession::closeViewerInternal(const std::shared_ptr<Viewer>& viewer,
                                                bool notifyPhp,
                                                const std::string& finalState,
                                                const std::string& err) {
    if (!viewer || viewer->closed.exchange(true)) return;
    viewer->videoReady.store(false, std::memory_order_release);
    {
        std::lock_guard qLock(viewer->queueMutex);
        viewer->videoQueue.clear();
        viewer->videoQueueBytes = 0;
        viewer->audioQueue.clear();
        viewer->queueCv.notify_all();
    }

    std::shared_ptr<rtc::Track> vt, at;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard tLock(viewer->trackMutex);
        vt = std::move(viewer->videoTrack);
        at = std::move(viewer->audioTrack);
    }
    {
        std::lock_guard pLock(viewer->pcMutex);
        pc = std::move(viewer->pc);
        viewer->pendingCandidates.clear();
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
    if (viewer->senderThread.joinable()) {
        viewer->senderThread.request_stop();
        viewer->senderThread.join();
    }
    if (notifyPhp && callbacks_.onViewerState) {
        callbacks_.onViewerState(viewer->iid, finalState, err);
    }
}

void IzletBroadcastSession::removeViewer(const std::string& iid, const std::string& reason, bool notifyPhp) {
    std::shared_ptr<Viewer> viewer;
    {
        std::lock_guard lock(mediaMutex_);
        const auto it = viewers_.find(iid);
        if (it == viewers_.end()) return;
        viewer = std::move(it->second);
        viewers_.erase(it);
    }
    oviora::log::info("izlet-sfu", std::format("yid={} viewer left iid={} reason={}",
                                              shortYid_, viewer->shortIid, reason.empty() ? "none" : reason));
    closeViewerInternal(viewer, notifyPhp, "bitti", "");
}

bool IzletBroadcastSession::hasViewer(const std::string& iid) const {
    std::lock_guard lock(mediaMutex_);
    return viewers_.contains(iid);
}

std::size_t IzletBroadcastSession::viewerCount() const {
    std::lock_guard lock(mediaMutex_);
    return viewers_.size();
}

double IzletBroadcastSession::estimatedInputMbps() const noexcept {
    return estimatedPubMbps_.load(std::memory_order_relaxed);
}

IzletBroadcastStats IzletBroadcastSession::collectStats(double elapsedSec) {
    IzletBroadcastStats out;
    std::uint64_t totalOutBytes = 0;
    {
        std::lock_guard lock(mediaMutex_);
        out.viewers = static_cast<int>(viewers_.size());
        for (const auto& [_, v] : viewers_) {
            totalOutBytes += v->bytesSent.load(std::memory_order_relaxed);
        }
    }
    const std::uint64_t lostNow = ingestLost_.load();
    out.lostPackets = static_cast<int>(lostNow - lastStatsLost_);
    lastStatsLost_ = lostNow;
    const std::uint64_t pubBytes = pubBytesTotal_.load(std::memory_order_relaxed);
    const std::uint64_t deltaOut = totalOutBytes >= lastStatsOutBytes_ ? (totalOutBytes - lastStatsOutBytes_) : 0;
    const std::uint64_t deltaPub = pubBytes >= lastStatsPubBytes_ ? (pubBytes - lastStatsPubBytes_) : 0;
    lastStatsOutBytes_ = totalOutBytes;
    lastStatsPubBytes_ = pubBytes;

    if (elapsedSec > 0.05) {
        out.outboundKbps = static_cast<int>(std::llround((double(deltaOut) * 8.0 / 1000.0) / elapsedSec));
        out.outboundMbps = (double(deltaOut) * 8.0 / 1000000.0) / elapsedSec;
        out.publisherInputMbps = (double(deltaPub) * 8.0 / 1000000.0) / elapsedSec;
        out.publisherKbps = static_cast<int>(std::llround(out.publisherInputMbps * 1000.0));
        estimatedPubMbps_.store(out.publisherInputMbps, std::memory_order_relaxed);
    }
    return out;
}

void IzletBroadcastSession::closeAll(bool notifyPhp) {
    if (closed_.exchange(true)) return;

    std::vector<std::shared_ptr<Viewer>> list;
    {
        std::lock_guard lock(mediaMutex_);
        list.reserve(viewers_.size());
        for (auto& [_, v] : viewers_) {
            list.push_back(std::move(v));
        }
        viewers_.clear();
        gopBuffer_.reset();
    }
    for (const auto& v : list) {
        closeViewerInternal(v, notifyPhp, "bitti", "");
    }

    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> vt, at;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard tLock(pubTrackMutex_);
        dc = std::move(pubKontrolDc_);
        vt = std::move(pubVideoTrack_);
        at = std::move(pubAudioTrack_);
    }
    {
        std::lock_guard lock(pubPcMutex_);
        pc = std::move(pubPc_);
        pubPendingCandidates_.clear();
    }
    if (dc) dc->resetCallbacks();
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

} // namespace oviora::gecit
