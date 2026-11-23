// SPDX-License-Identifier: GPL-3.0-only
#include "oviora/platform/IzletPublisher.h"

#include "oviora/Log.h"

#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>

#ifdef OVIORA_HAS_IZLET
#include <rtc/rtc.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace oviora::platform {
namespace {

#ifdef OVIORA_HAS_IZLET
constexpr std::size_t kMaxVideoQueueFrames = 120;
constexpr std::size_t kMaxVideoQueueBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxAudioQueuePackets = 100;
constexpr std::size_t kMaxFreeVideoBuffers = 16;
constexpr std::size_t kMaxFreeAudioBuffers = 16;

void initRtcLoggingOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        rtc::InitLogger(rtc::LogLevel::None, nullptr);
    });
}

std::string normalizeCandidateAttr(std::string cand) {
    if (cand.rfind("a=", 0) == 0) cand.erase(0, 2);
    while (!cand.empty() && (cand.back() == '\r' || cand.back() == '\n' || cand.back() == ' ')) {
        cand.pop_back();
    }
    return cand;
}

bool parseCodec(const QString& codecStr, oviora::remoteplay::Codec& out) {
    const QString low = codecStr.trimmed().toLower();
    if (low == QLatin1String("h264")) {
        out = oviora::remoteplay::Codec::H264;
        return true;
    }
    if (low == QLatin1String("h265") || low == QLatin1String("hevc") || low == QLatin1String("h265hdr")) {
        out = oviora::remoteplay::Codec::H265;
        return true;
    }
    return false;
}

std::vector<rtc::IceServer> parseIceServers(const QJsonArray& arr) {
    std::vector<rtc::IceServer> out;
    auto addUrl = [&](const QString& urlStr, const QString& user, const QString& pass) {
        const std::string url = urlStr.trimmed().toStdString();
        if (url.empty()) return;
        try {
            rtc::IceServer srv(url);
            if (!user.isEmpty()) srv.username = user.toStdString();
            if (!pass.isEmpty()) srv.password = pass.toStdString();
            out.push_back(std::move(srv));
        } catch (const std::exception&) {
        }
    };
    for (const auto& item : arr) {
        if (item.isString()) {
            addUrl(item.toString(), {}, {});
        } else if (item.isObject()) {
            const QJsonObject obj = item.toObject();
            const QString user = obj.value(QStringLiteral("username")).toString();
            const QString pass = obj.value(QStringLiteral("credential")).toString();
            const QJsonValue urlsVal = obj.value(QStringLiteral("urls"));
            if (urlsVal.isString()) {
                addUrl(urlsVal.toString(), user, pass);
            } else if (urlsVal.isArray()) {
                for (const auto& u : urlsVal.toArray()) {
                    if (u.isString()) addUrl(u.toString(), user, pass);
                }
            }
        }
    }
    return out;
}

bool containsH265Irap(std::span<const std::uint8_t> b) noexcept {
    std::size_t pos = 0;
    while (pos + 4 <= b.size()) {
        std::size_t prefix = 0;
        if (b[pos] == 0 && b[pos + 1] == 0) {
            if (b[pos + 2] == 1) prefix = 3;
            else if (pos + 4 <= b.size() && b[pos + 2] == 0 && b[pos + 3] == 1) prefix = 4;
        }
        if (!prefix) {
            ++pos;
            continue;
        }
        const std::size_t hdr = pos + prefix;
        if (hdr + 1 >= b.size()) break;
        const unsigned type = (b[hdr] >> 1) & 0x3Fu;
        if (type == 19u || type == 20u || type == 21u) return true;
        pos = hdr + 2;
    }
    return false;
}
#endif

} // namespace

struct IzletPublisher::Shared {
    QPointer<IzletPublisher> owner;

    std::atomic<bool> active{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> videoTrackOpen{false};
    std::atomic<bool> audioTrackOpen{false};

    oviora::remoteplay::Codec codec = oviora::remoteplay::Codec::H264;
    QString codecName;

    std::mutex signalMutex;
    std::function<void(const QJsonObject&)> sendSignal;

    std::mutex stateMutex;
    QString lastEmittedState;

#ifdef OVIORA_HAS_IZLET
    std::mutex pcMutex;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<rtc::DataChannel> kontrolDc;
    bool remoteAnswerApplied = false;
    std::vector<rtc::Candidate> pendingCandidates;

    std::mutex idrMutex;
    std::chrono::steady_clock::time_point lastIdrEmit{};
    bool hasIdrEmit = false;

    struct QueuedVideoFrame {
        rtc::binary data;
        std::chrono::steady_clock::time_point arrival;
        bool isIdr = false;
    };
    struct QueuedAudioPacket {
        rtc::binary data;
        std::uint32_t frameSamples = 480;
    };

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::vector<std::uint8_t> cachedConfig;
    bool waitingForIdr = true;
    std::deque<QueuedVideoFrame> videoQueue;
    std::size_t videoQueueBytes = 0;
    std::deque<QueuedAudioPacket> audioQueue;
    std::vector<rtc::binary> freeVideoBufs;
    std::vector<rtc::binary> freeAudioBufs;
    std::jthread senderThread;

    rtc::binary borrowVideoBuf(std::size_t needed) {
        rtc::binary b;
        if (!freeVideoBufs.empty()) {
            b = std::move(freeVideoBufs.back());
            freeVideoBufs.pop_back();
        }
        b.resize(needed);
        return b;
    }

    void recycleVideoBuf(rtc::binary&& b) {
        if (freeVideoBufs.size() < kMaxFreeVideoBuffers && b.capacity() <= 2u * 1024u * 1024u) {
            b.clear();
            freeVideoBufs.push_back(std::move(b));
        }
    }

    rtc::binary borrowAudioBuf(std::size_t needed) {
        rtc::binary b;
        if (!freeAudioBufs.empty()) {
            b = std::move(freeAudioBufs.back());
            freeAudioBufs.pop_back();
        }
        b.resize(needed);
        return b;
    }

    void recycleAudioBuf(rtc::binary&& b) {
        if (freeAudioBufs.size() < kMaxFreeAudioBuffers && b.capacity() <= 8192u) {
            b.clear();
            freeAudioBufs.push_back(std::move(b));
        }
    }

    void clearQueuesLocked() {
        while (!videoQueue.empty()) {
            recycleVideoBuf(std::move(videoQueue.front().data));
            videoQueue.pop_front();
        }
        videoQueueBytes = 0;
        while (!audioQueue.empty()) {
            recycleAudioBuf(std::move(audioQueue.front().data));
            audioQueue.pop_front();
        }
    }
#endif

    void emitState(const QString& st) {
        {
            std::lock_guard lock(stateMutex);
            if (lastEmittedState == st) return;
            lastEmittedState = st;
        }
        QPointer<IzletPublisher> target = owner;
        if (!target) return;
        QMetaObject::invokeMethod(target.data(), [target, st]() {
            if (target) emit target->stateChanged(st);
        }, Qt::QueuedConnection);
    }

    void postSignal(const QJsonObject& obj) {
        std::function<void(const QJsonObject&)> cb;
        {
            std::lock_guard lock(signalMutex);
            cb = sendSignal;
        }
        if (!cb) return;
        QPointer<IzletPublisher> target = owner;
        if (!target) return;
        QMetaObject::invokeMethod(target.data(), [target, cb = std::move(cb), obj]() {
            if (target && target->active()) cb(obj);
        }, Qt::QueuedConnection);
    }

#ifdef OVIORA_HAS_IZLET
    void emitIdrRequest() {
        {
            std::lock_guard lock(idrMutex);
            const auto now = std::chrono::steady_clock::now();
            if (hasIdrEmit && (now - lastIdrEmit) < std::chrono::milliseconds(500)) {
                return;
            }
            hasIdrEmit = true;
            lastIdrEmit = now;
        }
        QPointer<IzletPublisher> target = owner;
        if (!target) return;
        QMetaObject::invokeMethod(target.data(), [target]() {
            if (target && target->active()) emit target->idrRequested();
        }, Qt::QueuedConnection);
    }

    void enqueueVideo(oviora::remoteplay::SampleKind kind,
                      std::span<const std::uint8_t> annexB,
                      bool isIdr,
                      bool lostOrRecovered) {
        if (!active.load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire) || annexB.empty()) {
            return;
        }
        if (kind == oviora::remoteplay::SampleKind::CodecConfig) {
            const auto nal = oviora::remoteplay::inspectAnnexB(annexB, codec);
            if (!nal.valid || !nal.hasConfig) return;
            std::lock_guard lock(queueMutex);
            cachedConfig.assign(annexB.begin(), annexB.end());
            return;
        }
        if (!videoTrackOpen.load(std::memory_order_acquire)) {
            return;
        }
        const auto nal = oviora::remoteplay::inspectAnnexB(annexB, codec);
        if (!nal.valid || !nal.hasPicture) return;

        const bool effectiveIdr = isIdr || nal.idr ||
            (codec != oviora::remoteplay::Codec::H264 && containsH265Irap(annexB));
        const auto now = std::chrono::steady_clock::now();

        std::lock_guard lock(queueMutex);
        if (stopping.load(std::memory_order_relaxed)) return;
        if (lostOrRecovered) waitingForIdr = true;
        if (waitingForIdr && !effectiveIdr) return;

        const bool needConfig = effectiveIdr && !nal.hasConfig;
        if (needConfig && cachedConfig.empty()) {
            waitingForIdr = true;
            return;
        }
        const std::size_t totalSize = (needConfig ? cachedConfig.size() : 0) + annexB.size();
        if (totalSize > kMaxVideoQueueBytes) {
            waitingForIdr = true;
            return;
        }
        if (videoQueue.size() >= kMaxVideoQueueFrames || videoQueueBytes + totalSize > kMaxVideoQueueBytes) {
            clearQueuesLocked();
            if (!effectiveIdr) {
                waitingForIdr = true;
                return;
            }
        }
        rtc::binary buf = borrowVideoBuf(totalSize);
        std::size_t off = 0;
        if (needConfig) {
            std::memcpy(buf.data(), cachedConfig.data(), cachedConfig.size());
            off = cachedConfig.size();
        }
        std::memcpy(buf.data() + off, annexB.data(), annexB.size());
        waitingForIdr = false;
        videoQueueBytes += buf.size();
        videoQueue.push_back(QueuedVideoFrame{std::move(buf), now, effectiveIdr});
        queueCv.notify_one();
    }

    void enqueueAudio(std::span<const std::uint8_t> opusPacket, std::uint32_t frameSamples) {
        if (!active.load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire) ||
            !audioTrackOpen.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard lock(queueMutex);
        if (stopping.load(std::memory_order_relaxed)) return;
        while (audioQueue.size() >= kMaxAudioQueuePackets) {
            recycleAudioBuf(std::move(audioQueue.front().data));
            audioQueue.pop_front();
        }
        rtc::binary buf = borrowAudioBuf(opusPacket.size());
        if (!opusPacket.empty()) {
            std::memcpy(buf.data(), opusPacket.data(), opusPacket.size());
        }
        audioQueue.push_back(QueuedAudioPacket{
            std::move(buf),
            frameSamples > 0 ? frameSamples : 480u
        });
        queueCv.notify_one();
    }

    void senderLoop(const std::stop_token& stopToken) {
        bool videoOriginSet = false;
        std::chrono::steady_clock::time_point videoOrigin{};
        std::uint64_t audioSamplesSent = 0;

        while (!stopToken.stop_requested() && !stopping.load(std::memory_order_acquire)) {
            std::deque<QueuedVideoFrame> vBatch;
            std::deque<QueuedAudioPacket> aBatch;
            {
                std::unique_lock lock(queueMutex);
                queueCv.wait(lock, [&] {
                    return stopToken.stop_requested() || stopping.load(std::memory_order_relaxed) ||
                           !videoQueue.empty() || !audioQueue.empty();
                });
                if (stopToken.stop_requested() || stopping.load(std::memory_order_relaxed)) break;
                vBatch.swap(videoQueue);
                videoQueueBytes = 0;
                aBatch.swap(audioQueue);
            }

            std::shared_ptr<rtc::Track> vTrack, aTrack;
            {
                std::lock_guard pcLock(pcMutex);
                vTrack = videoTrack;
                aTrack = audioTrack;
            }

            for (auto& vf : vBatch) {
                if (stopToken.stop_requested() || stopping.load(std::memory_order_relaxed)) break;
                if (vTrack && vTrack->isOpen() && !vf.data.empty()) {
                    if (!videoOriginSet) {
                        videoOrigin = vf.arrival;
                        videoOriginSet = true;
                    }
                    const std::chrono::duration<double> elapsed =
                        vf.arrival >= videoOrigin ? (vf.arrival - videoOrigin) : std::chrono::duration<double>::zero();
                    try {
                        vTrack->sendFrame(std::move(vf.data), elapsed);
                    } catch (const std::exception&) {
                    }
                } else {
                    std::lock_guard lock(queueMutex);
                    recycleVideoBuf(std::move(vf.data));
                }
            }

            for (auto& ap : aBatch) {
                if (stopToken.stop_requested() || stopping.load(std::memory_order_relaxed)) break;
                const std::uint32_t step = ap.frameSamples > 0 ? ap.frameSamples : 480u;
                const std::uint64_t samplePos = audioSamplesSent;
                audioSamplesSent += step;
                if (aTrack && aTrack->isOpen() && !ap.data.empty()) {
                    const std::chrono::duration<double> tsSec(static_cast<double>(samplePos) / 48000.0);
                    try {
                        aTrack->sendFrame(std::move(ap.data), tsSec);
                    } catch (const std::exception&) {
                    }
                } else {
                    std::lock_guard lock(queueMutex);
                    recycleAudioBuf(std::move(ap.data));
                }
            }
        }
    }
#endif
};

IzletPublisher::IzletPublisher(QObject* parent)
    : QObject(parent), shared_(std::make_shared<Shared>()) {
    shared_->owner = this;
}

IzletPublisher::~IzletPublisher() {
    stop();
}

bool IzletPublisher::isAvailable() noexcept {
#ifdef OVIORA_HAS_IZLET
    return true;
#else
    return false;
#endif
}

bool IzletPublisher::active() const noexcept {
    return shared_->active.load(std::memory_order_acquire);
}

QString IzletPublisher::codec() const {
    return shared_->codecName;
}

bool IzletPublisher::start(const QString& codecStr,
                           const QJsonArray& iceServers,
                           std::function<void(const QJsonObject&)> sendSignal) {
#ifndef OVIORA_HAS_IZLET
    Q_UNUSED(codecStr);
    Q_UNUSED(iceServers);
    Q_UNUSED(sendSignal);
    shared_->emitState(QStringLiteral("failed"));
    return false;
#else
    oviora::remoteplay::Codec parsedCodec = oviora::remoteplay::Codec::H264;
    if (!parseCodec(codecStr, parsedCodec)) {
        oviora::log::warn("izlet-pub", std::format("unsupported codec: {}", codecStr.toStdString()));
        return false;
    }

    stop();
    initRtcLoggingOnce();

    auto& s = *shared_;
    s.stopping.store(false, std::memory_order_release);
    s.videoTrackOpen.store(false, std::memory_order_release);
    s.audioTrackOpen.store(false, std::memory_order_release);
    s.codec = parsedCodec;
    s.codecName = (parsedCodec == oviora::remoteplay::Codec::H264) ? QStringLiteral("h264") : QStringLiteral("h265");
    {
        std::lock_guard lock(s.signalMutex);
        s.sendSignal = std::move(sendSignal);
    }
    {
        std::lock_guard lock(s.stateMutex);
        s.lastEmittedState.clear();
    }
    {
        std::lock_guard lock(s.queueMutex);
        s.cachedConfig.clear();
        s.waitingForIdr = true;
        s.clearQueuesLocked();
    }

    rtc::Configuration cfg;
    cfg.disableAutoNegotiation = true;
    cfg.maxMessageSize = 64 * 1024;
    cfg.iceServers = parseIceServers(iceServers);

    std::string offerSdp;
    try {
        auto pc = std::make_shared<rtc::PeerConnection>(cfg);
        std::weak_ptr<Shared> weak = shared_;

        pc->onStateChange([weak](rtc::PeerConnection::State state) {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire)) return;
            switch (state) {
            case rtc::PeerConnection::State::Connecting:
                sp->emitState(QStringLiteral("connecting"));
                break;
            case rtc::PeerConnection::State::Connected:
                sp->emitState(QStringLiteral("connected"));
                break;
            case rtc::PeerConnection::State::Disconnected:
                sp->emitState(QStringLiteral("disconnected"));
                break;
            case rtc::PeerConnection::State::Failed:
                sp->emitState(QStringLiteral("failed"));
                break;
            case rtc::PeerConnection::State::Closed:
                sp->emitState(QStringLiteral("stopped"));
                break;
            default:
                break;
            }
        });

        pc->onLocalCandidate([weak](rtc::Candidate cand) {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire)) return;
            const std::string candStr = normalizeCandidateAttr(cand.candidate());
            if (candStr.empty()) return;
            QJsonObject msg;
            msg.insert(QStringLiteral("candidate"), QString::fromStdString(candStr));
            msg.insert(QStringLiteral("mid"), QString::fromStdString(cand.mid()));
            sp->postSignal(msg);
        });

        // Video track (sendonly, H.264 or H.265)
        constexpr rtc::SSRC kVideoSsrc = 0x495A5631u;
        constexpr std::uint8_t kVideoPt = 96;
        rtc::Description::Video videoDesc("0", rtc::Description::Direction::SendOnly);
        if (parsedCodec == oviora::remoteplay::Codec::H264) {
            videoDesc.addH264Codec(kVideoPt);
        } else {
            videoDesc.addH265Codec(kVideoPt);
        }
        videoDesc.addSSRC(kVideoSsrc, "oviora-izlet-video", "oviora-izlet", "video0");
        auto videoTrack = pc->addTrack(videoDesc);

        auto videoRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc, "oviora-izlet-video", kVideoPt, rtc::RtpPacketizer::VideoClockRate);
        std::shared_ptr<rtc::RtpPacketizer> videoPacketizer;
        if (parsedCodec == oviora::remoteplay::Codec::H264) {
            videoPacketizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, videoRtpCfg);
        } else {
            videoPacketizer = std::make_shared<rtc::H265RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, videoRtpCfg);
        }
        auto vSr = std::make_shared<rtc::RtcpSrReporter>(videoRtpCfg);
        auto vNack = std::make_shared<rtc::RtcpNackResponder>();
        auto vPli = std::make_shared<rtc::PliHandler>([weak]() {
            if (auto sp = weak.lock(); sp && !sp->stopping.load(std::memory_order_acquire)) {
                sp->emitIdrRequest();
            }
        });
        videoPacketizer->addToChain(vSr);
        videoPacketizer->addToChain(vNack);
        videoPacketizer->addToChain(vPli);
        videoTrack->setMediaHandler(videoPacketizer);
        videoTrack->onOpen([weak]() {
            if (auto sp = weak.lock(); sp && !sp->stopping.load(std::memory_order_acquire)) {
                sp->videoTrackOpen.store(true, std::memory_order_release);
                sp->emitIdrRequest();
            }
        });

        // Audio track (sendonly, Opus 48 kHz stereo)
        constexpr rtc::SSRC kAudioSsrc = 0x495A4131u;
        constexpr std::uint8_t kAudioPt = 111;
        rtc::Description::Audio audioDesc("1", rtc::Description::Direction::SendOnly);
        audioDesc.addOpusCodec(kAudioPt);
        audioDesc.addSSRC(kAudioSsrc, "oviora-izlet-audio", "oviora-izlet", "audio0");
        auto audioTrack = pc->addTrack(audioDesc);

        auto audioRtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(
            kAudioSsrc, "oviora-izlet-audio", kAudioPt, rtc::OpusRtpPacketizer::DefaultClockRate);
        auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioRtpCfg);
        auto aSr = std::make_shared<rtc::RtcpSrReporter>(audioRtpCfg);
        auto aNack = std::make_shared<rtc::RtcpNackResponder>();
        audioPacketizer->addToChain(aSr);
        audioPacketizer->addToChain(aNack);
        audioTrack->setMediaHandler(audioPacketizer);
        audioTrack->onOpen([weak]() {
            if (auto sp = weak.lock(); sp && !sp->stopping.load(std::memory_order_acquire)) {
                sp->audioTrackOpen.store(true, std::memory_order_release);
            }
        });

        // Reliable ordered control DataChannel (`kontrol`)
        auto kontrolDc = pc->createDataChannel("kontrol");
        kontrolDc->onMessage([weak](rtc::message_variant msg) {
            auto sp = weak.lock();
            if (!sp || sp->stopping.load(std::memory_order_acquire)) return;
            QByteArray raw;
            if (const auto* text = std::get_if<std::string>(&msg)) {
                raw = QByteArray(text->data(), qsizetype(text->size()));
            } else if (const auto* bin = std::get_if<rtc::binary>(&msg)) {
                raw = QByteArray(reinterpret_cast<const char*>(bin->data()), qsizetype(bin->size()));
            } else {
                return;
            }
            const auto doc = QJsonDocument::fromJson(raw);
            if (!doc.isObject()) return;
            if (doc.object().value(QStringLiteral("t")).toString() == QLatin1String("idr")) {
                sp->emitIdrRequest();
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
            std::lock_guard pcLock(s.pcMutex);
            s.pc = std::move(pc);
            s.videoTrack = std::move(videoTrack);
            s.audioTrack = std::move(audioTrack);
            s.kontrolDc = std::move(kontrolDc);
            s.remoteAnswerApplied = false;
            s.pendingCandidates.clear();
        }
    } catch (const std::exception& e) {
        oviora::log::error("izlet-pub", std::format("start failed: {}", e.what()));
        stop();
        s.emitState(QStringLiteral("failed"));
        return false;
    }

    s.active.store(true, std::memory_order_release);
    s.senderThread = std::jthread([sp = shared_](std::stop_token st) {
        sp->senderLoop(st);
    });

    s.emitState(QStringLiteral("connecting"));
    QJsonObject offerMsg;
    offerMsg.insert(QStringLiteral("type"), QStringLiteral("offer"));
    offerMsg.insert(QStringLiteral("sdp"), QString::fromStdString(offerSdp));
    s.postSignal(offerMsg);
    return true;
#endif
}

void IzletPublisher::onRemoteSignal(const QJsonObject& signal) {
#ifndef OVIORA_HAS_IZLET
    Q_UNUSED(signal);
#else
    auto& s = *shared_;
    if (!s.active.load(std::memory_order_acquire) || s.stopping.load(std::memory_order_acquire)) return;

    const QString type = signal.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("answer")) {
        const std::string sdp = signal.value(QStringLiteral("sdp")).toString().toStdString();
        if (sdp.empty()) return;
        std::lock_guard pcLock(s.pcMutex);
        if (!s.pc || s.stopping.load(std::memory_order_relaxed)) return;
        try {
            s.pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
            s.remoteAnswerApplied = true;
            for (const auto& c : s.pendingCandidates) {
                try {
                    s.pc->addRemoteCandidate(c);
                } catch (const std::exception&) {
                }
            }
            s.pendingCandidates.clear();
        } catch (const std::exception& e) {
            oviora::log::warn("izlet-pub", std::format("answer failed: {}", e.what()));
            s.emitState(QStringLiteral("failed"));
        }
        return;
    }

    if (signal.contains(QStringLiteral("candidate"))) {
        const std::string candStr = normalizeCandidateAttr(
            signal.value(QStringLiteral("candidate")).toString().toStdString());
        if (candStr.empty()) return;
        std::string mid = signal.value(QStringLiteral("mid")).toString().toStdString();
        if (mid.empty()) mid = signal.value(QStringLiteral("sdpMid")).toString().toStdString();
        if (mid.empty()) mid = "0";

        std::lock_guard pcLock(s.pcMutex);
        if (!s.pc || s.stopping.load(std::memory_order_relaxed)) return;
        try {
            rtc::Candidate cand(candStr, mid);
            if (s.remoteAnswerApplied) {
                s.pc->addRemoteCandidate(cand);
            } else {
                s.pendingCandidates.push_back(std::move(cand));
            }
        } catch (const std::exception&) {
        }
    }
#endif
}

void IzletPublisher::stop() {
    auto& s = *shared_;
    const bool wasActive = s.active.exchange(false, std::memory_order_acq_rel);
    s.stopping.store(true, std::memory_order_release);
    s.videoTrackOpen.store(false, std::memory_order_release);
    s.audioTrackOpen.store(false, std::memory_order_release);

    {
        std::lock_guard lock(s.signalMutex);
        s.sendSignal = nullptr;
    }

#ifdef OVIORA_HAS_IZLET
    {
        std::lock_guard lock(s.queueMutex);
        s.queueCv.notify_all();
    }
    if (s.senderThread.joinable()) {
        s.senderThread.request_stop();
        s.senderThread.join();
    }
    {
        std::lock_guard lock(s.queueMutex);
        s.clearQueuesLocked();
        s.cachedConfig.clear();
        s.waitingForIdr = true;
    }

    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> vt, at;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard pcLock(s.pcMutex);
        dc = std::move(s.kontrolDc);
        vt = std::move(s.videoTrack);
        at = std::move(s.audioTrack);
        pc = std::move(s.pc);
        s.pendingCandidates.clear();
        s.remoteAnswerApplied = false;
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
#endif

    if (wasActive) {
        s.emitState(QStringLiteral("stopped"));
    }
}

void IzletPublisher::pushVideo(oviora::remoteplay::SampleKind kind,
                               std::span<const std::uint8_t> annexB,
                               bool isIdr,
                               bool lostOrRecovered) {
#ifndef OVIORA_HAS_IZLET
    Q_UNUSED(kind);
    Q_UNUSED(annexB);
    Q_UNUSED(isIdr);
    Q_UNUSED(lostOrRecovered);
#else
    shared_->enqueueVideo(kind, annexB, isIdr, lostOrRecovered);
#endif
}

void IzletPublisher::pushAudio(std::span<const std::uint8_t> opusPacket, std::uint32_t frameSamples) {
#ifndef OVIORA_HAS_IZLET
    Q_UNUSED(opusPacket);
    Q_UNUSED(frameSamples);
#else
    shared_->enqueueAudio(opusPacket, frameSamples);
#endif
}

oviora::remoteplay::MediaRelayCallbacks IzletPublisher::makeTeeCallbacks() {
    oviora::remoteplay::MediaRelayCallbacks cb;
    std::weak_ptr<Shared> weak = shared_;
    cb.onVideo = [weak](oviora::remoteplay::SampleKind kind,
                        std::span<const std::uint8_t> bytes,
                        std::uint32_t, std::uint32_t,
                        bool idr, bool lostOrRecovered) {
#ifdef OVIORA_HAS_IZLET
        if (auto sp = weak.lock()) {
            sp->enqueueVideo(kind, bytes, idr, lostOrRecovered);
        }
#else
        Q_UNUSED(weak); Q_UNUSED(kind); Q_UNUSED(bytes); Q_UNUSED(idr); Q_UNUSED(lostOrRecovered);
#endif
    };
    cb.onAudio = [weak](std::span<const std::uint8_t> opusPacket,
                        std::uint32_t, std::uint32_t, std::uint32_t frameSize) {
#ifdef OVIORA_HAS_IZLET
        if (auto sp = weak.lock()) {
            sp->enqueueAudio(opusPacket, frameSize);
        }
#else
        Q_UNUSED(weak); Q_UNUSED(opusPacket); Q_UNUSED(frameSize);
#endif
    };
    return cb;
}

} // namespace oviora::platform
