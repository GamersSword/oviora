// QML bridge: Preferences responsibilities.
#include "../QmlPlayerBridge_p.h"
#include "oviora/ngx/AnrProvider.h"

namespace oviora::ui {
// --- shell preferences -------------------------------------------------------
bool QmlPlayerBridge::reducedMotion() const { return impl_->facade.reducedMotion(); }
int QmlPlayerBridge::uiDeviceGeneration() const { return impl_->uiDeviceGeneration; }
void QmlPlayerBridge::noteUiDeviceRecreated(const QString& window) {
    ++impl_->uiDeviceGeneration;
    oviora::log::warn("ui-device", std::format("{} scene graph recreated after a lost graphics device; icons rebuilt generation={}",
                                              window.toStdString(), impl_->uiDeviceGeneration));
    emit uiDeviceGenerationChanged();
}
void QmlPlayerBridge::setReducedMotion(bool value) {
    if (impl_->facade.reducedMotion() == value) return;
    impl_->facade.setReducedMotion(value);
    emit settingsChanged();
}
QString QmlPlayerBridge::defaultPage() const { return utf8Of(impl_->facade.defaultPage()); }
void QmlPlayerBridge::setDefaultPage(const QString& value) {
    if (utf8Of(impl_->facade.defaultPage()) == value) return;
    impl_->facade.setDefaultPage(wideOf(value));
    emit settingsChanged();
}
QString QmlPlayerBridge::defaultPageLabel() const {
    const QString page = defaultPage();
    if (page == QLatin1String("min")) return tr("极简模式");
    if (page == QLatin1String("pro")) return tr("专业模式");
    if (page == QLatin1String("last")) return tr("上次");
    return tr("首页");
}

QString QmlPlayerBridge::initialPage() const {
    // The saved preference unless a test override was set; "last" reopens the
    // page the previous session ended on.
    if (!impl_->initialPageOverride.isEmpty()) return impl_->initialPageOverride;
    if (defaultPage() == QLatin1String("last")) {
        const QString last = impl_->prefString("lastPage");
        return last.isEmpty() ? QStringLiteral("home") : last;
    }
    return defaultPage();
}
void QmlPlayerBridge::setInitialPage(const QString& value) {
    impl_->initialPageOverride = value;
    emit settingsChanged();
}

QString QmlPlayerBridge::currentPage() const { return impl_->currentPage; }
void QmlPlayerBridge::setCurrentPage(const QString& value) {
    impl_->currentPage = value;
    static const QStringList remembered{"home", "min", "pro", "node", "set", "exp"};
    if (remembered.contains(value) && impl_->prefString("lastPage") != value) {
        impl_->prefs["lastPage"] = value == QLatin1String("node") ? QStringLiteral("pro") : value;
        impl_->savePrefs();
    }
}

QString QmlPlayerBridge::remotePlayState() const {
    // The engine's own remote-play state word, or empty when there is none. No
    // invented "connected" wording.
    return uiText(impl_->snapshot.remoteRecoveryMessage);
}

QVariantList QmlPlayerBridge::componentList() const {
    if (!impl_->componentListRead) { impl_->componentList = readComponentList(); impl_->componentListRead = true; }
    return impl_->componentList;
}
void QmlPlayerBridge::refreshComponentList() {
    auto list = readComponentList();
    impl_->componentListRead = true;
    if (list == impl_->componentList) return;
    impl_->componentList = std::move(list);
    emit componentListChanged();
}
QVariantList QmlPlayerBridge::readComponentList() const {
    // Read from the runtime manifest the build produces rather than a list written
    // by hand here, so the page cannot claim a component the package lacks.
    QVariantList out;
    const auto anrDirectory=ngx::AnrProvider::directory(runtime::localRuntimeDirectory().wstring());
    out<<QVariantMap{{"name",QStringLiteral("ANR FG")},{"detail",QString::fromStdWString(anrDirectory)},
        {"loaded",ngx::AnrProvider::available(runtime::localRuntimeDirectory().wstring())},{"experimental",true}};
    const auto vfgDirectory=pipeline::VfgBackend::runtimeDirectory(runtime::localRuntimeDirectory().wstring());
    const bool vfgAvailable=pipeline::VfgBackend::runtimeAvailable(runtime::localRuntimeDirectory().wstring());
    out<<QVariantMap{{"name",QStringLiteral("NVIDIA VFG")},{"detail",QString::fromStdWString(vfgDirectory)},
        {"loaded",vfgAvailable},{"experimental",true}};
    const auto manifest = runtime::localRuntimeDirectory() / L"release-runtime-manifest.json";
    QFile file(QString::fromWCharArray(manifest.c_str()));
    if (!file.open(QIODevice::ReadOnly)) {
        // No manifest is a reportable fact, not a reason to invent entries.
        return out;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto entries = document.isArray() ? document.array()
                                            : document.object().value(QStringLiteral("files")).toArray();
    for (const auto& value : entries) {
        const auto object = value.toObject();
        QVariantMap item;
        item["name"] = object.value(QStringLiteral("name")).toString();
        // Release manifests write fileVersion/authenticode; keep the short keys too.
        auto field = [&](const char* primary, const char* fallback) {
            const QString text = object.value(QLatin1String(primary)).toString();
            return text.isEmpty() ? object.value(QLatin1String(fallback)).toString() : text;
        };
        const QString version = field("version", "fileVersion");
        const QString signature = field("signature", "authenticode");
        const bool experimental = object.value(QStringLiteral("experimental")).toBool();
        // Several runtimes share a file name (the NR slots); the folder tells them apart.
        const QString path = object.value(QStringLiteral("path")).toString();
        const QString folder = path.section(QLatin1Char('/'), 0, -2);
        QStringList detail;
        for (const QString& part : {version, signature, folder})
            if (!part.isEmpty()) detail << part;
        item["detail"] = detail.join(QStringLiteral(" · "));
        item["loaded"] = object.value(QStringLiteral("loaded")).toBool(true);
        item["experimental"] = experimental;
        if (!item["name"].toString().isEmpty()) out << item;
    }
    return out;
}

void QmlPlayerBridge::openProjectPage() {
    // 关于 → 交流群: the group QR codes are kept current in the project README.
    if (!QDesktopServices::openUrl(QUrl(QStringLiteral("https://oviora.tr"))))
        emit notice(tr("无法打开浏览器"), true);
}

void QmlPlayerBridge::copyDiagnostics() {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(diagnosticsReport());
        emit notice(tr("诊断信息已复制"), false);
    }
}


// --- shell preferences (P4-f / P4-c / P4-d) --------------------------------
QVariantMap QmlPlayerBridge::preferences() const { return impl_->prefs; }
QString QmlPlayerBridge::gpuPriorityStatus() const {
    const auto status=gfx::gpuPriorityStatus();
    const auto name=[](int priority){return priority==5?tr("实时"):priority==4?tr("高"):tr("普通");};
    if(status.state==gfx::GpuPriorityState::Pending)return tr("创建显卡会话后生效");
    if(status.state==gfx::GpuPriorityState::Unavailable)return tr("系统不支持调整 GPU 优先级");
    if(status.state==gfx::GpuPriorityState::Rejected)return tr("系统未接受设置，当前：%1").arg(status.actual<0?tr("未知"):name(status.actual));
    if(status.capped)return tr("已生效：%1（抖音直播伴侣运行中，暂不使用实时）").arg(name(status.actual));
    return tr("已生效：%1").arg(name(status.actual));
}
void QmlPlayerBridge::rememberWindowSize(int width, int height) {
    // Only kept when the user chose "记住上次"; the key is internal.
    if (width < 400 || height < 200 || width > 16384 || height > 16384) return;
    const QString v = QStringLiteral("%1x%2").arg(width).arg(height);
    if (impl_->prefs.value(QStringLiteral("lastWindow")).toString() == v) return;
    impl_->prefs[QStringLiteral("lastWindow")] = v;
    if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "last window size not saved");
}
QRect QmlPlayerBridge::screenAvailableAt(int x, int y) const {
    QScreen* screen = QGuiApplication::screenAt(QPoint(x, y));
    if (!screen) screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1040);
}
QRect QmlPlayerBridge::launchScreenAvailable() const {
    const QPoint at = QCursor::pos();
    return screenAvailableAt(at.x(), at.y());
}

bool QmlPlayerBridge::setPreference(const QString& key, const QVariant& value) {
    static const QHash<QString, std::pair<int, int>> ranges{
        {"subtitleSize", {16, 56}}, {"subtitleFont", {0, 5}}, {"subtitleOutline", {0, 3}},
        {"subtitleMargin", {0, 240}}, {"subtitleLines", {0, 8}}};
    // Game input (bridge/GameInput.cpp): Genel → Yayın menüsü, Kumandalar, Tuşlar.
    static const QHash<QString, std::pair<int, int>> inputRanges{{"rumbleStrength", {0, 5}}, {"hapticStrength", {0, 200}}};
    static const QStringList inputFlags{"padMenuCombo", "keyboardMouse", "keyboardPlay", "mouseTouch", "padBackground"};
    // The PS5 stream (bridge/StreamSession.cpp, Main.qml): Genel -> Oyunu birakinca, Gorunti ->
    // Pencere, Ses ve ag -> Mikrofon and Baglanti, Konsollar -> Otomatik baglan.
    static const QHash<QString, QStringList> streamChoices{{"disconnectAction", {"ask", "sleep", "nothing"}},
                                                          {"suspendAction", {"sleep", "nothing"}},
                                                          {"streamWindow", {"normal", "maximized", "fullscreen"}}};
    static const QHash<QString, std::pair<int, int>> streamRanges{{"weakLinkWarning", {0, 100}}, {"hideCursorSeconds", {1, 10}}};
    static const QStringList streamFlags{"showStreamStats", "micStartOn", "fullscreenDoubleClick", "hideCursor"};
    static const QStringList flags{"subtitleEnabled", "subtitleBackground", "subtitleFit",
                                   "subtitleSecondLanguage", "audioForceStereo", "holdCompare",
                                   "cinePillHidden", "obsGameCapture", "fullscreenMemoryProtection", "hdrFgCompatibility", "computeOnMonitorGpu",
                                   "streamerMode"};
    QVariant stored;
    if (streamChoices.contains(key)) {
        const QString v = value.toString();
        if (!streamChoices.value(key).contains(v)) return false;
        stored = v;
    } else if (streamRanges.contains(key)) {
        bool ok = false; const int n = value.toInt(&ok);
        const auto [low, high] = streamRanges.value(key);
        if (!ok || n < low || n > high) return false;
        stored = n;
    } else if (streamFlags.contains(key)) {
        stored = value.toBool();
    } else if (key == QLatin1String("micDevice")) {
        // A recording device's name as SDL lists it (Microphone::devices()); "" = Windows' default.
        const QString v = value.toString();
        if (v.size() > 256) return false;
        stored = v;
    } else if (key == QLatin1String("autoConnect")) {
        // A paired PS5's profile id (lanConsoles key), or "" for none.
        const QString v = value.toString();
        if (!v.isEmpty() && std::none_of(impl_->ps5ProfileList.begin(), impl_->ps5ProfileList.end(),
                                         [&](const QVariant& p) { return p.toMap().value("id").toString() == v; })) return false;
        stored = v;
    } else if (inputRanges.contains(key)) {
        bool ok = false; const int n = value.toInt(&ok);
        const auto [low, high] = inputRanges.value(key);
        if (!ok || n < low || n > high) return false;
        stored = n;
    } else if (inputFlags.contains(key)) {
        stored = value.toBool();
    } else if (key == QLatin1String("padMenuButtons")) {
        // Four slots, 0 = not used, 1..16 = Cross … PS (old streamMenuShortcut1..4).
        const auto list = value.toList();
        if (list.size() != 4) return false;
        QVariantList out;
        for (const auto& v : list) { bool ok = false; const int n = v.toInt(&ok); if (!ok || n < 0 || n > 16) return false; out << n; }
        stored = out;
    } else if (key == QLatin1String("padButtonMap")) {
        // PS5 button (0..15) -> the pad button (0..15) that presses it; only changed ones.
        const auto map = value.toMap();
        QVariantMap out;
        for (auto it = map.begin(); it != map.end(); ++it) {
            bool okKey = false, okValue = false; const int b = it.key().toInt(&okKey); const int src = it.value().toInt(&okValue);
            if (!okKey || !okValue || b < 0 || b > 15 || src < 0 || src > 15) return false;
            if (b != src) out[QString::number(b)] = src;
        }
        stored = out;
    } else if (key == QLatin1String("mouseAim")) {
        // Tuşlar → Fareyle nişan alma (old mouse_aim/*): every field checked, unknown ones refused.
        static const QSet<int> toggleKeys{Qt::Key_F6, Qt::Key_F7, Qt::Key_F8, Qt::Key_F9, Qt::Key_F10, Qt::Key_ScrollLock, Qt::Key_Pause};
        const auto map = value.toMap();
        QVariantMap out;
        for (auto it = map.begin(); it != map.end(); ++it) {
            const QString& k = it.key();
            bool ok = true; int n = 0;
            if (k == QLatin1String("enabled") || k == QLatin1String("invert_y")) { out[k] = it.value().toBool(); continue; }
            n = it.value().toInt(&ok);
            if (!ok) return false;
            if (k == QLatin1String("sensitivity")) { if (n < 1 || n > 100) return false; }
            else if (k == QLatin1String("accel")) { if (n < 0 || n > 100) return false; }
            else if (k == QLatin1String("left") || k == QLatin1String("right") || k == QLatin1String("middle")) { if (n < 0 || n > 10) return false; }
            else if (k == QLatin1String("toggle_key")) { if (!toggleKeys.contains(n)) return false; }
            else return false;
            out[k] = n;
        }
        stored = out;
    } else if (key == QLatin1String("keyMap")) {
        // Old button value (1 << 0 … 1 << 25) -> Qt key; only the keys changed from the defaults.
        const auto map = value.toMap();
        QVariantMap out;
        for (auto it = map.begin(); it != map.end(); ++it) {
            bool okKey = false, okValue = false; const qulonglong b = it.key().toULongLong(&okKey); const int code = it.value().toInt(&okValue);
            if (!okKey || !okValue || b == 0 || b > (1ull << 25) || (b & (b - 1)) || code <= 0 || code == Qt::Key_unknown) return false;
            out[it.key()] = code;
        }
        stored = out;
    } else if (ranges.contains(key)) {
        bool ok = false; const int n = value.toInt(&ok);
        const auto [low, high] = ranges.value(key);
        if (!ok || n < low || n > high) return false;
        stored = n;
    } else if (flags.contains(key)) {
        stored = value.toBool();
    } else if (key == QLatin1String("nrResidualView") || key == QLatin1String("nrCorrectionView")) {
        const auto view = value.toString();
        if (view != QLatin1String("web") && view != QLatin1String("list")) return false;
        stored = view;
    } else if (key == QLatin1String("screenshotDir")) {
        const QString dir = value.toString();
        if (!dir.isEmpty() && !QFileInfo(dir).isDir()) { emit notice(tr("截图目录不存在：%1").arg(dir), true); return false; }
        stored = dir;
    } else if (key == QLatin1String("audioDevice")) {
        stored = value.toString();
    } else if (key == QLatin1String("sliderKeyStep")) {
        // Arrow-key step of a focused slider (设置 → 通用): 1, 0.1 or 0.01.
        bool ok = false; const double v = value.toDouble(&ok);
        if (!ok || !(qFuzzyCompare(v, 1.0) || qFuzzyCompare(v, 0.1) || qFuzzyCompare(v, 0.01))) return false;
        stored = v;
    } else if (key == QLatin1String("accent")) {
        const QString v = value.toString();
        if (v != QLatin1String("orange") && v != QLatin1String("white")) return false;
        stored = v;
    } else if (key == QLatin1String("backdrop")) {
        bool ok = false; const int n = value.toInt(&ok);
        if (!ok || n < 0 || n > 2) return false;
        stored = n;
    } else if (key == QLatin1String("uiScale")) {
        bool ok = false; const int n = value.toInt(&ok);
        if (!ok || (n != 0 && n != 100 && n != 125 && n != 150)) return false;
        stored = n;
    } else if (key == QLatin1String("overlayCompat")) {
        // 设置 → 监控软件兼容; read by main.cpp before Qt starts.
        const QString v = value.toString();
        if (v != QLatin1String("auto") && v != QLatin1String("off")) return false;
        stored = v;
    } else if (key == QLatin1String("uiRenderer")) {
        // 设置 → 界面绘制 (oviora/ui/InterfaceRenderer.h); read by main.cpp before Qt starts.
        const QString v = value.toString();
        if (v != QLatin1String("software") && v != QLatin1String("gpu")) return false;
        stored = v;
    } else if (key == QLatin1String("gpuPriority")) {
        const QString v=value.toString();
        if(v!=QLatin1String("normal")&&v!=QLatin1String("high")&&v!=QLatin1String("realtime"))return false;
        stored=v;
    } else if (key == QLatin1String("dockPinned")) {
        stored = value.toBool();
    } else if (key == QLatin1String("language")) {
        const QString v = value.toString();
        static const QStringList languages{"auto", "tr", "en", "de"};
        if (!languages.contains(v)) return false;
        stored = v;
    } else if (key == QLatin1String("monitorGpu")) {
        const QString v = value.toString();
        if (!v.isEmpty() && std::none_of(impl_->gpuAdapters.begin(), impl_->gpuAdapters.end(), [&](const auto& a) { return a.id == v; })) return false;
        stored = v;
    } else if (key == QLatin1String("windowSize")) {
        const QString v = value.toString();
        // Oviora: 16:9 like the PS5 picture (the 16:10 sizes left bars above and below)
        static const QStringList sizes{"1280x720", "1600x900", "1920x1080", "last"};
        if (!sizes.contains(v)) return false;
        stored = v;
    } else {
        return false;
    }
    if (impl_->prefs.value(key) == stored) return true;
    const auto previous = impl_->prefs;
    impl_->prefs[key] = stored;
    if (!impl_->savePrefs()) {
        impl_->prefs = previous;
        emit notice(tr("偏好保存失败，已保留原设置"), true);
        return false;
    }
    oviora::log::info("ui-prefs", std::format("{}={}", key.toStdString(), stored.toString().toStdString()));
    applyPreference(key);
    emit preferencesChanged();
    return true;
}
void QmlPlayerBridge::applyPreference(const QString& key) {
    static const QStringList gameInputKeys{"padMenuCombo", "padMenuButtons", "padButtonMap", "rumbleStrength", "hapticStrength",
                                           "keyboardMouse", "keyboardPlay", "keyMap", "mouseAim", "mouseTouch"};
    if (gameInputKeys.contains(key)) loadGameInput();
    if (key == QLatin1String("autoConnect")) emit lanChanged();
    if(key==QLatin1String("hdrFgCompatibility")){
        impl_->presentation.hdrFgCompatibility=impl_->prefBool("hdrFgCompatibility",false);
        impl_->engine.requestPresentation(impl_->presentation);
        emit presentationChanged();
    }
    if(key==QLatin1String("gpuPriority")){
        const auto value=impl_->prefString("gpuPriority");
        // Unset means High (user default, 2026-11-13); saved choices stay explicit.
        gfx::requestGpuPriority(value==QLatin1String("normal")?gfx::GpuPriority::Normal:
                                value==QLatin1String("realtime")?gfx::GpuPriority::Realtime:gfx::GpuPriority::High);
    }
    if(key==QLatin1String("overlayCompat"))updateLiveCompanionCompat();
    if(key==QLatin1String("fullscreenMemoryProtection")){impl_->engine.setFullscreenMemoryProtection(impl_->prefBool("fullscreenMemoryProtection",false));emit snapshotChanged();}
    if (key == QLatin1String("audioDevice")) sink::setPreferredRenderEndpoint(impl_->prefString("audioDevice").toStdWString());
    else if (key == QLatin1String("audioForceStereo")) sink::setForceStereoDownmix(impl_->prefBool("audioForceStereo", false));
    else if (key == QLatin1String("language")) {
        const QString code = i18n::resolve(impl_->prefString("language"));
        if (!i18n::apply(code)) emit notice(tr("这个语言的翻译没有找到，界面保持简体中文"), true);
        if (impl_->qmlEngine) {
            impl_->qmlEngine->setUiLanguage(i18n::current());
            impl_->qmlEngine->retranslate();
            // Strings the bridge builds itself (tr() in getters) follow once their
            // properties notify: every argument-free notify signal fires once.
            const QMetaObject* meta = metaObject();
            QSet<int> fired;
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                const QMetaMethod notify = meta->property(i).notifySignal();
                if (notify.isValid() && notify.parameterCount() == 0 && !fired.contains(notify.methodIndex())) {
                    fired.insert(notify.methodIndex());
                    notify.invoke(this, Qt::DirectConnection);
                }
            }
        }
    }
    else if (key == QLatin1String("monitorGpu") || key == QLatin1String("computeOnMonitorGpu")) {
        if (key == QLatin1String("monitorGpu")) { impl_->resolveGpuMonitor(); impl_->gpuUtil = -1.0; emit perfChanged(); }
        const auto before = impl_->computeLuid;
        impl_->applyComputeGpu();
        if (impl_->computeLuid != before) {
            // Effect availability follows the compute GPU (NVIDIA-only effects on another card).
            (void)currentEffectCapabilities(true);
            // The chain is checked against the new card the way a restart checks it:
            // DLSS moves to an available backend, NVOF to FidelityFX flow, and stages
            // the card cannot run are switched off with their parameters kept.
            auto candidate = impl_->currentSession();
            const auto previous = candidate;
            // Switching back gives the list mode its frame generation again (DLSS 4X
            // moved to FSR 2X by the last switch returns as DLSS 4X) once the card runs it.
            const auto caps = currentEffectCapabilities();
            const auto runs = [&](const QString& id) { return caps.value(id).toMap().value("available").toBool(); };
            auto& active = candidate.configurations[size_t(candidate.active)];
            const bool listMode = !active.editor;
            const auto memory = impl_->prefs.value(QStringLiteral("computeFgMemory")).toMap();
            bool restored = false;
            if (listMode && !memory.isEmpty()) {
                const auto backend = engine::FrameGenerationBackend(memory.value(QStringLiteral("backend")).toInt());
                const auto flow = engine::OpticalFlowBackend(memory.value(QStringLiteral("flow")).toInt());
                if (runs(fgEffectId(backend))) {
                    active.fgBackend = backend;
                    active.chain.fgMultiplier = memory.value(QStringLiteral("multiplier")).toUInt();
                    if (flow != engine::OpticalFlowBackend::Nvidia || runs(QStringLiteral("flow0"))) active.opticalFlowBackend = flow;
                    impl_->prefs.remove(QStringLiteral("computeFgMemory"));
                    restored = true;
                    oviora::log::info("effect-availability", std::format("compute gpu switch restored backend={} multiplier={}",
                        engine::frameGenerationBackendName(backend), active.chain.fgMultiplier));
                }
            }
            QStringList disabled;
            for (auto& configuration : candidate.configurations) {
                const auto reason = disableUnavailableEffects(configuration);
                if (!reason.isEmpty()) disabled << reason;
            }
            const auto& before = previous.configurations[size_t(previous.active)];
            if (listMode && !restored && (before.fgBackend != active.fgBackend || before.opticalFlowBackend != active.opticalFlowBackend))
                impl_->prefs[QStringLiteral("computeFgMemory")] = QVariantMap{{QStringLiteral("backend"), int(before.fgBackend)},
                    {QStringLiteral("multiplier"), int(before.chain.fgMultiplier)}, {QStringLiteral("flow"), int(before.opticalFlowBackend)}};
            if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "compute gpu frame generation memory not saved");
            if (candidate != previous) {
                auto next = impl_->facade.pendingSettings();
                const auto& configuration = candidate.configurations[size_t(candidate.active)];
                configuration.apply(next);
                if (next.validate().empty() && impl_->facade.applySettings(next, engine::runtimeOrder(configuration.chain))) {
                    impl_->session = candidate;
                    impl_->restoreConfiguration(configuration);
                    impl_->revalidate();
                    impl_->options = engine::PlayerOptions::from(next, engine::runtimeOrder(impl_->activeRuntime()));
                    if (!impl_->persistSession()) oviora::log::warn("effect-availability", "compute gpu refit not saved");
                    emitChainChanged(); emit settingsChanged();
                } else oviora::log::warn("effect-availability", "compute gpu refit not accepted; chain unchanged");
                if (!disabled.isEmpty()) {
                    const auto message = tr("已关闭当前硬件或包不支持的效果，参数仍保留：") + disabled.join("; ");
                    oviora::log::warn("effect-availability", message.toStdString());
                    emit notice(message, false);
                }
            }
            emit effectCapabilitiesChanged(); emit fgChoicesChanged();
            // The low queue follows the new card's own control: the NVIDIA states
            // there, the plain switch elsewhere (the NVIDIA choice is kept).
            if (engine::syncNvidiaLowLatencyQueue(impl_->presentation, nvidiaLowLatencyAvailable())) applyPresentation();
            if (impl_->snapshot.running)
                emit notice(impl_->computeLuid ? tr("运算显卡已切换为「%1」，重新打开视频后生效").arg(impl_->computeGpuName)
                                               : tr("运算显卡已恢复默认（性能最强的显卡），重新打开视频后生效"), false);
        }
    }
}
QVariantList QmlPlayerBridge::gpuMonitorChoices() const {
    QVariantList out;
    out << QVariantMap{{"id", QString()}, {"label", tr("自动（独立显卡）")}};
    for (const auto& a : impl_->gpuAdapters) out << QVariantMap{{"id", a.id}, {"label", a.name}};
    return out;
}
QString QmlPlayerBridge::gpuMonitorName() const { return impl_->gpuMonitorName; }
QString QmlPlayerBridge::computeGpuHint() const {
    if (!impl_->prefBool("computeOnMonitorGpu", false)) return tr("关闭时增强、补帧和导出用性能最强的显卡");
    if (impl_->computeLuid) return tr("增强、补帧和导出都在「%1」上运行；切换后重新打开视频生效").arg(impl_->computeGpuName);
    return tr("上面选“自动”时仍用性能最强的显卡；选一块显卡后它也负责运算");
}
QString QmlPlayerBridge::uiLanguage() const { return i18n::current(); }
void QmlPlayerBridge::setQmlEngine(QQmlEngine* engine) {
    impl_->qmlEngine = engine;
    if (engine) engine->setUiLanguage(i18n::current());
}
QVariantList QmlPlayerBridge::audioDevices() const {
    QVariantList out;
    out << QVariantMap{{"id", QString()}, {"label", tr("跟随系统默认")}, {"isDefault", false}};
    const QString chosen = impl_->prefString("audioDevice");
    bool listed = chosen.isEmpty();
    for (const auto& e : impl_->audioEndpoints) {
        const QString id = QString::fromStdWString(e.id);
        listed = listed || id == chosen;
        out << QVariantMap{{"id", id}, {"label", QString::fromStdWString(e.name) + (e.isDefault ? tr(" · 当前默认") : QString())},
                           {"isDefault", e.isDefault}};
    }
    // A saved device that is unplugged stays visible, marked, instead of vanishing.
    if (!listed) out << QVariantMap{{"id", chosen}, {"label", tr("已保存的设备（未连接）")}, {"isDefault", false}, {"missing", true}};
    return out;
}
QString QmlPlayerBridge::audioOutputStatus() const {
    const auto& a = impl_->audioActive;
    if (!a.valid) return tr("尚未打开音频输出");
    QString text = QString::fromStdWString(a.name.empty() ? L"(未命名设备)" : a.name) + tr(" · %1 声道").arg(a.channels);
    if (a.downmix) text += tr(" · 下混");
    if (a.fallback) text = tr("所选设备不可用，已回落到系统默认：") + text;
    return text;
}
bool QmlPlayerBridge::audioOutputFallback() const { return impl_->audioActive.valid && impl_->audioActive.fallback; }
void QmlPlayerBridge::refreshAudioDevices() {
    impl_->audioEndpoints = sink::enumerateRenderEndpoints();
    impl_->audioActive = sink::activeRenderEndpoint();
    impl_->micDevicesRead = false;
    oviora::log::info("ui-audio", std::format("render endpoints={}", impl_->audioEndpoints.size()));
    emit audioDevicesChanged();
}
// --- shortcuts (P4-f) --------------------------------------------------------
namespace {
const QVariantMap& shortcutDefaults() {
    // Screenshot was Ctrl+S before 2026-10-27; saved custom keys are kept.
    static const QVariantMap d{{"playPause", "Space"}, {"fullscreen", "F"}, {"lock", "Ctrl+L"},
                               {"hold", "V"}, {"screenshot", "F7"}, {"toggleMode", "Tab"},
                               {"maskToggle", "Ctrl+M"}};  // Oviora: masks on/off at once (Ctrl: never sent to the PS5)
    return d;
}
}
QVariantMap QmlPlayerBridge::shortcuts() const {
    QVariantMap out = shortcutDefaults();
    const auto custom = impl_->prefs.value("shortcuts").toMap();
    QStringList taken;
    for (auto it = custom.begin(); it != custom.end(); ++it)
        if (out.contains(it.key())) {
            out[it.key()] = it.value();
            taken << it.value().toString();
        }
    // A default added later (F7/F8) may be a key the user already gave another
    // action: that action keeps it and the new one stays unbound until set.
    for (auto it = out.begin(); it != out.end(); ++it)
        if (!custom.contains(it.key()) && taken.contains(it.value().toString())) it.value() = QString();
    return out;
}
void QmlPlayerBridge::logShortcuts() {
    const auto keys = shortcuts();
    // At start and after every change: a field log then says which key each
    // action answers to.
    const auto custom = impl_->prefs.value("shortcuts").toMap();
    std::string line, changed;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        line += std::format("{}{}={}", line.empty() ? "" : " ", it.key().toStdString(),
                            it.value().toString().isEmpty() ? std::string("none") : it.value().toString().toStdString());
        if (custom.contains(it.key())) changed += (changed.empty() ? "" : ",") + it.key().toStdString();
    }
    oviora::log::info("ui-shortcuts", std::format("{} custom={}", line, changed.empty() ? std::string("none") : changed));
}
bool QmlPlayerBridge::setShortcut(const QString& action, const QString& sequence) {
    if (!shortcutDefaults().contains(action)) return false;
    const QKeySequence seq(sequence, QKeySequence::PortableText);
    if (seq.isEmpty() || seq.count() != 1) { emit notice(tr("无效的按键"), true); return false; }
    const QString text = seq.toString(QKeySequence::PortableText);
    // Keys the shell already owns cannot be taken over.
    static const QStringList reserved{"Esc", "F11", "Left", "Right", "Up", "Down", "Ctrl+O", "Ctrl+E",
                                      "B", "Z", "X", "T", "Y", "Shift+Z", "Shift+X", "Alt+Return"};
    if (reserved.contains(text)) { emit notice(tr("%1 已被固定功能占用").arg(text), true); return false; }
    const auto current = shortcuts();
    for (auto it = current.begin(); it != current.end(); ++it)
        if (it.key() != action && it.value().toString() == text) {
            emit notice(tr("%1 已用于其他操作").arg(text), true); return false;
        }
    auto custom = impl_->prefs.value("shortcuts").toMap();
    custom[action] = text;
    const auto previous = impl_->prefs;
    impl_->prefs["shortcuts"] = custom;
    if (!impl_->savePrefs()) { impl_->prefs = previous; emit notice(tr("快捷键保存失败"), true); return false; }
    oviora::log::info("ui-prefs", std::format("shortcut {}={}", action.toStdString(), text.toStdString()));
    logShortcuts();
    emit preferencesChanged();
    return true;
}
void QmlPlayerBridge::resetShortcuts() {
    const auto previous = impl_->prefs;
    impl_->prefs.remove("shortcuts");
    if (!impl_->savePrefs()) { impl_->prefs = previous; return; }
    logShortcuts();
    emit preferencesChanged();
}
bool QmlPlayerBridge::obsGameCaptureActive() const { return qApp && qApp->property("ovioraObsGameCapture").toBool(); }
bool QmlPlayerBridge::overlayCompatActive() const { return qApp && qApp->property("ovioraOverlayCompat").toBool(); }
bool QmlPlayerBridge::softwareUiActive() const { return qApp && qApp->property("ovioraSoftwareUi").toBool(); }
bool QmlPlayerBridge::rivaTunerRunning() const { return gfx::rivaTunerRunning(); }
bool QmlPlayerBridge::presentationOverlayRunning() const {
    return gfx::rivaTunerRunning() || gfx::nvidiaOverlayRunning() || gfx::gamePPRunning() || gfx::douyinLiveCompanionRunning();
}
// The field hangs with 抖音直播伴侣 (2026-10-15) came with this process at Realtime GPU
// priority. OBS itself never asks for Realtime when hardware-accelerated GPU scheduling
// is on (High instead, libobs-d3d11 set_priority); the same class is used here while
// the companion runs, so neither program can hold the GPU off from the other.
void QmlPlayerBridge::updateLiveCompanionCompat() {
    const bool automatic = impl_->prefString("overlayCompat") != QLatin1String("off");
    const bool running = automatic && !GetEnvironmentVariableW(L"OVIORA_TEST_IGNORE_DOUYIN", nullptr, 0)
                         && gfx::douyinLiveCompanionRunning();
    if (running == impl_->liveCompanionCap) return;
    impl_->liveCompanionCap = running;
    gfx::setGpuPriorityCap(running ? gfx::GpuPriority::High : gfx::GpuPriority::Realtime);
    const auto status = gfx::gpuPriorityStatus();
    oviora::log::warn("overlay-hooks", std::format("Douyin live companion {}: GPU priority cap={} requested={} actual={}",
        running ? "running" : "gone", running ? "high" : "none", int(status.requested), status.actual));
    if (running && status.capped)
        emit notice(tr("检测到抖音直播伴侣：GPU 优先级暂时从“实时”降为“高”，避免两个程序争用显卡时卡死；直播伴侣退出后自动恢复"), false);
    emit preferencesChanged();
}

int QmlPlayerBridge::uiScaleActive() const { return qApp ? qApp->property("ovioraUiScale").toInt() : 0; }
void QmlPlayerBridge::openFeedbackPage() {
    copyDiagnostics();
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://oviora.tr")));
    emit notice(tr("诊断信息已复制，可直接粘贴到反馈里"), false);
}
bool QmlPlayerBridge::eventFilter(QObject* watched, QEvent* event) {
    const auto type = event->type();
    // Keyboard play on the PS5 stream goes first (bridge/GameInput.cpp).
    // Mouse aim and the mouse touchpad first (bridge/MouseAim.cpp): the captured pointer, its
    // toggle key and Esc.
    if (mouseAimEvent(watched, event)) return true;
    if (gameKeyEvent(watched, event)) return true;
    if (type == QEvent::ApplicationDeactivate) { releaseGameKeys(); updateMouseAim(); }
    if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (!key->isAutoRepeat() && qobject_cast<QWindow*>(watched)) {
            const QKeySequence hold(shortcuts().value("hold").toString(), QKeySequence::PortableText);
            const int pressed = key->key() | int(key->modifiers() & ~Qt::KeypadModifier);
            // Typing into a text field never compares.
            const QObject* focus = QGuiApplication::focusObject();
            const bool typing = focus && (focus->inherits("QQuickTextInput") || focus->inherits("QQuickTextEdit"));
            if (!hold.isEmpty() && hold[0].toCombined() == pressed && !typing && impl_->prefBool("holdCompare", true)) {
                const bool down = type == QEvent::KeyPress;
                if (down != impl_->holdKeyDown) { impl_->holdKeyDown = down; holdOriginal(down); }
                return true;
            }
            if (type == QEvent::KeyRelease && impl_->holdKeyDown && key->key() == QKeySequence(hold)[0].key()) {
                impl_->holdKeyDown = false; holdOriginal(false);
            }
        }
    } else if (type == QEvent::ApplicationDeactivate && impl_->holdKeyDown) {
        impl_->holdKeyDown = false; holdOriginal(false);
    }
    return QObject::eventFilter(watched, event);
}
// Writes the shell preferences now (a pending volume included).
void QmlPlayerBridge::flushPreferences() {
    if (!impl_->savePrefs()) oviora::log::warn("ui-prefs", "preferences not saved");
}

QString QmlPlayerBridge::screenshotDirectory() const {
    const QString chosen = impl_->prefString("screenshotDir");
    if (!chosen.isEmpty()) return chosen;
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return QDir(pictures.isEmpty() ? QDir::homePath() : pictures).filePath(QStringLiteral("Oviora"));
}
QString QmlPlayerBridge::lastScreenshot() const { return impl_->lastScreenshot; }
QString QmlPlayerBridge::dataDirectory() const { return QString::fromStdWString(impl_->dataDir.wstring()); }
// main() records the file it actually opened (it falls back to a per-process name).
QString QmlPlayerBridge::logFile() const { return qApp ? qApp->property("ovioraLogFile").toString() : QString(); }
void QmlPlayerBridge::chooseScreenshotDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(nullptr, tr("选择截图目录"), screenshotDirectory());
    if (!dir.isEmpty()) setPreference(QStringLiteral("screenshotDir"), dir);
}
void QmlPlayerBridge::openScreenshotDirectory() {
    const QString dir = screenshotDirectory();
    QDir().mkpath(dir);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) emit notice(tr("无法打开目录：%1").arg(dir), true);
}
void QmlPlayerBridge::openLogFolder() {
    const QString file = logFile();
    const QString dir = file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
    if (dir.isEmpty() || !QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) emit notice(tr("日志目录不可用"), true);
}
void QmlPlayerBridge::openDataFolder() {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dataDirectory()))) emit notice(tr("无法打开数据目录"), true);
}

// --- P5 (2026-09-29): headers, colour history, export rows, presets -------
QString QmlPlayerBridge::sourceKind() const {
    const auto& s = impl_->snapshot;
    if (!hasSource()) return {};
    if (s.remotePlay) return QStringLiteral("ps5");
    if (s.image) return QStringLiteral("image");
    return impl_->liveKind;
}
QString QmlPlayerBridge::sourceTitle() const {
    const QString kind = sourceKind();
    if (kind.isEmpty()) return {};
    if (kind == QLatin1String("ps5")) return tr("PS5 串流 · %1").arg(impl_->liveLabel);
    return sourceName();
}
// "1080p60" / "1080p59.94": the reported height and nominal rate only.
QString QmlPlayerBridge::sourceRateText() const {
    const auto& s = impl_->snapshot;
    if (!s.sourceHeight) return {};
    if (s.image) return QStringLiteral("%1×%2").arg(s.sourceWidth).arg(s.sourceHeight);
    const double fps = s.nominalSourceFps;
    if (fps <= 0.0) return QStringLiteral("%1p").arg(s.sourceHeight);
    const bool whole = std::abs(fps - std::round(fps)) < 0.01;
    return QStringLiteral("%1p%2").arg(s.sourceHeight)
        .arg(whole ? QString::number(int(std::round(fps))) : QString::number(fps, 'f', 2));
}
QString QmlPlayerBridge::sourceFormatText() const {
    QStringList parts;
    const QString rate = sourceRateText();
    if (rate.isEmpty()) return {};
    parts << rate;
    const QString colour = uiText(impl_->snapshot.colorStatus);
    if (!colour.isEmpty()) parts << colour.section(QString::fromUtf8(" → "), 0, 0).trimmed();
    return parts.join(QString::fromUtf8(" · "));
}

bool QmlPlayerBridge::colourCanUndo() const { return !impl_->colourUndo.empty(); }
bool QmlPlayerBridge::colourCanRedo() const { return !impl_->colourRedo.empty(); }
bool QmlPlayerBridge::colourCanPaste() const { return impl_->colourClip.has_value(); }
namespace {
bool isColourNode(const engine::EffectChain& chain, int index) {
    return index >= 0 && uint32_t(index) < chain.nodeCount && chain.nodes[index].type == engine::EffectType::Color;
}
}
// Undo and redo re-commit a stored state through the normal path (validation,
// chain rebuild), marked as a replay so it does not push history of its own.
bool QmlPlayerBridge::colourUndo() {
    while (!impl_->colourUndo.empty()) {
        const auto step = impl_->colourUndo.back();
        impl_->colourUndo.pop_back();
        if (!isColourNode(impl_->chain, step.index)) continue;
        impl_->selectedColour = step.index;
        const auto current = selectedColourSettings();
        impl_->colourReplaying = true;
        const bool ok = commitColour(step.colour);
        impl_->colourReplaying = false;
        if (ok) impl_->colourRedo.push_back({step.index, current});
        impl_->colourLastIndex = -1;
        oviora::log::info("ui-colour", std::format("undo layer={} ok={} left={}", step.index, ok, impl_->colourUndo.size()));
        emit settingsChanged();
        return ok;
    }
    emit settingsChanged();
    return false;
}
bool QmlPlayerBridge::colourRedo() {
    while (!impl_->colourRedo.empty()) {
        const auto step = impl_->colourRedo.back();
        impl_->colourRedo.pop_back();
        if (!isColourNode(impl_->chain, step.index)) continue;
        impl_->selectedColour = step.index;
        const auto current = selectedColourSettings();
        impl_->colourReplaying = true;
        const bool ok = commitColour(step.colour);
        impl_->colourReplaying = false;
        if (ok) impl_->colourUndo.push_back({step.index, current});
        impl_->colourLastIndex = -1;
        oviora::log::info("ui-colour", std::format("redo layer={} ok={}", step.index, ok));
        emit settingsChanged();
        return ok;
    }
    emit settingsChanged();
    return false;
}
void QmlPlayerBridge::colourCopy() {
    if (impl_->colourIndex() < 0) { emit notice(tr("处理链中没有调色节点"), true); return; }
    impl_->colourClip = selectedColourSettings();
    oviora::log::info("ui-colour", "copy");
    emit settingsChanged();
    emit notice(tr("已复制调色参数"), false);
}
bool QmlPlayerBridge::colourPaste() {
    if (!impl_->colourClip) return false;
    auto next = *impl_->colourClip;
    next.enabled = selectedColourSettings().enabled;   // paste the grade, not the switch
    impl_->colourLastIndex = -1;                        // a paste is its own step
    const bool ok = commitColour(next);
    oviora::log::info("ui-colour", std::format("paste ok={}", ok));
    return ok;
}

void QmlPlayerBridge::importPresetDialog() {
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("导入预设"), QString(),
                                                      tr("Oviora 预设 (*.vpreset);;所有文件 (*)"));
    if (!path.isEmpty()) importPreset(path);
}
bool QmlPlayerBridge::importPreset(const QString& path) {
    std::wstring name;
    auto& library = impl_->facade.presets();
    const bool ok = library.importFile(std::filesystem::path(wideOf(path)), name);
    oviora::log::info("ui-preset", std::format("import ok={} name={}", ok, utf8Of(name).toStdString()));
    if (!ok) { emit notice(tr("导入失败：%1").arg(uiText(library.error())), true); return false; }
    emit presetsChanged();
    emit notice(tr("已导入预设：%1").arg(utf8Of(name)), false);
    return true;
}
void QmlPlayerBridge::exportPresetDialog(int index) {
    const auto& entries = impl_->facade.presets().entries();
    if (index < 0 || size_t(index) >= entries.size()) return;
    const QString path = QFileDialog::getSaveFileName(nullptr, tr("导出预设"),
                                                      utf8Of(entries[size_t(index)].name) + QStringLiteral(".vpreset"),
                                                      tr("Oviora 预设 (*.vpreset)"));
    if (!path.isEmpty()) exportPreset(index, path);
}
bool QmlPlayerBridge::exportPreset(int index, const QString& path) {
    auto& library = impl_->facade.presets();
    if (index < 0 || size_t(index) >= library.entries().size()) return false;
    const bool ok = library.exportEntry(size_t(index), std::filesystem::path(wideOf(path)));
    oviora::log::info("ui-preset", std::format("export index={} ok={}", index, ok));
    if (!ok) { emit notice(tr("导出失败：%1").arg(uiText(library.error())), true); return false; }
    emit notice(tr("预设已导出到：%1").arg(QDir::toNativeSeparators(path)), false);
    return true;
}

QString QmlPlayerBridge::liveOpeningText() const {
    if (!impl_->pendingLiveText.isEmpty()) return impl_->pendingLiveText;
    const auto& s = impl_->snapshot;
    if (s.running || s.failed || s.transport != engine::TransportState::Opening) return {};
    const QString status = oviora::ui::i18n::text(s.status);
    if (s.remotePlay)
        return status.contains(QStringLiteral("PIN")) ? status : tr("正在连接 PS5 · %1").arg(impl_->liveLabel);
    return {};
}


// --- application lifetime and dialog navigation ------------------------------
void QmlPlayerBridge::restartApplication() {
    requestApplicationRestart(false);
}
void QmlPlayerBridge::restartForOverlayCompatibility() {
    requestApplicationRestart(true);
}
void QmlPlayerBridge::requestApplicationRestart(bool overlayCompatibility) {
    flushPreferences();
    qApp->setProperty("ovioraOverlayCompatRestart", overlayCompatibility);
    // main launches the replacement only after UI/engine owners unwind.
    QCoreApplication::exit(42);
}

void QmlPlayerBridge::quit() {
    flushPreferences();
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) app->quit();
}

void QmlPlayerBridge::openPs5Dialog() { emit navigate(QStringLiteral("ps5")); }
void QmlPlayerBridge::openAudioDialog() { emit navigate(QStringLiteral("audio")); }
void QmlPlayerBridge::openSubtitleDialog() { emit navigate(QStringLiteral("subtitle")); }


} // namespace oviora::ui
