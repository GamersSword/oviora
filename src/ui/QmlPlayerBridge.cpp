// QML bridge: Core responsibilities.
#include "QmlPlayerBridge_p.h"
#include "oviora/gfx/PresentationHooks.h"
#include <QThreadPool>

namespace oviora::ui {
namespace {
bool prepareStartupPreset(const engine::PresetEntry& entry, engine::ChainSession& session,
                          engine::EnhancementSettings& settings, QString& error) {
    // The node editor was removed: only a list preset can be the default.
    if (entry.kind != engine::ChainMode::List || !session.select(engine::ChainMode::List)) {
        error = QObject::tr("默认预设的编辑模式无效");
        return false;
    }
    auto& configuration = session.configurations[size_t(engine::ChainMode::List)];
    configuration.apply(settings);
    auto chain = configuration.chain;
    const auto result = engine::PresetLibrary::applyToChain(entry, chain, settings);
    if (!result.accepted) { error = uiText(result.message); return false; }
    configuration = engine::ChainConfiguration::capture(chain, settings);
    if (!session.valid() || !settings.validate().empty()) {
        error = QObject::tr("默认预设参数无效，已保留恢复会话");
        return false;
    }
    return true;
}
} // namespace
QmlPlayerBridge::QmlPlayerBridge(engine::EngineController& engine, std::filesystem::path dataDirectory,
                                QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(engine, std::move(dataDirectory))) {
    // The compute GPU decides which effects exist, so it is settled before
    // anything below samples the capabilities or restores the saved chain.
    impl_->loadPrefs();
    // The last volume (user 2026-11-08: it came back at 100% every launch).
    // Mute stays per session; profiles without the key keep 100%.
    if (impl_->prefs.contains(QStringLiteral("volume"))) {
        bool ok = false;
        const double saved = impl_->prefs.value(QStringLiteral("volume")).toDouble(&ok);
        if (ok && std::isfinite(saved) && saved >= 0.0 && saved <= 1.0) {
            impl_->engine.setVolume(float(saved), false);
            impl_->snapshot.volume = float(saved);
            oviora::log::info("ui-prefs", std::format("volume restored={:.3f}", saved));
        }
    }
    logShortcuts();
    impl_->enumerateGpuAdapters();
    impl_->resolveGpuMonitor();
    impl_->applyComputeGpu();
    connect(this,&QmlPlayerBridge::snapshotChanged,this,
        [this,ceiling=fgMaxMultiplier(),text=fgProviderText()]() mutable {
            const auto nextCeiling=fgMaxMultiplier();const auto nextText=fgProviderText();
            if(nextCeiling==ceiling&&nextText==text)return;
            ceiling=nextCeiling;text=nextText;emit fgChoicesChanged();
        });
    connect(this,&QmlPlayerBridge::settingsChanged,this,&QmlPlayerBridge::fgChoicesChanged);
    auto* capabilityTimer=new QTimer(this);
    connect(capabilityTimer,&QTimer::timeout,this,[this,reported=currentEffectCapabilities(),priorityRevision=uint64_t(-1),tick=0]() mutable {
        const bool trace = qApp && qApp->property("ovioraTraceUiStalls").toBool();
        const auto t0 = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if(gfx::gpuPriorityStatus().state==gfx::GpuPriorityState::Pending)gfx::applyRequestedGpuPriority();
        const auto priority=gfx::gpuPriorityStatus();
        if(priority.revision!=priorityRevision){priorityRevision=priority.revision;emit preferencesChanged();}
        // Oviora: the runtime file probe (a newly downloaded component) every 10 s, and never on the
        // UI thread while a stream runs; actions still refresh it at once
        if(++tick%5!=0||impl_->snapshot.running)return;
        const auto next=currentEffectCapabilities(true);
        if (trace) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1.0) oviora::log::info("ui-stall-timer", std::format("capabilityTimer ms={:.2f}", ms));
        }
        // An action may have refreshed the cache before this tick. Compare with
        // the last reported map so that QML still learns about that change.
        if(next==reported)return;
        reported=next;emit effectCapabilitiesChanged();emit fgChoicesChanged();
    });
    capabilityTimer->start(2000);
    applyPreference(QStringLiteral("gpuPriority"));
    updateLiveCompanionCompat();
    impl_->engine.setFullscreenMemoryProtection(impl_->prefBool("fullscreenMemoryProtection",false));
    if (qApp) qApp->installEventFilter(this);
    impl_->displayChangeFilter.changed = &impl_->displayChanged;
    if (qApp) qApp->installNativeEventFilter(&impl_->displayChangeFilter);
    // Before anything opens: the renderer reads the output choice at start().
    applyPreference(QStringLiteral("language"));
    applyPreference(QStringLiteral("audioDevice"));
    applyPreference(QStringLiteral("audioForceStereo"));
    loadGameInput();
    // The PC going to sleep during a PS5 stream (Genel → Oyunu bırakınca), and auto connect
    // to the chosen PS5 at start (Konsollar → Otomatik bağlan; bridge/StreamSession.cpp).
    impl_->powerFilter.suspending = [this] { pcSuspending(); };
    if (qApp) qApp->installNativeEventFilter(&impl_->powerFilter);
    impl_->autoConnectPending = impl_->prefString("autoConnect");
    impl_->autoConnectUntil = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    if (!impl_->autoConnectPending.isEmpty()) oviora::log::info("remoteplay-ui", "auto connect: waiting for the chosen PS5 on this network");
    const bool presetsLoaded = impl_->facade.importLegacyStores();
    const auto presetNotice = uiText(impl_->facade.error());
    if (!presetNotice.isEmpty())
        QTimer::singleShot(0, this, [this, presetNotice, presetsLoaded] { emit notice(presetNotice, !presetsLoaded); });
    // Recent files, the last capture session and the shell preferences are saved
    // on every change; without this load they were written but never read back,
    // so every restart came up with an empty history. A damaged file is logged
    // and the defaults stand.
    if (!impl_->facade.loadPreferences())
        oviora::log::error("ui", "preferences not loaded: " + utf8Of(impl_->facade.error()).toStdString());
    auto restored = impl_->session;
    QString restoreError;
    if (!impl_->facade.loadChainSession(restored)) {
        restoreError = uiText(impl_->facade.error());
    } else {
        const auto migrationNotice = uiText(impl_->facade.error());
        if (!migrationNotice.isEmpty())
            QTimer::singleShot(0, this, [this, migrationNotice] { emit notice(migrationNotice, false); });
        // The node editor was removed: a session saved in node mode opens in list mode,
        // whose configuration is always kept beside it.
        if (restored.active != engine::ChainMode::List) {
            oviora::log::info("ui-session", "node mode retired: the saved session opens in list mode");
            restored.active = engine::ChainMode::List;
        }
        const auto savedConfiguration = restored;
        QStringList disabled;
        for (auto& configuration : restored.configurations) {
            const auto reason = disableUnavailableEffects(configuration);
            if (!reason.isEmpty()) disabled << reason;
        }
        if (!disabled.isEmpty()) {
            const auto message = tr("已关闭当前硬件或包不支持的效果，参数仍保留：") + disabled.join("; ");
            oviora::log::warn("effect-availability", message.toStdString());
            QTimer::singleShot(0, this, [this, message] { emit notice(message, false); });
        }
        auto settings = impl_->facade.pendingSettings();
        const auto& configuration = restored.configurations[size_t(restored.active)];
        configuration.apply(settings);
        const auto previous = impl_->facade.pendingSettings();
        if (settings.validate().empty() && impl_->facade.applySettings(settings,engine::runtimeOrder(configuration.chain))) {
            impl_->session = restored;
            // Persist a repaired backend as well as the active mode. Otherwise
            // every restart reloads the unavailable DLSS value again.
            impl_->savedSession = savedConfiguration;
            impl_->restoreConfiguration(configuration);
            impl_->options = engine::PlayerOptions::from(settings,engine::runtimeOrder(impl_->activeRuntime()));
            impl_->revalidate();
        } else {
            impl_->facade.setPending(previous);
            restoreError = tr("模式会话未被引擎接受，保留初始状态");
        }
    }
    bool defaultFlowApplied = false;
    if (!restoreError.isEmpty()) {
        oviora::log::error("ui-session", restoreError.toStdString());
        QTimer::singleShot(0, this, [this, restoreError] { emit notice(restoreError, true); });
    } else if (presetsLoaded) {
        const auto defaultIndex = impl_->facade.presets().defaultIndex();
        if (defaultIndex) {
            const auto before = impl_->currentSession();
            auto candidate = before;
            auto settings = impl_->facade.pendingSettings();
            QString error;
            const auto& entry = impl_->facade.presets().entries()[*defaultIndex];
            if (!prepareStartupPreset(entry, candidate, settings, error)) {
                oviora::log::error("ui-preset", error.toStdString());
            } else {
                auto& configuration = candidate.configurations[size_t(candidate.active)];
                const auto disabled = disableUnavailableEffects(configuration);
                configuration.apply(settings);
                if (!disabled.isEmpty()) oviora::log::warn("effect-availability", "default preset disabled: " + disabled.toStdString());
            if (!impl_->facade.applySettings(settings,
                                                    engine::runtimeOrder(candidate.configurations[size_t(candidate.active)].chain))) {
                error = tr("引擎未接受默认预设，已保留恢复会话");
            } else {
                impl_->session = candidate;
                impl_->restoreConfiguration(candidate.configurations[size_t(candidate.active)]);
                impl_->revalidate();
                impl_->options = engine::PlayerOptions::from(settings, engine::runtimeOrder(impl_->activeRuntime()));
                defaultFlowApplied = entry.flow.has_value() &&
                    (entry.contents & engine::presetContentMask(engine::PresetContent::Flow));
                if (defaultFlowApplied) {
                    impl_->prefs[QStringLiteral("contentRate")] = int(settings.content);
                    if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "default preset content cadence not saved");
                }
                if (candidate != before && !impl_->facade.saveChainSession(candidate))
                    error = tr("默认预设已应用，但会话保存失败：") + uiText(impl_->facade.error());
                else impl_->savedSession = candidate;
            }
            }
            if (!error.isEmpty())
                QTimer::singleShot(0, this, [this, error] { emit notice(error, true); });
        }
    }
    // These controls are global, outside either effect chain. Restore them
    // from shell preferences after the session; old profiles keep defaults.
    {
        auto s = impl_->facade.pendingSettings();
        const auto& p = impl_->prefs;
        bool changed = false;
        if (p.contains(QStringLiteral("audioSync"))) {
            const int m = p.value(QStringLiteral("audioSync")).toInt();
            if (m >= 0 && m <= 2 && int(s.audioSync) != m) { s.audioSync = static_cast<engine::AudioSyncMode>(m); changed = true; }
        }
        if (p.contains(QStringLiteral("audioOffsetMs"))) {
            const int ms = p.value(QStringLiteral("audioOffsetMs")).toInt();
            if (ms >= -250 && ms <= 250 && s.audioOffsetMs != ms) { s.audioOffsetMs = ms; changed = true; }
        }
        if (p.contains(QStringLiteral("edgeAegis"))) {
            const bool on = p.value(QStringLiteral("edgeAegis")).toBool();
            if (s.fgMotionRefine != on) { s.fgMotionRefine = on; changed = true; }
        }
        if (p.contains(QStringLiteral("dlssgPerformance"))) {
            const bool on = p.value(QStringLiteral("dlssgPerformance")).toBool();
            if (s.dlssgLite != on) { s.dlssgLite = on; changed = true; }
        }
        if (p.contains(QStringLiteral("vfgAutoRegulate"))) {
            const bool on = p.value(QStringLiteral("vfgAutoRegulate")).toBool();
            if (s.vfgAutoRegulate != on) { s.vfgAutoRegulate = on; changed = true; }
        }
        if (!defaultFlowApplied && p.contains(QStringLiteral("contentRate"))) {
            const int rate = p.value(QStringLiteral("contentRate")).toInt();
            if(rate>=0&&rate<=int(engine::ContentRate::Capture60To30)){
                const auto mode=engine::currentContentRate(static_cast<engine::ContentRate>(rate));
                if(s.content!=mode){s.content=mode;changed=true;}
                if(rate!=int(mode)){
                    impl_->prefs[QStringLiteral("contentRate")]=int(mode);
                    if(!impl_->savePrefs())oviora::log::warn("ui-prefs","duplicate-removal mode migration not saved");
                }
            }
        }
        auto restoreHold = [&](const char* key, float& target) {
            if (!p.contains(QLatin1String(key))) return;
            bool ok = false;
            const double value = p.value(QLatin1String(key)).toDouble(&ok);
            if (ok && std::isfinite(value) && value >= 0 && value <= 1 && target != float(value)) {
                target = float(value); changed = true;
            }
        };
        restoreHold("nrHoldStrength", s.nrHoldStrength);
        restoreHold("nrHoldTolerance", s.nrHoldTolerance);
        if (p.contains(QStringLiteral("nrReuse"))) {
            const int value = p.value(QStringLiteral("nrReuse")).toInt();
            const auto mode = static_cast<engine::NrReuseMode>(value);
            if (value >= 0 && engine::validNrReuseMode(mode) && s.nrReuse != mode) { s.nrReuse = mode; changed = true; }
        }
        if (p.contains(QStringLiteral("captureForceSdr"))) {
            const bool value = p.value(QStringLiteral("captureForceSdr")).toBool();
            changed |= s.forceSdrPreview != value; s.forceSdrPreview = value;
        }
        if (changed && !impl_->commit(std::move(s))) oviora::log::warn("ui-prefs", "saved global controls not accepted");
    }
    // Presentation (低延迟队列 / 显示同步 / 输出上限) and the picture aspect.
    {
        const auto saved = impl_->prefs.value(QStringLiteral("presentation")).toMap();
        auto& pr = impl_->presentation;
        pr.hdrFgCompatibility = impl_->prefBool("hdrFgCompatibility", false);
        if (!saved.isEmpty()) {
            pr.enabled = saved.value(QStringLiteral("lowQueue")).toBool();
            // Before NVIDIA-specific choices, the low queue meant On on NVIDIA.
            const int savedLatency=saved.value(QStringLiteral("nvidiaLowLatency"),pr.enabled&&nvidiaLowLatencyAvailable()?1:0).toInt();
            pr.nvidiaLowLatency=engine::savedNvidiaLowLatency(savedLatency);
            if(savedLatency!=int(pr.nvidiaLowLatency)){
                auto migrated=saved;migrated[QStringLiteral("nvidiaLowLatency")]=int(pr.nvidiaLowLatency);
                impl_->prefs[QStringLiteral("presentation")]=migrated;
                if(!impl_->savePrefs())oviora::log::warn("ui-prefs","retired NVIDIA latency mode migration not saved");
            }
            engine::syncNvidiaLowLatencyQueue(pr,nvidiaLowLatencyAvailable());
            pr.display = static_cast<engine::DisplaySync>(std::clamp(saved.value(QStringLiteral("display")).toInt(), 0, 2));
            pr.outputRate = static_cast<engine::OutputRateMode>(std::clamp(saved.value(QStringLiteral("outputRate")).toInt(), 0, 2));
            pr.customFps = std::clamp(saved.value(QStringLiteral("customFps"), 60.0).toDouble(), 1.0, 1000.0);
            pr.variableRefresh = saved.value(QStringLiteral("vrr")).toBool();
        }
        if (pr.valid()) impl_->engine.requestPresentation(pr);
        impl_->aspectMode = std::clamp(impl_->prefs.value(QStringLiteral("aspect")).toInt(), 0, 6);
        const double savedZoom = impl_->prefs.value(QStringLiteral("viewZoom"), 1.0).toDouble();
        impl_->viewZoom = std::isfinite(savedZoom) ? std::clamp(savedZoom, 1.0, 5.0) : 1.0;
    }
    impl_->sessionSaveTimer = new QTimer(this);
    impl_->sessionSaveTimer->setSingleShot(true);
    impl_->sessionSaveTimer->setInterval(300);
    const auto scheduleSave = [this] { impl_->sessionSaveTimer->start(); };
    connect(this, &QmlPlayerBridge::chainChanged, this, scheduleSave);
    connect(this, &QmlPlayerBridge::settingsChanged, this, scheduleSave);
    connect(impl_->sessionSaveTimer, &QTimer::timeout, this, [this] {
        if (!impl_->persistSession()) {
            oviora::log::error("ui-session", utf8Of(impl_->facade.error()).toStdString());
            emit notice(tr("当前修改尚未保存：") + uiText(impl_->facade.error()), true);
        }
    });
    // 60 Hz ceiling, and it does nothing at all when the snapshot is unchanged.
    impl_->hdrMemorySaveTimer=new QTimer(this);
    impl_->hdrMemorySaveTimer->setSingleShot(true);impl_->hdrMemorySaveTimer->setInterval(400);
    connect(impl_->hdrMemorySaveTimer,&QTimer::timeout,this,[this]{flushHdrMemory();});
    impl_->volumeSaveTimer=new QTimer(this);
    impl_->volumeSaveTimer->setSingleShot(true);impl_->volumeSaveTimer->setInterval(500);
    connect(impl_->volumeSaveTimer,&QTimer::timeout,this,[this]{
        if(impl_->savePrefs())oviora::log::info("ui-prefs",std::format("volume={:.3f} saved",impl_->prefs.value(QStringLiteral("volume")).toDouble()));
        else oviora::log::warn("ui-prefs","volume not saved");
    });
    impl_->timer = new QTimer(this);
    connect(impl_->timer, &QTimer::timeout, this, [this] {
        const bool trace = qApp && qApp->property("ovioraTraceUiStalls").toBool();
        const auto t0 = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const bool had = impl_->haveSnapshot;
        const uint64_t before = impl_->publishedRevision;
        const bool wasRunning = impl_->snapshot.running;
        const bool wasImage = impl_->snapshot.image;
        const bool wasFailed = impl_->snapshot.failed;
        const bool wasEngineHung = impl_->snapshot.status == engine::kEngineHangStatus;
        const bool wasFullscreenUnsafe = impl_->snapshot.vramFullscreenUnsafe;
        const auto wasTransport = impl_->snapshot.transport;
        const uint64_t wasSession = impl_->snapshot.sessionId;
        const auto oldSourceAspect=sourceAspect();
        if(impl_->poll()){emit settingsChanged();emitChainChanged();}
        if(wasSession!=impl_->snapshot.sessionId||oldSourceAspect!=sourceAspect())emit compareChanged();
  // Custom: a display change re-applies that device's HDR curve. The id check is
  // a few Win32 calls, so this is safe on the polling path.
  syncHdrDisplayProfile();
  syncHdrTitleProfile();
        if (impl_->snapshot.vramRunawayMiB && (impl_->vramNoticeSession != impl_->snapshot.sessionId ||
            (!wasFullscreenUnsafe && impl_->snapshot.vramFullscreenUnsafe))) {
            impl_->vramNoticeSession = impl_->snapshot.sessionId;
            if (impl_->snapshot.vramFullscreenUnsafe)
                emit notice(tr("全屏期间显存持续增长了 %1 GB，已退出全屏保护播放；当前会话使用窗口播放。增强设置保留。"
                               "原因尚未确认，可开启设置中的 OBS 游戏采集兼容、重启后测试全屏。")
                                .arg(impl_->snapshot.vramRunawayMiB / 1024.0, 0, 'f', 1), true);
            else emit notice(tr("显存在设置未变时增加了 %1 GB，正在尝试回收；回收结果见诊断日志。原因尚未确认。")
                                 .arg(impl_->snapshot.vramRunawayMiB / 1024.0, 0, 'f', 1), true);
        }
        tickSubtitles();
        // The statistics panel over the picture: about twice a second
        if (++impl_->statsTicks >= 30) { impl_->statsTicks = 0; tickStatsOverlay(); }
        else followStatsOverlay();
        tickScreenshot();
        tickPs5();
        // Geometry uses the source DAR and the same transform as mouse/compare.
        if (impl_->snapshot.running && impl_->videoWindow) {
            const bool fresh = impl_->aspectSession != impl_->snapshot.sessionId;
            impl_->aspectSession = impl_->snapshot.sessionId;
            applyAspect(fresh);
        }
        // Status light: 1.4.4 sampled its dashboard history every 250 ms.
        if (++impl_->historyTicks >= 16) {
            impl_->historyTicks = 0;
            updateRunStatus();
        }
        // The renderer reopens on its own thread (device change, fallback); pick
        // up what it opened about twice a second.
        if (++impl_->audioPollTicks >= 30) {
            impl_->audioPollTicks = 0;
            const auto active = sink::activeRenderEndpoint();
            if (active.valid != impl_->audioActive.valid || active.id != impl_->audioActive.id ||
                active.fallback != impl_->audioActive.fallback || active.channels != impl_->audioActive.channels ||
                active.downmix != impl_->audioActive.downmix) {
                const bool fellBack = active.fallback && !impl_->audioActive.fallback;
                impl_->audioActive = active;
                emit audioDevicesChanged();
                if (fellBack) emit notice(tr("所选音频设备不可用，已切到系统默认设备"), true);
            }
        }
        if (!had || impl_->publishedRevision != before) {
            const auto& s = impl_->snapshot;
            if (!had || wasSession != s.sessionId || wasTransport != s.transport ||
                wasRunning != s.running || wasImage != s.image || wasFailed != s.failed)
                oviora::log::info("qml-window", std::format(
                    "source-state session={} transport={} running={} image={} failed={} opening={} revision={}",
                    s.sessionId, int(s.transport), s.running, s.image, s.failed,
                    impl_->openingSource, impl_->publishedRevision));
            impl_->power.update(s.running && !s.failed && !s.image && s.transport == engine::TransportState::Playing);
            // A live source that fails says why, and the user is taken back to the
            // start page: the reason used to land only in the (closed) PS5 panel,
            // leaving a black cinema strip that looked like a minimised window.
            if (had && !wasFailed && s.failed && (s.capture || s.remotePlay)) {
                const QString why = oviora::ui::i18n::text(s.status);
                oviora::log::warn("ui-live-failed", std::format("remote={} status={}", s.remotePlay, utf8Of(s.status).toStdString()));
                QString text = why.isEmpty() ? (s.remotePlay ? tr("PS5 串流没有连上") : tr("采集没有开始")) : why;
                // The interface itself may be gone with the device (Qt loses it too), so
                // the hint is also left for the next start.
                const QString injected = utf8Of(gfx::riskyInjections());
                if (const QString advice = injectionAdvice(injected); !advice.isEmpty()) {
                    text += QStringLiteral("；") + advice;
                    QFile marker(lastFailurePath());
                    if (marker.open(QIODevice::WriteOnly)) marker.write((QStringLiteral("device\n") + injected + QStringLiteral("\n")).toUtf8());
                }
                emit notice(text, true);
                if (impl_->currentPage == QLatin1String("min")) emit navigate(QStringLiteral("home"));
            }
            // Engine watchdog (field logs(15)): a frozen picture says why, once.
            if (had && !wasEngineHung && s.status == engine::kEngineHangStatus) emit notice(uiText(s.status), true);
            emit snapshotChanged();
        }
        if (trace) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1.0) oviora::log::info("ui-stall-timer", std::format("pollTimer ms={:.2f} running={}", ms, impl_->snapshot.running));
        }
    });
    impl_->timer->start(16);
    startGpuSampler();
    // Overlays that hook presentation get injected into this process when a D3D device
    // appears (Qt's at start, the engine's at open); see gfx/PresentationHooks.h for what
    // they broke in the field. Log each once (every 2 s for two minutes, then every 10 s).
    auto* hookTimer = new QTimer(this);
    auto hookScanInFlight = std::make_shared<std::atomic<bool>>(false);
    connect(hookTimer, &QTimer::timeout, this, [this, hookTimer, hookScanInFlight, started = GetTickCount64()] {
        const bool trace = qApp && qApp->property("ovioraTraceUiStalls").toBool();
        const auto t0 = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        struct Seen { const wchar_t* module; const char* product; };
        static const Seen others[] = {
            {L"graphics-hook64.dll", "OBS game capture"}, {L"DiscordHook64.dll", "Discord overlay"},
            {L"gameoverlayrenderer64.dll", "Steam overlay"}, {L"nvspcap64.dll", "NVIDIA overlay / instant replay"},
            {L"ow-graphics-hook64.dll", "Overwolf overlay"},
        };
        static bool loggedOthers[std::size(others)]{};
        for (size_t k = 0; k < std::size(others); ++k)
            if (!loggedOthers[k] && GetModuleHandleW(others[k].module)) {
                loggedOthers[k] = true;
                oviora::log::info("overlay-hooks", std::format("{} is injected into this process", others[k].product));
            }
        // RivaTuner / GamePP are only logged. The XeSS guard that switched frame generation
        // away from XeSS while they were injected was removed on request (2026-10-02): the
        // interface now draws with Direct3D 12 like 1.4.4's process, which is what they
        // coexisted with.
        static bool handled = false;
        if (const auto* hook = gfx::injectedPresentationHook(); hook && !handled) {
            handled = true;
            oviora::log::warn("overlay-hooks", std::format("{} is injected into this process ({})",
                hook->product, QString::fromWCharArray(hook->module).toStdString()));
            if (GetModuleHandleW(L"RTSSHooks64.dll")) {
                wchar_t path[32768]{};
                GetModuleFileNameW(GetModuleHandleW(L"RTSSHooks64.dll"), path, DWORD(std::size(path)));
                oviora::log::info("overlay-hooks", std::format("RTSS hook module: activeServer={} path={}",
                    gfx::rivaTunerRunning(), QString::fromWCharArray(path).toStdString()));
            }
        }
        // RivaTuner arrived after a start with the interface on the GPU (it was not running
        // then, see main.cpp). Its OSD stays out of this process (RTSSHooksProfileOverride),
        // so nothing breaks; a restart moves the interface to software drawing and puts the
        // OSD on the video.
        static bool rivaTunerLate = false;
        const bool checkGpuOverlays = !qApp->property("ovioraSoftwareUi").toBool() && !qApp->property("ovioraUiRendererForced").toBool();
        if (!rivaTunerLate && checkGpuOverlays && GetModuleHandleW(L"RTSSHooks64.dll") && gfx::rivaTunerRunning()) {
            rivaTunerLate = true;
            const bool automatic = impl_->prefString("overlayCompat") != QLatin1String("off");
            oviora::log::warn("overlay-hooks", std::format("RivaTuner injected into a run with a GPU interface; its OSD is off in this process ({})",
                automatic ? "active OSD server: compatibility restart suggested" : "monitoring compatibility off"));
            if (automatic) emit overlayRestartSuggested(QStringLiteral("rtss"));
        }
        static bool nvidiaOverlayLate = false;
        static bool gamePPLate = false;
        if (checkGpuOverlays && !gamePPLate
                && !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_GAMEPP", nullptr, 0) && gfx::gamePPInjected()) {
            gamePPLate = true;
            const bool automatic = impl_->prefString("overlayCompat") != QLatin1String("off");
            oviora::log::warn("overlay-hooks", std::format("GamePP is running or injected in a run with a GPU interface ({})",
                automatic ? "compatibility restart suggested" : "monitoring compatibility off"));
            if (automatic) emit overlayRestartSuggested(QStringLiteral("gamepp"));
        }
        const bool overlayAuto = impl_->prefString("overlayCompat") != QLatin1String("off");
        if (!overlayAuto && impl_->liveCompanionCap)
            applyLiveCompanionCompat(false);
        const bool checkNv = checkGpuOverlays && !nvidiaOverlayLate && !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_NVIDIA_OVERLAY", nullptr, 0);
        const bool checkGp = checkGpuOverlays && !gamePPLate && !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_GAMEPP", nullptr, 0);
        const bool checkDouyin = overlayAuto && !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_DOUYIN", nullptr, 0);
        // Toolhelp process snapshots take ~5-8 ms each; run a single combined snapshot off the GUI thread.
        if ((checkNv || checkGp || checkDouyin) && !hookScanInFlight->exchange(true)) {
            QPointer<QmlPlayerBridge> self(this);
            QThreadPool::globalInstance()->start([self, hookScanInFlight, checkNv, checkGp, checkDouyin]() {
                const auto scan = gfx::scanProcessHooks();
                if (QCoreApplication::instance()) {
                    QMetaObject::invokeMethod(QCoreApplication::instance(), [self, hookScanInFlight, scan, checkNv, checkGp, checkDouyin]() {
                        hookScanInFlight->store(false);
                        if (!self) return;
                        if (checkNv && !nvidiaOverlayLate && scan.nvidiaOverlay) {
                            nvidiaOverlayLate = true;
                            const bool automatic = self->impl_->prefString("overlayCompat") != QLatin1String("off");
                            oviora::log::warn("overlay-hooks", std::format("NVIDIA overlay is on in a run with a GPU interface ({})",
                                automatic ? "compatibility restart suggested" : "monitoring compatibility off"));
                            if (automatic) emit self->overlayRestartSuggested(QStringLiteral("nvidia"));
                        }
                        if (checkGp && !gamePPLate && scan.gamePP) {
                            gamePPLate = true;
                            const bool automatic = self->impl_->prefString("overlayCompat") != QLatin1String("off");
                            oviora::log::warn("overlay-hooks", std::format("GamePP is running or injected in a run with a GPU interface ({})",
                                automatic ? "compatibility restart suggested" : "monitoring compatibility off"));
                            if (automatic) emit self->overlayRestartSuggested(QStringLiteral("gamepp"));
                        }
                        if (checkDouyin) {
                            const bool automatic = self->impl_->prefString("overlayCompat") != QLatin1String("off");
                            self->applyLiveCompanionCompat(automatic && scan.douyinCompanion);
                        }
                    }, Qt::QueuedConnection);
                } else {
                    hookScanInFlight->store(false);
                }
            });
        }
        // RivaTuner's default hooking keeps OBS game capture from hooking any Direct3D 12
        // program (2026-10-03, RTSS 7.3.5.28314 + OBS 32.1.2, a bare D3D12 window included;
        // RTSS version corrected against the executable on 2026-10-04); its
        // "Use Microsoft Detours API hooking" option lets both work. Said once, ever.
        static bool obsWithRivaTuner = false;
        if (!obsWithRivaTuner && GetModuleHandleW(L"graphics-hook64.dll") && GetModuleHandleW(L"RTSSHooks64.dll")) {
            obsWithRivaTuner = true;
            oviora::log::warn("overlay-hooks", "OBS game capture and RivaTuner are both injected: OBS may not get the picture unless RTSS uses Detours hooking for Oviora");
            if (!impl_->prefs.value(QStringLiteral("obsRivaTunerHintShown")).toBool()) {
                impl_->prefs[QStringLiteral("obsRivaTunerHintShown")] = true;
                (void)impl_->savePrefs();
                emit notice(tr("检测到 OBS 游戏采集和小飞机（RTSS）同时注入 Oviora。两者默认的挂钩方式冲突，OBS 游戏采集可能抓不到画面（任何 D3D12 程序都一样）。"
                               "在 RTSS 里添加 Oviora.exe，并在它的设置中勾选“Use Microsoft Detours API hooking”；或在 OBS 改用窗口采集（Windows 10 1903+）"), false);
            }
        }
        // Overlays inject when they decide the window is a game, e.g. on entering fullscreen
        // long after start; after the first two minutes look every 10 s instead of every 2 s.
        if (GetTickCount64() - started > 120000 && hookTimer->interval() < 10000) hookTimer->setInterval(10000);
        if (trace) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms >= 1.0) oviora::log::info("ui-stall-timer", std::format("hookTimer ms={:.2f}", ms));
        }
    });
    hookTimer->start(2000);
    // The previous run crashed or lost the GPU with a known-risky component injected:
    // say so once the window is up (the crash handler cannot show anything itself).
    QTimer::singleShot(2500, this, [this] {
        QFile marker(lastFailurePath());
        if (!marker.exists()) return;
        QByteArray raw;
        if (marker.open(QIODevice::ReadOnly)) raw = marker.readAll();
        marker.close();
        marker.remove();
        // The crash handler writes UTF-16 (no allocation in the filter); the bridge UTF-8.
        const QString text = raw.size() > 1 && raw.at(1) == 0
            ? QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()), raw.size() / 2) : QString::fromUtf8(raw);
        const QStringList lines = text.split(QLatin1Char('\n'));
        const QString kind = lines.value(0), modules = lines.value(1);
        QString advice = injectionAdvice(kind + QLatin1Char(',') + modules);
        oviora::log::info("overlay-hooks", std::format("previous run: {} injected={}", kind.toStdString(), modules.toStdString()));
        if (advice.isEmpty()) return;
        const QString head = kind.startsWith(QStringLiteral("crash"))
            ? tr("上次 Oviora 异常退出（崩溃在 %1）").arg(kind.mid(6).trimmed())
            : tr("上次采集时显卡设备丢失");
        emit notice(head + QStringLiteral("；") + advice, true);
    });
    // Unsupported saved FG choices were repaired together with both modes
    // above, using actual provider availability rather than a brand alone.
}

QmlPlayerBridge::~QmlPlayerBridge() {
    flushHdrMemory();
    if (qApp) qApp->removeEventFilter(this);
    if (qApp) qApp->removeNativeEventFilter(&impl_->displayChangeFilter);
    if (qApp) qApp->removeNativeEventFilter(&impl_->powerFilter);
    { const QSignalBlocker quiet(this); mouseAimRelease("closing"); }
    if (qApp && impl_->rawMouseFilter.received) qApp->removeNativeEventFilter(&impl_->rawMouseFilter);
    // flushPreferences() writes every preference, a pending volume included.
    if (impl_->volumeSaveTimer) impl_->volumeSaveTimer->stop();
    flushPreferences();
    if (impl_->sessionSaveTimer) impl_->sessionSaveTimer->stop();
    if (!impl_->persistSession())
        oviora::log::error("ui-session", utf8Of(impl_->facade.error()).toStdString());
}



} // namespace oviora::ui
