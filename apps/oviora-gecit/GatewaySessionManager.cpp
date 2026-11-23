// SPDX-License-Identifier: GPL-3.0-only
#include "GatewaySessionManager.h"

#include "oviora/Log.h"
#include "oviora/remoteplay/ChiakiBackend.h"
#include "oviora/remoteplay/SessionInbox.h"
#include "oviora/remoteplay/StreamRecovery.h"

#include <QMetaObject>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <format>
#include <thread>

namespace oviora::gecit {
namespace {

std::string mapFailureMessage(const oviora::remoteplay::NativeSnapshot& native,
                              const oviora::remoteplay::SessionInbox::Snapshot& inbox) {
    if (inbox.state == oviora::remoteplay::SessionState::LoginPinRequired) {
        return "PS5 oturum PIN kodu istiyor.";
    }
    if (native.applicationReason == 0x80108b09u) {
        return "PS5 eşleştirmeyi kabul etmedi.";
    }
    if (native.applicationReason == 0x80108b12u) {
        return "Bu PSN hesabında PS5 Uzaktan Oynatma kapalı.";
    }
    if (native.applicationReason == 0x80108b02u) {
        return "PS5 üzerinde bu PSN hesabı oturumu açık değil.";
    }
    if (native.startupRetryAllowed || native.applicationReason == 0x80108b10u) {
        return "PS5 şu anda başka bir uzaktan oynatma oturumunda.";
    }
    if (native.lastQuitReason == 12 /* CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN */) {
        return "PS5 akışı sonlandırdı.";
    }
    if (native.lastQuitReason == 14 /* CHIAKI_QUIT_REASON_PSN_DATA_CONNECTION_FAILED */) {
        return "PS5 veri bağlantısı açılamadı (NAT/güvenlik duvarı).";
    }
    if (native.lastQuitReason == 15 /* CHIAKI_QUIT_REASON_PSN_CTRL_LOGIN_REJECTED */) {
        return "PS5 kontrol oturumunu reddetti.";
    }
    return "PS5 bağlantısı kurulamadı veya kesildi.";
}

std::vector<AnnexBTestFrame> loadMockH264(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return splitAnnexBFrames(bytes);
}

} // namespace

struct GatewaySessionManager::ActiveSession {
    std::string wsid;
    std::string shortId;
    std::shared_ptr<WebRtcGatewaySession> webrtc;

    std::mutex cacheMutex;
    H264RelayCache h264Cache;

    std::mutex inputMutex;
    KumandaSeqTracker seqTracker;
    oviora::remoteplay::ControllerState controller{};
    oviora::remoteplay::HostTime lastInput100ns = 0;

    std::atomic<bool> stopRequested{false};
    std::atomic<bool> idrRequested{false};
    std::atomic<bool> canliSent{false};
    std::atomic<double> lastPs5Loss{0.0};
    std::atomic<bool> endedByGateway{false};

    std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastStatsAt = std::chrono::steady_clock::now();
    std::uint64_t lastStatsBytes = 0;
    std::uint64_t lastStatsFrames = 0;

    std::jthread ownerThread;
};

GatewaySessionManager::GatewaySessionManager(const GatewayConfig& config,
                                             PhpGatewayClient& phpClient,
                                             std::filesystem::path mockH264Path,
                                             QObject* parent)
    : QObject(parent),
      config_(config),
      php_(phpClient),
      mockH264Path_(std::move(mockH264Path)),
      statsTimer_(new QTimer(this)) {
    if (!mockH264Path_.empty()) {
        mockFrames_ = loadMockH264(mockH264Path_);
        oviora::log::info("gecit", std::format("deneme-kaynak yuklendi kare={} yol_ok={}",
                                               mockFrames_.size(), !mockFrames_.empty()));
    }
    statsTimer_->setInterval(5000);
    connect(statsTimer_, &QTimer::timeout, this, &GatewaySessionManager::publishStats);
    statsTimer_->start();
}

GatewaySessionManager::~GatewaySessionManager() {
    shutdownAll();
}

std::size_t GatewaySessionManager::activeCount() const {
    std::lock_guard lock(sessionsMutex_);
    return sessions_.size();
}

void GatewaySessionManager::handleJobs(std::vector<GwJob> jobs) {
    for (auto& job : jobs) {
        switch (job.type) {
        case GwJob::Type::Start:
            startSession(std::move(job.start));
            break;
        case GwJob::Type::Signal:
            signalSession(job.signal);
            break;
        case GwJob::Type::Stop:
            stopSession(job.stop.wsid, job.stop.reason, true);
            break;
        case GwJob::Type::IzletStart:
            startIzletBroadcast(job.izletStart);
            break;
        case GwJob::Type::IzleJoin:
            joinIzletViewer(job.izleJoin);
            break;
        case GwJob::Type::IzleLeave:
            leaveIzletViewer(job.izleLeave.iid, job.izleLeave.reason, true);
            break;
        case GwJob::Type::IzletStop:
            stopIzletBroadcast(job.izletStop.yid, job.izletStop.reason, true);
            break;
        }
    }
}

void GatewaySessionManager::startSession(GwStartJob job) {
    const std::string shortId = shortWsid(job.wsid);
    if (shuttingDown_) {
        job.wipeToken();
        return;
    }

    {
        std::lock_guard lock(sessionsMutex_);
        if (sessions_.contains(job.wsid)) {
            oviora::log::warn("gecit", std::format("wsid={} already active, ignoring duplicate start", shortId));
            job.wipeToken();
            return;
        }
        if (sessions_.size() >= config_.maxSessions) {
            oviora::log::warn("gecit", std::format("wsid={} rejected: maxSessions={} reached", shortId, config_.maxSessions));
            php_.sendState(job.wsid, "hata", "Geçit kapasitesi dolu.");
            job.wipeToken();
            return;
        }
    }

    const bool mockMode = !mockH264Path_.empty();
    if (!mockMode) {
        if (job.psnDuid.empty() || job.psnToken.empty() ||
            !oviora::remoteplay::accountIdFromBase64(job.psnAccountIdB64)) {
            oviora::log::warn("gecit", std::format("wsid={} invalid PSN parameters", shortId));
            php_.sendState(job.wsid, "hata", "PSN oturum bilgileri geçersiz.");
            job.wipeToken();
            return;
        }
    }

    auto session = std::make_shared<ActiveSession>();
    session->wsid = job.wsid;
    session->shortId = shortId;

    WebRtcSessionCallbacks cb;
    cb.onLocalSignal = [this, wsid = job.wsid](const std::string& signalJson) {
        QMetaObject::invokeMethod(this, [this, wsid, signalJson]() {
            if (!shuttingDown_) php_.sendSignal(wsid, signalJson);
        }, Qt::QueuedConnection);
    };
    cb.onConnected = [shortId]() {
        oviora::log::info("gecit", std::format("wsid={} webrtc connected", shortId));
    };
    cb.onDisconnected = [this, wsid = job.wsid, shortId](const std::string& reason) {
        oviora::log::info("gecit", std::format("wsid={} webrtc ended ({})", shortId, reason));
        QMetaObject::invokeMethod(this, [this, wsid]() {
            stopSession(wsid, "webrtc disconnected", true);
        }, Qt::QueuedConnection);
    };
    cb.onPliRequested = [weak = std::weak_ptr<ActiveSession>(session)]() {
        if (auto s = weak.lock()) {
            s->idrRequested.store(true, std::memory_order_relaxed);
        }
    };
    cb.onKumanda = [weak = std::weak_ptr<ActiveSession>(session)](std::span<const std::uint8_t> data) {
        if (auto s = weak.lock()) {
            std::lock_guard lock(s->inputMutex);
            const auto res = parseKumandaPacket(data, s->seqTracker, s->controller);
            if (res == KumandaParseResult::UpdatedState || res == KumandaParseResult::UpdatedTouches) {
                s->lastInput100ns = oviora::remoteplay::monotonic100ns();
            }
        }
    };
    cb.onBye = [this, wsid = job.wsid, shortId]() {
        oviora::log::info("gecit", std::format("wsid={} bye received from browser", shortId));
        QMetaObject::invokeMethod(this, [this, wsid]() {
            stopSession(wsid, "browser bye", true);
        }, Qt::QueuedConnection);
    };

    session->webrtc = std::make_shared<WebRtcGatewaySession>(job.wsid, config_, std::move(cb));

    {
        std::lock_guard lock(sessionsMutex_);
        sessions_.emplace(job.wsid, session);
    }

    oviora::log::info("gecit", std::format("wsid={} starting ({}x{}@{} {}kbps mock={})",
                                           shortId, job.width, job.height, job.fps, job.bitrateKbps, mockMode));
    php_.sendState(job.wsid, "baglaniyor", "");

    session->ownerThread = std::jthread([this, session, startJob = std::move(job), mockMode](std::stop_token) mutable {
        if (mockMode) {
            runMockSourceOwner(session, std::move(startJob));
        } else {
            runRealPs5Owner(session, std::move(startJob));
        }
    });
}

void GatewaySessionManager::signalSession(const GwSignalJob& job) {
    if (job.targetKind == SignalTargetKind::Yid) {
        std::shared_ptr<IzletBroadcastSession> b;
        {
            std::lock_guard lock(sessionsMutex_);
            const auto it = izletBroadcasts_.find(job.targetId);
            if (it == izletBroadcasts_.end()) return;
            b = it->second;
        }
        std::string err;
        if (!b->handlePublisherSignal(job.data, &err)) {
            oviora::log::warn("izlet-sfu", std::format("yid={} pub signal rejected: {}", shortWsid(job.targetId), err));
            php_.sendStateFor("yid", job.targetId, "hata", "WebRTC sinyalleşme hatası.");
            stopIzletBroadcast(job.targetId, "publisher signal error", false);
        }
        return;
    }

    if (job.targetKind == SignalTargetKind::Iid) {
        std::shared_ptr<IzletBroadcastSession> b;
        {
            std::lock_guard lock(sessionsMutex_);
            const auto vIt = viewerToBroadcast_.find(job.targetId);
            if (vIt == viewerToBroadcast_.end()) return;
            const auto bIt = izletBroadcasts_.find(vIt->second);
            if (bIt == izletBroadcasts_.end()) return;
            b = bIt->second;
        }
        std::string err;
        if (!b->handleViewerSignal(job.targetId, job.data, &err)) {
            oviora::log::warn("izlet-sfu", std::format("iid={} viewer signal rejected: {}", shortWsid(job.targetId), err));
            php_.sendStateFor("iid", job.targetId, "hata", "İzleyici WebRTC sinyalleşme hatası.");
            leaveIzletViewer(job.targetId, "viewer signal error", false);
        }
        return;
    }

    std::shared_ptr<ActiveSession> session;
    {
        std::lock_guard lock(sessionsMutex_);
        const auto it = sessions_.find(job.targetId);
        if (it == sessions_.end()) return;
        session = it->second;
    }
    std::string err;
    if (!session->webrtc->handleRemoteSignal(job.data, &err)) {
        oviora::log::warn("gecit", std::format("wsid={} signal rejected: {}", session->shortId, err));
        php_.sendState(session->wsid, "hata", "WebRTC sinyalleşme hatası.");
        session->webrtc->sendError("WebRTC sinyalleşme hatası.");
        stopSession(session->wsid, "signal error", false);
    }
}

void GatewaySessionManager::stopSession(const std::string& wsid, const std::string& reason, bool notifyPhp) {
    std::shared_ptr<ActiveSession> session;
    {
        std::lock_guard lock(sessionsMutex_);
        const auto it = sessions_.find(wsid);
        if (it == sessions_.end()) return;
        session = std::move(it->second);
        sessions_.erase(it);
    }

    const auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - session->startedAt).count();
    oviora::log::info("gecit", std::format("wsid={} stopping reason={} durationMs={}",
                                           session->shortId, reason.empty() ? "none" : reason, durMs));

    session->stopRequested.store(true);
    session->endedByGateway.store(true);
    if (session->webrtc) {
        session->webrtc->sendState("bitti");
        session->webrtc->close();
    }
    if (session->ownerThread.joinable()) {
        session->ownerThread.request_stop();
        session->ownerThread.join();
    }
    if (notifyPhp) {
        php_.sendState(wsid, "bitti", "");
    }
}

void GatewaySessionManager::publishStats() {
    struct Entry {
        std::string wsid;
        int kbps = 0;
        double fps = 0.0;
        int rttMs = -1;
        double loss = 0.0;
    };
    struct IzletEntry {
        std::string yid;
        int kbps = 0;
        int viewers = 0;
        int lost = 0;
        int inKbps = 0;
    };
    std::vector<Entry> batch;
    std::vector<IzletEntry> izletBatch;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(sessionsMutex_);
        batch.reserve(sessions_.size());
        for (const auto& [wsid, s] : sessions_) {
            const auto st = s->webrtc->stats();
            const double sec = std::chrono::duration<double>(now - s->lastStatsAt).count();
            const std::uint64_t totalBytes = st.videoBytesSent + st.audioBytesSent;
            const std::uint64_t deltaBytes = totalBytes >= s->lastStatsBytes ? totalBytes - s->lastStatsBytes : 0;
            const std::uint64_t deltaFrames = st.videoFramesSent >= s->lastStatsFrames ? st.videoFramesSent - s->lastStatsFrames : 0;
            const int kbps = sec > 0.05 ? static_cast<int>(std::llround((double(deltaBytes) * 8.0 / 1000.0) / sec)) : 0;
            const double fps = sec > 0.05 ? (double(deltaFrames) / sec) : 0.0;
            s->lastStatsAt = now;
            s->lastStatsBytes = totalBytes;
            s->lastStatsFrames = st.videoFramesSent;

            batch.push_back({wsid, kbps, std::round(fps * 10.0) / 10.0, st.rttMs, s->lastPs5Loss.load()});
            oviora::log::info("gecit-stats", std::format("wsid={} kbps={} fps={:.1f} rttMs={} loss={:.3f}",
                                                         s->shortId, kbps, fps, st.rttMs, s->lastPs5Loss.load()));
        }

        const double izletSec = std::chrono::duration<double>(now - lastIzletStatsAt_).count();
        lastIzletStatsAt_ = now;
        izletBatch.reserve(izletBroadcasts_.size());
        for (const auto& [yid, b] : izletBroadcasts_) {
            const auto st = b->collectStats(izletSec);
            izletBatch.push_back({yid, st.outboundKbps, st.viewers, st.lostPackets, st.publisherKbps});
            oviora::log::info("izlet-stats", std::format("yid={} kbps={} viewers={} inKbps={} lost={}",
                                                         shortWsid(yid), st.outboundKbps, st.viewers, st.publisherKbps, st.lostPackets));
        }
    }
    for (const auto& e : batch) {
        php_.sendStats(e.wsid, e.kbps, e.fps, e.rttMs, e.loss);
    }
    for (const auto& iz : izletBatch) {
        php_.sendIzletStats(iz.yid, iz.kbps, iz.viewers, iz.lost, iz.inKbps);
    }
}

void GatewaySessionManager::startIzletBroadcast(const GwIzletStartJob& job) {
    if (shuttingDown_) return;
    const std::string shortId = shortWsid(job.yid);
    const auto codecOpt = parseIzletCodec(job.codec);
    if (!codecOpt) {
        oviora::log::warn("izlet-sfu", std::format("yid={} unsupported codec {}", shortId, job.codec));
        php_.sendStateFor("yid", job.yid, "hata", "Desteklenmeyen video codec'i.");
        return;
    }

    {
        std::lock_guard lock(sessionsMutex_);
        if (izletBroadcasts_.contains(job.yid)) {
            oviora::log::warn("izlet-sfu", std::format("yid={} duplicate izlet_start ignored", shortId));
            return;
        }
    }

    IzletBroadcastCallbacks cb;
    cb.onPublisherSignal = [this](const std::string& yid, const std::string& jsonStr) {
        QMetaObject::invokeMethod(this, [this, yid, jsonStr]() {
            if (!shuttingDown_) php_.sendSignalFor("yid", yid, jsonStr);
        }, Qt::QueuedConnection);
    };
    cb.onPublisherState = [this](const std::string& yid, const std::string& state, const std::string& err) {
        QMetaObject::invokeMethod(this, [this, yid, state, err]() {
            if (!shuttingDown_) php_.sendStateFor("yid", yid, state, err);
        }, Qt::QueuedConnection);
    };
    cb.onPublisherEnded = [this](const std::string& yid, const std::string& reason) {
        QMetaObject::invokeMethod(this, [this, yid, reason]() {
            stopIzletBroadcast(yid, reason, true);
        }, Qt::QueuedConnection);
    };
    cb.onViewerSignal = [this](const std::string& iid, const std::string& jsonStr) {
        QMetaObject::invokeMethod(this, [this, iid, jsonStr]() {
            if (!shuttingDown_) php_.sendSignalFor("iid", iid, jsonStr);
        }, Qt::QueuedConnection);
    };
    cb.onViewerState = [this](const std::string& iid, const std::string& state, const std::string& err) {
        QMetaObject::invokeMethod(this, [this, iid, state, err]() {
            if (!shuttingDown_) php_.sendStateFor("iid", iid, state, err);
        }, Qt::QueuedConnection);
    };
    cb.onViewerEnded = [this](const std::string&, const std::string& iid, const std::string& reason) {
        QMetaObject::invokeMethod(this, [this, iid, reason]() {
            leaveIzletViewer(iid, reason, true);
        }, Qt::QueuedConnection);
    };

    auto broadcast = std::make_shared<IzletBroadcastSession>(job.yid, *codecOpt, config_, std::move(cb));
    {
        std::lock_guard lock(sessionsMutex_);
        if (izletBroadcasts_.contains(job.yid)) return;
        izletBroadcasts_.emplace(job.yid, broadcast);
    }

    oviora::log::info("izlet-sfu", std::format("yid={} started codec={}", shortId, izletCodecName(*codecOpt)));
    php_.sendStateFor("yid", job.yid, "baglaniyor", "");
}

void GatewaySessionManager::joinIzletViewer(const GwIzleJoinJob& job) {
    if (shuttingDown_) return;
    std::shared_ptr<IzletBroadcastSession> broadcast;
    double currentEstimatedOutMbps = 0.0;
    {
        std::lock_guard lock(sessionsMutex_);
        if (viewerToBroadcast_.contains(job.iid)) {
            return;
        }
        const auto it = izletBroadcasts_.find(job.yid);
        if (it == izletBroadcasts_.end()) {
            php_.sendStateFor("iid", job.iid, "hata", "Yayın bulunamadı veya sona erdi.");
            return;
        }
        broadcast = it->second;
        for (const auto& [_, b] : izletBroadcasts_) {
            currentEstimatedOutMbps += b->estimatedInputMbps() * static_cast<double>(b->viewerCount());
        }
    }

    if (!canAcceptViewerBandwidth(currentEstimatedOutMbps, broadcast->estimatedInputMbps(), config_.maxViewerMbps)) {
        oviora::log::warn("izlet-sfu", std::format("iid={} rejected: bandwidth cap {} Mbps reached",
                                                  shortWsid(job.iid), config_.maxViewerMbps));
        php_.sendStateFor("iid", job.iid, "hata", "Sunucu izleyici çıkış kapasitesi dolu.");
        return;
    }

    std::string err;
    if (!broadcast->addViewer(job.iid, &err)) {
        oviora::log::warn("izlet-sfu", std::format("iid={} rejected: {}", shortWsid(job.iid), err));
        php_.sendStateFor("iid", job.iid, "hata", err.empty() ? "İzleyici eklenemedi." : err);
        return;
    }

    {
        std::lock_guard lock(sessionsMutex_);
        viewerToBroadcast_[job.iid] = job.yid;
    }
    php_.sendStateFor("iid", job.iid, "baglaniyor", "");
}

void GatewaySessionManager::leaveIzletViewer(const std::string& iid, const std::string& reason, bool notifyPhp) {
    std::shared_ptr<IzletBroadcastSession> broadcast;
    {
        std::lock_guard lock(sessionsMutex_);
        const auto vIt = viewerToBroadcast_.find(iid);
        if (vIt == viewerToBroadcast_.end()) return;
        const std::string yid = vIt->second;
        viewerToBroadcast_.erase(vIt);
        const auto bIt = izletBroadcasts_.find(yid);
        if (bIt != izletBroadcasts_.end()) {
            broadcast = bIt->second;
        }
    }
    if (broadcast) {
        broadcast->removeViewer(iid, reason, notifyPhp);
    }
}

void GatewaySessionManager::stopIzletBroadcast(const std::string& yid, const std::string& reason, bool notifyPhp) {
    std::shared_ptr<IzletBroadcastSession> broadcast;
    {
        std::lock_guard lock(sessionsMutex_);
        const auto it = izletBroadcasts_.find(yid);
        if (it == izletBroadcasts_.end()) return;
        broadcast = std::move(it->second);
        izletBroadcasts_.erase(it);
        for (auto vIt = viewerToBroadcast_.begin(); vIt != viewerToBroadcast_.end();) {
            if (vIt->second == yid) {
                vIt = viewerToBroadcast_.erase(vIt);
            } else {
                ++vIt;
            }
        }
    }
    oviora::log::info("izlet-sfu", std::format("yid={} stopping reason={}",
                                              shortWsid(yid), reason.empty() ? "none" : reason));
    if (broadcast) {
        broadcast->closeAll(notifyPhp);
    }
    if (notifyPhp) {
        php_.sendStateFor("yid", yid, "bitti", "");
    }
}

void GatewaySessionManager::runRealPs5Owner(const std::shared_ptr<ActiveSession>& session, GwStartJob job) {
    oviora::remoteplay::NativeConnectRequest req;
    req.video.width = (job.width == 1280 && job.height == 720) ? 1280u : 1920u;
    req.video.height = (job.width == 1280 && job.height == 720) ? 720u : 1080u;
    req.video.fps = (job.fps == 30) ? 30u : 60u;
    req.video.bitrateKbps = std::clamp(job.bitrateKbps, 1000u, 100000u);
    req.video.codec = oviora::remoteplay::Codec::H264; // Contract: always H.264.
    req.viewOnly = false;
    req.adaptiveResolution = false; // Contract: adaptive resolution off.

    auto route = std::make_shared<oviora::remoteplay::PsnRoute>();
    route->duid = job.psnDuid;
    route->token = std::move(job.psnToken);
    if (const auto aid = oviora::remoteplay::accountIdFromBase64(job.psnAccountIdB64)) {
        route->accountId = *aid;
    }
    req.psn = route;
    job.wipeToken();

    auto inbox = std::make_shared<oviora::remoteplay::SessionInbox>();
    auto inboxToken = inbox->begin(oviora::remoteplay::monotonic100ns());
    inbox->streamProfile(req.video);

    oviora::remoteplay::MediaRelayCallbacks relay;
    relay.onVideo = [session, this](oviora::remoteplay::SampleKind kind,
                                    std::span<const std::uint8_t> bytes,
                                    std::uint32_t, std::uint32_t, bool idr, bool lostOrRecovered) {
        if (session->stopRequested.load(std::memory_order_relaxed)) return;
        std::optional<std::vector<std::uint8_t>> outPacket;
        {
            std::lock_guard cLock(session->cacheMutex);
            if (kind == oviora::remoteplay::SampleKind::CodecConfig) {
                session->h264Cache.setConfig(bytes);
                return;
            }
            outPacket = session->h264Cache.processAccessUnit(bytes, idr, lostOrRecovered);
            if (lostOrRecovered || (session->h264Cache.waitingForIdr() && !idr)) {
                session->idrRequested.store(true, std::memory_order_relaxed);
            }
        }
        if (outPacket && !outPacket->empty()) {
            if (session->webrtc->sendVideoFrame(*outPacket) && !session->canliSent.exchange(true)) {
                oviora::log::info("gecit", std::format("wsid={} first frame sent (canli)", session->shortId));
                session->webrtc->sendState("canli");
                QMetaObject::invokeMethod(this, [this, wsid = session->wsid]() {
                    if (!shuttingDown_) php_.sendState(wsid, "canli", "");
                }, Qt::QueuedConnection);
            }
        }
    };
    relay.onAudio = [session](std::span<const std::uint8_t> opusPacket,
                              std::uint32_t, std::uint32_t, std::uint32_t frameSize) {
        if (session->stopRequested.load(std::memory_order_relaxed)) return;
        session->webrtc->sendAudioPacket(opusPacket, frameSize > 0 ? frameSize : 480u);
    };

    oviora::remoteplay::ChiakiBackend backend;
    const auto startRes = backend.start(req, inboxToken, false, std::move(relay));

    // Wipe PSN token from memory immediately after Chiaki consumes it.
    if (!route->token.empty()) {
        SecureZeroMemory(route->token.data(), route->token.size());
        route->token.clear();
    }
    req.psn.reset();
    route.reset();

    if (!startRes.ok) {
        const auto nativeSnap = backend.snapshot();
        const auto inboxSnap = inbox->quickSnapshot();
        const std::string errMsg = mapFailureMessage(nativeSnap, inboxSnap);
        oviora::log::warn("gecit", std::format("wsid={} ps5 start failed op={} code={} quit={}",
                                               session->shortId, startRes.operation, startRes.code, nativeSnap.lastQuitReason));
        session->webrtc->sendError(errMsg);
        session->webrtc->sendState("hata");
        QMetaObject::invokeMethod(this, [this, wsid = session->wsid, errMsg]() {
            if (!shuttingDown_) php_.sendState(wsid, "hata", errMsg);
            stopSession(wsid, "ps5 start failed", false);
        }, Qt::QueuedConnection);
        return;
    }

    const auto timer = std::unique_ptr<void, decltype(&CloseHandle)>(
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS),
        &CloseHandle);
    auto nextController = std::chrono::steady_clock::now();
    auto lastIdrSent = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (!session->stopRequested.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        const auto hostNow = oviora::remoteplay::monotonic100ns();

        if (session->idrRequested.exchange(false, std::memory_order_relaxed)) {
            if (now - lastIdrSent >= std::chrono::milliseconds(500)) {
                (void)backend.requestIdr();
                lastIdrSent = now;
            }
        }

        if (now >= nextController) {
            oviora::remoteplay::ControllerState state{};
            {
                std::lock_guard lock(session->inputMutex);
                // 100 ms staleness rule (same as desktop RemotePlaySessionSource).
                if (session->lastInput100ns > 0 && (hostNow - session->lastInput100ns) <= 1000000) {
                    state = session->controller;
                } else {
                    session->controller = {};
                }
            }
            (void)backend.submitController(state);
            nextController = now + std::chrono::milliseconds(4);
        }

        const auto fb = backend.takeFeedback();
        if (fb.rumble) {
            session->webrtc->sendRumble(fb.left, fb.right);
        }

        const auto nativeSnap = backend.snapshot();
        session->lastPs5Loss.store(nativeSnap.packetLoss, std::memory_order_relaxed);
        const auto inboxSnap = inbox->quickSnapshot();
        if (inboxSnap.state == oviora::remoteplay::SessionState::Failed ||
            inboxSnap.state == oviora::remoteplay::SessionState::LoginPinRequired) {
            const std::string errMsg = mapFailureMessage(nativeSnap, inboxSnap);
            const bool normalEnd = (nativeSnap.lastQuitReason == 12);
            const std::string stateStr = normalEnd ? "bitti" : "hata";
            oviora::log::warn("gecit", std::format("wsid={} ps5 ended state={} quitReason={} appReason={:#x}",
                                                   session->shortId, stateStr, nativeSnap.lastQuitReason, nativeSnap.applicationReason));
            if (!normalEnd) session->webrtc->sendError(errMsg);
            session->webrtc->sendState(stateStr);
            QMetaObject::invokeMethod(this, [this, wsid = session->wsid, stateStr, errMsg]() {
                if (!shuttingDown_) php_.sendState(wsid, stateStr, stateStr == "hata" ? errMsg : "");
                stopSession(wsid, "ps5 ended", false);
            }, Qt::QueuedConnection);
            break;
        }

        const auto waitUs = std::clamp<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(nextController - std::chrono::steady_clock::now()).count(),
            100, 4000);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -waitUs * 10;
            if (SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer.get(), 20);
            } else {
                Sleep(1);
            }
        } else {
            Sleep(1);
        }
    }

    inbox->invalidate();
    const auto stopRes = backend.stop();
    if (stopRes.ok) {
        inbox->finishStop();
    }
}

void GatewaySessionManager::runMockSourceOwner(const std::shared_ptr<ActiveSession>& session, GwStartJob job) {
    job.wipeToken();
    if (mockFrames_.empty()) {
        const std::string errMsg = "Deneme video dosyası yüklenemedi.";
        session->webrtc->sendError(errMsg);
        session->webrtc->sendState("hata");
        QMetaObject::invokeMethod(this, [this, wsid = session->wsid, errMsg]() {
            if (!shuttingDown_) php_.sendState(wsid, "hata", errMsg);
            stopSession(wsid, "mock file missing", false);
        }, Qt::QueuedConnection);
        return;
    }

    const auto timer = std::unique_ptr<void, decltype(&CloseHandle)>(
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS),
        &CloseHandle);

    constexpr std::uint8_t kOpusSilence20ms[3] = {0xFC, 0xFF, 0xFE};
    const auto frameInterval = std::chrono::microseconds(16667); // 60 fps
    const auto audioInterval = std::chrono::milliseconds(20);
    auto nextVideo = std::chrono::steady_clock::now();
    auto nextAudio = nextVideo;
    std::size_t frameIdx = 0;

    while (!session->stopRequested.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();

        if (!session->webrtc->isVideoReady()) {
            Sleep(10);
            nextVideo = std::chrono::steady_clock::now();
            nextAudio = nextVideo;
            continue;
        }

        if (session->idrRequested.exchange(false, std::memory_order_relaxed)) {
            std::lock_guard cLock(session->cacheMutex);
            session->h264Cache.requireKeyframe();
            // Jump to the next IDR frame (or frame 0) so the browser gets a keyframe immediately.
            bool jumped = false;
            for (std::size_t k = 0; k < mockFrames_.size(); ++k) {
                const std::size_t cand = (frameIdx + k) % mockFrames_.size();
                if (mockFrames_[cand].isIdr) {
                    frameIdx = cand;
                    jumped = true;
                    break;
                }
            }
            if (!jumped) frameIdx = 0;
        }

        if (now >= nextVideo) {
            const auto& f = mockFrames_[frameIdx];
            frameIdx = (frameIdx + 1) % mockFrames_.size();
            std::optional<std::vector<std::uint8_t>> pkt;
            {
                std::lock_guard cLock(session->cacheMutex);
                if (!f.configBytes.empty()) session->h264Cache.setConfig(f.configBytes);
                pkt = session->h264Cache.processAccessUnit(f.frameBytes, f.isIdr, false);
            }
            if (pkt && !pkt->empty()) {
                if (session->webrtc->sendVideoFrame(*pkt) && !session->canliSent.exchange(true)) {
                    oviora::log::info("gecit", std::format("wsid={} mock first frame sent (canli)", session->shortId));
                    session->webrtc->sendState("canli");
                    QMetaObject::invokeMethod(this, [this, wsid = session->wsid]() {
                        if (!shuttingDown_) php_.sendState(wsid, "canli", "");
                    }, Qt::QueuedConnection);
                }
            }
            nextVideo += frameInterval;
            if (nextVideo < now - std::chrono::milliseconds(100)) nextVideo = now + frameInterval;
        }

        if (now >= nextAudio) {
            session->webrtc->sendAudioPacket(kOpusSilence20ms, 960u);
            nextAudio += audioInterval;
            if (nextAudio < now - std::chrono::milliseconds(100)) nextAudio = now + audioInterval;
        }

        const auto nextWake = std::min(nextVideo, nextAudio);
        const auto waitUs = std::clamp<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(nextWake - std::chrono::steady_clock::now()).count(),
            200, 16667);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -waitUs * 10;
            if (SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer.get(), 20);
            } else {
                Sleep(1);
            }
        } else {
            Sleep(1);
        }
    }
}

void GatewaySessionManager::shutdownAll() {
    shuttingDown_ = true;
    if (statsTimer_) statsTimer_->stop();
    std::vector<std::string> ids;
    std::vector<std::string> yids;
    {
        std::lock_guard lock(sessionsMutex_);
        ids.reserve(sessions_.size());
        for (const auto& [wsid, _] : sessions_) ids.push_back(wsid);
        yids.reserve(izletBroadcasts_.size());
        for (const auto& [yid, _] : izletBroadcasts_) yids.push_back(yid);
    }
    for (const auto& id : ids) {
        stopSession(id, "gateway shutdown", false);
    }
    for (const auto& yid : yids) {
        stopIzletBroadcast(yid, "gateway shutdown", false);
    }
}

} // namespace oviora::gecit
