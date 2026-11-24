// QML bridge: what a running PS5 stream offers besides the picture and the pad, ported from the
// old Oviora (QmlBackend / StreamSession): rest mode, the stream statistics, the microphone, the
// consoles hidden from the home, auto connect at start and the PC going to sleep.
#include "../QmlPlayerBridge_p.h"

namespace oviora::ui {
namespace {
bool remoteRunning(const engine::PlayerSnapshot& s) { return s.remotePlay && s.running && !s.failed; }
}

// --- rest mode ------------------------------------------------------------------------------------
void QmlPlayerBridge::ps5Rest() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    const auto s = impl_->engine.snapshot();
    if (!s.remotePlay || !s.running) { ps5Cancel(); return; }
    oviora::log::info("remoteplay-ui", "rest mode: goto bed, then the stream ends");
    impl_->engine.remotePlayGotoBed();
    // The ctrl channel sends it on its own thread; the session ends a moment later so the
    // message leaves first (the old app stopped right after, on a session it owned itself).
    const auto session = s.sessionId;
    QTimer::singleShot(800, this, [this, session] {
        if (impl_->engine.snapshot().sessionId == session && impl_->snapshot.remotePlay) stopPlayback();
    });
#else
    ps5Cancel();
#endif
}

void QmlPlayerBridge::pcSuspending() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    // Every Qt window gets the broadcast: once is enough.
    const auto now = std::chrono::steady_clock::now();
    if (now - impl_->lastSuspend < std::chrono::seconds(10)) return;
    impl_->lastSuspend = now;
    const auto s = impl_->engine.snapshot();
    if (!s.remotePlay || !(s.running || s.transport == engine::TransportState::Opening)) return;
    // Genel → Oyunu bırakınca → Bilgisayar uykuya geçince (old: suspendAction, default nothing)
    const bool rest = impl_->prefString("suspendAction") == QLatin1String("sleep");
    oviora::log::info("remoteplay-ui", std::format("PC going to sleep during a PS5 stream: {}", rest ? "rest mode, then disconnect" : "disconnect"));
    if (rest && s.running) {
        impl_->engine.remotePlayGotoBed();
        // Windows gives a sleeping app about two seconds; the message needs a few milliseconds.
        Sleep(500);
    }
    stopPlayback();
#endif
}

// --- statistics -----------------------------------------------------------------------------------
QVariantMap QmlPlayerBridge::streamStats() const {
    const auto& s = impl_->snapshot;
    if (!remoteRunning(s)) return {{"active", false}};
    const auto& r = s.remoteStream;
    const double measured = s.remoteMeasuredMbps > 0 ? s.remoteMeasuredMbps : r.videoMbps;
    // For the stream's one-time frame generation hint (Main.qml): whether this PC can run any
    // FG and whether the chain holds an FG stage the user turned off.
    const auto caps = currentEffectCapabilities();
    static constexpr const char* kFgIds[] = {"dlss", "xess", "fsr3", "fsr4", "vfg"};
    const bool fgSupported = std::any_of(std::begin(kFgIds), std::end(kFgIds),
        [&](const char* id) { return caps.value(id).toMap().value("available").toBool(); });
    bool fgTurnedOff = false;
    for (uint32_t n = 0; n < impl_->chain.nodeCount; ++n)
        fgTurnedOff = fgTurnedOff || (impl_->chain.nodes[n].type == engine::EffectType::FrameGeneration && !impl_->chain.nodes[n].enabled);
    return {{"active", true},
            {"width", int(s.sourceWidth ? s.sourceWidth : r.requestedProfile.width)},
            {"height", int(s.sourceHeight ? s.sourceHeight : r.requestedProfile.height)},
            {"fps", s.remoteRatesReady ? s.remoteReceivedFps : 0.0},
            {"mbps", measured},
            {"requestedMbps", r.requestedProfile.bitrateKbps / 1000.0},
            {"packetLoss", s.remotePacketLoss},
            {"rttMs", s.remoteRttMs},
            {"framesLost", qulonglong(r.framesLost)},
            {"skipped", qulonglong(s.remotePlaySkipped)},
            {"linkReady", s.remoteLinkReady},
            {"internet", impl_->connectionPsn},
            {"requestedFps", int(r.requestedProfile.fps)},
            {"fgOn", s.applied.multiplier > 1},
            {"fgSupported", fgSupported},
            {"fgTurnedOff", fgTurnedOff}};
}

// --- statistics panel over the picture --------------------------------------------------------------
// The old overlay was a Qt Quick item cut out of the video window, so it could not be see-through.
// It is now a layered child of the video window (VideoOverlays.cpp), redrawn about twice a second.
namespace {
QString num(double v, int decimals) { return QLocale(QLocale::Turkish).toString(v, 'f', decimals); }
QString ms(double v) { return (v < 10 ? num(v, 2) : num(v, 1)) + QStringLiteral(" ms"); }
constexpr QRgb kDanger = 0xFFFF6B6B, kWarm = 0xFFFFB86B, kOk = 0xFF6FDDA4, kFaint = 0xFF8E8899;
}
QRectF QmlPlayerBridge::statsOverlayRect() const { return impl_->statsRect; }
void QmlPlayerBridge::setStatsOverlayShown(bool shown) {
    if (impl_->statsWanted == shown) return;
    impl_->statsWanted = shown;
    oviora::log::info("stats-overlay", shown ? "shown" : "hidden");
    impl_->statsTicks = 0;
    tickStatsOverlay();
}
// Video client pixels -> the main window's logical pixels (QML puts the weak-link mark under it)
static QRectF statsRectInWindow(HWND video, const QRect& px) {
    if (px.isEmpty() || !video) return {};
    POINT topLeft{px.left(), px.top()};
    MapWindowPoints(video, GetAncestor(video, GA_ROOT), &topLeft, 1);
    const double dpr = std::max(1.0, GetDpiForWindow(video) / 96.0);
    return QRectF(topLeft.x / dpr, topLeft.y / dpr, px.width() / dpr, px.height() / dpr);
}
void QmlPlayerBridge::tickStatsOverlay() {
    auto& i = *impl_;
    const auto& s = i.snapshot;
    QRectF rect;
    if (i.statsWanted && remoteRunning(s) && i.videoWindow) {
        using Stage = diagnostics::GpuStage;
        const auto& r = s.remoteStream;
        std::vector<ui::StatsGroup> groups;
        // Yayın: what the console sends and what arrives
        {
            ui::StatsGroup g{tr("Yayın"), {}};
            const uint32_t w = s.sourceWidth ? s.sourceWidth : r.requestedProfile.width;
            const uint32_t h = s.sourceHeight ? s.sourceHeight : r.requestedProfile.height;
            g.rows.push_back({tr("Çözünürlük"), QStringLiteral("%1 × %2").arg(w).arg(h)});
            const auto codec = r.requestedProfile.codec;
            g.rows.push_back({tr("Kodek"), codec == remoteplay::Codec::H264 ? QStringLiteral("H.264")
                                         : codec == remoteplay::Codec::H265Hdr ? QStringLiteral("H.265 HDR") : QStringLiteral("H.265")});
            const double got = s.remoteMeasuredMbps > 0 ? s.remoteMeasuredMbps : r.videoMbps;
            g.rows.push_back({tr("Bit hızı (gelen / istenen)"),
                              QStringLiteral("%1 / %2 Mbps").arg(num(got, 1), num(r.requestedProfile.bitrateKbps / 1000.0, 0))});
            const double fps = s.remoteRatesReady ? s.remoteReceivedFps : 0.0;
            const QRgb fpsTone = s.remoteRatesReady && r.requestedProfile.fps && fps < r.requestedProfile.fps * 0.9 ? kWarm : 0;
            g.rows.push_back({tr("Kare hızı (gelen / PS5)"),
                              QStringLiteral("%1 / %2 fps").arg(s.remoteRatesReady ? num(fps, 0) : QStringLiteral("—")).arg(r.requestedProfile.fps), fpsTone});
            groups.push_back(std::move(g));
        }
        // Gecikme: network round trip, waiting before the decoder, decoding, arrival to the screen
        {
            ui::StatsGroup g{tr("Gecikme"), {}};
            if (s.remoteRttMs >= 0) g.rows.push_back({tr("Ağ (PS5 gidiş-dönüş)"), ms(s.remoteRttMs), s.remoteRttMs > 80 ? kDanger : 0});
            if (r.ingressWaitMeanMs) g.rows.push_back({tr("Kod çözücü sırası"), ms(*r.ingressWaitMeanMs)});
            if (r.decodeMeanMs)
                g.rows.push_back({tr("Kod çözme (ort. / P95)"),
                                  QStringLiteral("%1 / %2").arg(ms(*r.decodeMeanMs), r.decodeP95Ms ? ms(*r.decodeP95Ms) : QStringLiteral("—"))});
            if (s.captureAgeP95Ms > 0) g.rows.push_back({tr("Gelişten ekrana (P95)"), ms(s.captureAgeP95Ms), s.captureAgeP95Ms > 50 ? kWarm : 0});
            if (g.rows.empty()) g.rows.push_back({tr("Ölçülüyor"), QStringLiteral("…"), kFaint});
            groups.push_back(std::move(g));
        }
        // İşleme: what the engine does to the picture
        {
            ui::StatsGroup g{tr("İşleme"), {}};
            const auto& a = s.applied;
            if (a.multiplier > 1) {
                const QString name = fgBackendName();
                const QString method = name == QLatin1String("dlss") ? QStringLiteral("DLSS") : name == QLatin1String("xess") ? QStringLiteral("XeSS")
                                     : name == QLatin1String("fsr3") ? QStringLiteral("FSR 3") : name == QLatin1String("fsr4") ? QStringLiteral("FSR 4")
                                     : name == QLatin1String("vfg") ? QStringLiteral("NVIDIA VFG") : name.toUpper();
                g.rows.push_back({tr("Ara kare"), QStringLiteral("%1 · %2×").arg(method).arg(a.multiplier)});
            } else {
                g.rows.push_back({tr("Ara kare"), tr("Kapalı"), kFaint});
            }
            if (s.submissionFps && *s.submissionFps > 0) g.rows.push_back({tr("Çıkış kare hızı"), num(*s.submissionFps, 0) + QStringLiteral(" fps")});
            g.rows.push_back({tr("Süper çözünürlük"), a.sr ? tr("Açık") : tr("Kapalı"), a.sr ? kOk : kFaint});
            g.rows.push_back({tr("Neural Rendering (NR)"), a.nr ? tr("Açık") : tr("Kapalı"), a.nr ? kOk : kFaint});
            if (s.videoHdrActive) g.rows.push_back({QStringLiteral("RTX Video HDR"), tr("Açık"), kOk});
            groups.push_back(std::move(g));
        }
        // GPU maliyeti: the engine's GPU timestamps per stage, mean of the last second
        {
            ui::StatsGroup g{tr("GPU maliyeti"), {}};
            const bool live = queuedFramesKnown();
            // A stage measured once while the panel shows keeps its row (a dash while a second has
            // no sample), so the panel does not change height every second
            auto stage = [&](const QString& label, Stage st) {
                const auto& sample = s.metrics.flow.gpuTiming[size_t(st)];
                if (!live || sample.samples == 0 || !sample.mean || !std::isfinite(*sample.mean) || *sample.mean < 0) {
                    if (i.statsStages.contains(label)) g.rows.push_back({label, QStringLiteral("—"), kFaint});
                    return;
                }
                i.statsStages.insert(label);
                g.rows.push_back({label, ms(*sample.mean)});
            };
            stage(tr("Giriş"), Stage::Color);
            for (unsigned k = 0; k < diagnostics::kTimedColorNodes; ++k)
                stage(k ? tr("Renk %1").arg(k + 1) : tr("Renk"), diagnostics::colorNodeStage(k));
            for (unsigned k = 0; k < diagnostics::kTimedAaNodes; ++k)
                stage(k ? tr("Kenar yumuşatma %1").arg(k + 1) : tr("Kenar yumuşatma"), diagnostics::aaNodeStage(k));
            stage(tr("Süper çözünürlük"), Stage::Sr);
            stage(tr("Optik akış"), Stage::Flow);
            unsigned layers = 0;
            for (unsigned k = 0; k < diagnostics::kTimedNrLayers; ++k)
                if (s.metrics.flow.gpuTiming[size_t(diagnostics::nrLayerStage(k))].samples > 0) ++layers;
            if (layers >= 2) {
                for (unsigned k = 0; k < diagnostics::kTimedNrLayers; ++k) stage(tr("NR katman %1").arg(k + 1), diagnostics::nrLayerStage(k));
            } else {
                stage(QStringLiteral("NR"), Stage::Nr);
            }
            stage(tr("Artık düzeltme"), Stage::Residual);
            stage(tr("Ara kare"), Stage::FgBatch);
            stage(QStringLiteral("HDR"), Stage::VideoHdr);
            stage(tr("Sunum"), Stage::Blit);
            const QString totalLabel = tr("Toplam / kare bütçesi");
            if (chainTotalMsKnown() && chainTotalMs() > 0.005) {
                const double budget = stageBudgetMs();
                const double total = chainTotalMs();
                i.statsStages.insert(totalLabel);
                g.rows.push_back({totalLabel, budget > 0.01 ? QStringLiteral("%1 / %2").arg(ms(total), ms(budget)) : ms(total),
                                  budget > 0.01 && total > budget * 0.9 ? kDanger : budget > 0.01 && total > budget * 0.7 ? kWarm : 0});
            } else if (i.statsStages.contains(totalLabel)) {
                g.rows.push_back({totalLabel, QStringLiteral("—"), kFaint});
            }
            if (g.rows.empty()) g.rows.push_back({tr("Ölçülüyor"), QStringLiteral("…"), kFaint});
            groups.push_back(std::move(g));
        }
        // Donanım
        {
            ui::StatsGroup g{tr("Donanım"), {}};
            if (gpuUtilizationKnown()) {
                const double u = gpuUtilization();
                g.rows.push_back({tr("GPU kullanımı"), QStringLiteral("%") + num(u, 0), u > 90 ? kDanger : u > 75 ? kWarm : 0});
            }
            if (s.vramBudgetMiB > 0)
                g.rows.push_back({tr("Video belleği"),
                                  QStringLiteral("%1 / %2 GB").arg(num(s.vramUsageMiB / 1024.0, 1), num(s.vramBudgetMiB / 1024.0, 1)),
                                  s.vramUsageMiB > s.vramBudgetMiB * 9 / 10 ? kDanger : 0});
            g.rows.push_back({tr("Kod çözücü"), r.hardwareDecode ? tr("Donanım") : tr("Yazılım"), r.hardwareDecode ? 0 : kWarm});
            groups.push_back(std::move(g));
        }
        // Kayıplar
        {
            ui::StatsGroup g{tr("Kayıplar"), {}};
            const double loss = s.remotePacketLoss;
            g.rows.push_back({tr("Paket kaybı"), QStringLiteral("%") + num(loss * 100, 1), loss > 0.01 ? kDanger : 0});
            g.rows.push_back({tr("Atlanan kare"), QString::number(qulonglong(s.remotePlaySkipped)), s.remotePlaySkipped ? kWarm : 0});
            g.rows.push_back({tr("Kayıp kare"), QString::number(qulonglong(r.framesLost)), r.framesLost ? kDanger : 0});
            g.rows.push_back({tr("Tam kare (IDR) isteği"), QString::number(qulonglong(r.video.idrRequests)), r.video.idrRequests > 1 ? kWarm : 0});
            groups.push_back(std::move(g));
        }
        if (!i.statsOverlay) i.statsOverlay = std::make_unique<ui::StatsOverlay>();
        rect = statsRectInWindow(i.videoWindow, i.statsOverlay->update(i.videoWindow, groups));
    } else {
        if (i.statsOverlay) i.statsOverlay->hide();
        i.statsStages.clear();
    }
    if (rect != i.statsRect) { i.statsRect = rect; emit statsOverlayChanged(); }
}
// Every UI tick: the panel follows the window's size at once (it is only redrawn twice a second)
void QmlPlayerBridge::followStatsOverlay() {
    auto& i = *impl_;
    if (!i.statsOverlay || !i.statsWanted || !i.videoWindow) return;
    const QRectF rect = statsRectInWindow(i.videoWindow, i.statsOverlay->follow(i.videoWindow));
    if (rect != i.statsRect) { i.statsRect = rect; emit statsOverlayChanged(); }
}

// --- microphone -----------------------------------------------------------------------------------
bool QmlPlayerBridge::micOn() const { return impl_->micWanted; }
QStringList QmlPlayerBridge::microphones() const {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (!impl_->micDevicesRead) {
        impl_->micDevicesRead = true;
        impl_->micDevices.clear();
        for (const auto& name : remoteplay::Microphone::devices()) impl_->micDevices << QString::fromStdString(name);
    }
#endif
    return impl_->micDevices;
}
void QmlPlayerBridge::toggleMicrophone() {
    if (!remoteRunning(impl_->snapshot)) return;
    impl_->micWanted = !impl_->micWanted;
    oviora::log::info("remoteplay-mic", impl_->micWanted ? "turned on from the stream menu" : "turned off from the stream menu");
    emit micChanged();
    tickStreamSession();
}
void QmlPlayerBridge::tickStreamSession() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    const auto& s = i.snapshot;
    const bool active = remoteRunning(s);
    // The choice lasts for one stream (a reconnect inside it keeps it); a new stream starts with
    // the microphone as Ses ve ağ says (old: startMicUnmuted, off by default)
    if (i.micSession && (!s.remotePlay || s.sessionId != i.micSession)) {
        i.micSession = 0;
        if (i.micWanted) { i.micWanted = false; emit micChanged(); }
    }
    if (active && i.micSession != s.sessionId) {
        i.micSession = s.sessionId;
        const bool start = i.prefBool("micStartOn", false);
        if (start != i.micWanted) { i.micWanted = start; emit micChanged(); }
    }
    if (active && i.micWanted && !i.mic.running()) {
        auto& engine = i.engine;
        // OVIORA_TEST_MIC_SILENT=1 (test runs): the whole path runs, but only silence leaves the PC
        const bool silent = qEnvironmentVariableIsSet("OVIORA_TEST_MIC_SILENT");
        if (!i.mic.start(i.prefString("micDevice").toStdString(), [&engine, silent](std::vector<std::int16_t> block) {
                if (silent) std::fill(block.begin(), block.end(), std::int16_t(0));
                engine.remotePlayMicrophonePcm(std::move(block));
            })) {
            i.micWanted = false;
            emit micChanged();
            emit notice(tr("Mikrofon açılamadı. Windows'un ses ayarlarında bir mikrofon bağlı ve izinli olmalı."), true);
        }
    }
    if ((!active || !i.micWanted) && i.mic.running()) i.mic.stop();
    // The console's own state follows the wish (applied by the session once it is connected).
    if (active && i.micWanted != s.remoteMicrophoneOn) i.engine.remotePlayMicrophone(i.micWanted);
#endif
}

// --- consoles hidden from the home ----------------------------------------------------------------
QVariantList QmlPlayerBridge::hiddenConsoles() const { return impl_->prefs.value(QStringLiteral("hiddenConsoles")).toList(); }
void QmlPlayerBridge::lanHide(const QString& key) {
    for (const auto& v : lanConsoles()) {
        const auto c = v.toMap();
        if (c.value("key").toString() != key || c.value("registered").toBool()) continue;
        const QString id = c.value("id").toString();
        if (id.isEmpty()) return;
        auto list = hiddenConsoles();
        for (const auto& h : list) if (h.toMap().value("id").toString() == id) return;
        list << QVariantMap{{"id", id}, {"name", c.value("name").toString()}};
        impl_->prefs[QStringLiteral("hiddenConsoles")] = list;
        if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "hidden consoles not saved");
        oviora::log::info("ui-prefs", std::format("console hidden from the home; hidden={}", list.size()));
        emit preferencesChanged();
        emit lanChanged();
        return;
    }
}
void QmlPlayerBridge::lanUnhide(const QString& id) {
    auto list = hiddenConsoles();
    const auto before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const QVariant& h) { return h.toMap().value("id").toString() == id; }), list.end());
    if (list.size() == before) return;
    impl_->prefs[QStringLiteral("hiddenConsoles")] = list;
    if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "hidden consoles not saved");
    oviora::log::info("ui-prefs", std::format("console shown on the home again; hidden={}", list.size()));
    emit preferencesChanged();
    emit lanChanged();
}

// --- auto connect at start ------------------------------------------------------------------------
void QmlPlayerBridge::tickAutoConnect() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (i.autoConnectPending.isEmpty()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto s = i.engine.snapshot();
    if (s.remotePlay || s.running || i.openingSource) {
        oviora::log::info("remoteplay-ui", "auto connect: another stream started; not waiting any more");
        i.autoConnectPending.clear();
        return;
    }
    if (now > i.autoConnectUntil) {
        oviora::log::info("remoteplay-ui", "auto connect: the PS5 was not found on this network in time");
        i.autoConnectPending.clear();
        return;
    }
    if (i.ps5Busy) return;
    for (const auto& v : lanConsoles()) {
        const auto c = v.toMap();
        if (c.value("key").toString() != i.autoConnectPending || !c.value("registered").toBool() || !c.value("discovered").toBool()) continue;
        if (c.value("state").toString() == QLatin1String("ready")) {
            const QString key = i.autoConnectPending;
            i.autoConnectPending.clear();
            oviora::log::info("remoteplay-ui", "auto connect: the PS5 is on, connecting");
            lanConnect(key);
        } else if (!i.autoConnectWoke) {
            // Old Oviora woke a resting console first, then connected once it was on
            i.autoConnectWoke = true;
            i.autoConnectUntil = now + std::chrono::seconds(60);
            oviora::log::info("remoteplay-ui", "auto connect: the PS5 rests, waking it");
            lanWake(c.value("key").toString());
        }
        return;
    }
#endif
}
} // namespace oviora::ui
