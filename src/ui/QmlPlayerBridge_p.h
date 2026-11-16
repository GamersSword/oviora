#pragma once
// Private bridge state shared by cohesive implementation modules.
// Public QML API, ownership, polling and settings contracts remain in QmlPlayerBridge.h.
#include "oviora/ui/QmlPlayerBridge.h"
#include "oviora/gfx/PresentSink.h"
#include "oviora/gfx/NvidiaVrr.h"
#include "oviora/ngx/DlssgLite.h"
#include "oviora/RuntimePaths.h"
#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QScreen>
#include <QtGui/qscreen_platform.h>
#include <shellapi.h>
#include <QElapsedTimer>
#include <QScopedValueRollback>
#include "oviora/ui/EffectAvailability.h"
#include "oviora/engine/FrameGenerationSelection.h"
#include "oviora/ui/UiLanguage.h"
#include <QQmlEngine>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QSet>

#include <QRegion>
#include <QCursor>
#include <QScreen>
#include <QWindow>
#include <QPlatformSurfaceEvent>
#include <QPointer>

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QKeyEvent>
#include <QKeySequence>
#include <QCoreApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QFile>
#include <QSaveFile>
#include <QTimer>
#include <QBuffer>
#include <QImage>
#include <QImageReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "oviora/Log.h"
#include "oviora/RuntimePaths.h"
#include "oviora/pipeline/VfgBackend.h"
#include "oviora/gfx/GpuSchedulingPriority.h"
#include "oviora/engine/ColorLookStore.h"
#include "oviora/engine/ColorLut.h"
#include "oviora/engine/Subtitles.h"
#include "oviora/engine/PlaybackRate.h"
#include "oviora/sink/WasapiAudioSink.h"
#ifdef OVIORA_ENABLE_REMOTEPLAY
// SDL3-static's system libraries (see CMakeLists: kept off the link line).
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dinput8.lib")
#include "oviora/remoteplay/ControllerInput.h"
#include "oviora/remoteplay/Microphone.h"
#include "oviora/remoteplay/Discovery.h"
#include "oviora/remoteplay/ProfileStore.h"
#include "oviora/source/RemotePlaySource.h"
#endif
#include <future>
#include <functional>
#include <map>
#include <deque>
#include "SubtitleOverlay.h"
#include "VideoOverlays.h"
// GDI+ is only started here (the overlay draws with it); its headers expect
// min/max, which NOMINMAX builds do not define as macros.
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
// 1.4.4's live status rules (正常 / 补帧调度降档 / 输出未达标 / 输入帧率不足).
#include "LiveStatusHistory.h"
// GPU utilization for the performance orb: the Windows "GPU Engine" counters,
// the same source Task Manager reads.
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#include <atomic>
#include <set>
#include <mutex>
#include <thread>
#include "oviora/pipeline/ColorGradeTables.h"
#include "oviora/ui/PlaybackPowerGuard.h"
#include "oviora/gfx/XessMfgUnlock.h"
#include "oviora/gfx/D3D12DeviceContext.h"
#include "oviora/gfx/PresentationHooks.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "oviora/ngx/NrArchitecturePolicy.h"
#include "oviora/ngx/FgCompatibilitySession.h"

namespace oviora::ui {
namespace bridge_detail {
inline QString utf8Of(std::string_view s) { return QString::fromUtf8(s.data(), int(s.size())); }
inline QString utf8Of(const std::wstring& w) { return QString::fromWCharArray(w.c_str(), int(w.size())); }

// Where crash and device-loss hints go: next to the log, where the crash handler in
// apps/oviora-qml/main.cpp writes them too.
inline QString lastFailurePath() {
    return QFileInfo(qApp->property("ovioraLogFile").toString()).absolutePath() + QStringLiteral("/oviora-last-failure.txt");
}

// What the user can switch off, for the injected components named in `modules`
// (RTSSHooks64.dll, see gfx::riskyInjections). Empty when none is known.
inline QString injectionAdvice(const QString& modules) {
    QStringList parts;
    if (modules.contains(QStringLiteral("RTSSHooks64.dll"), Qt::CaseInsensitive) && !qApp->property("ovioraSoftwareUi").toBool())
        parts << QCoreApplication::translate("QmlPlayerBridge", "检测到 RivaTuner（小飞机 OSD）注入了 Oviora。请把 设置 → 监控软件兼容 设为“自动”并在小飞机运行时重启 Oviora：界面改用兼容绘制，OSD 只显示在视频上");
    return parts.join(QStringLiteral("；"));
}
// Messages and fixed table labels shown to the user, in the interface language
// (UiLanguage.h). Data that only passes through the UI - paths, device, preset and
// LUT names - keeps utf8Of(), and so do log lines.
inline QString uiText(const std::wstring& w) { return oviora::ui::i18n::text(w); }
inline QString uiText(std::string_view s) { return oviora::ui::i18n::text(QString::fromUtf8(s.data(), int(s.size()))); }
inline std::wstring wideOf(const QString& s) {
    return std::wstring(reinterpret_cast<const wchar_t*>(s.utf16()), size_t(s.size()));
}
inline QString mmss(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    const int total = int(seconds);
    const int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}
} // namespace bridge_detail
using namespace bridge_detail;

QVariantMap currentEffectCapabilities(bool refresh=false);
QString unavailableEffects(const engine::EnhancementSettings& settings);
QString disableUnavailableEffects(engine::ChainConfiguration& configuration);
ui::EffectGpu effectGpu();

namespace bridge_detail {
// WM_DISPLAYCHANGE reaches every top-level window when a display mode, refresh
// rate or HDR state, or the set of screens, changes: the 可变刷新 state is then
// read once more (DisplayStateService) instead of on a timer.
struct DisplayChangeFilter final : QAbstractNativeEventFilter {
    bool* changed = nullptr;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
        if (changed && type == "windows_generic_MSG" && static_cast<const MSG*>(message)->message == WM_DISPLAYCHANGE) *changed = true;
        return false;
    }
};
} // namespace bridge_detail

struct QmlPlayerBridge::Impl {
    // Per-layer on/off before the NR master switch turned them all off.
    std::vector<bool> nrMasterRestore;
    QQmlEngine* qmlEngine = nullptr;
    engine::EngineController& engine;
    PlayerUiFacade facade;

    // Last published snapshot and the revision it came from. Polling compares
    // revisions, so an unchanged snapshot costs one comparison per tick.
    engine::PlayerSnapshot snapshot;
    uint64_t publishedRevision = 0;
    bool haveSnapshot = false;
    bool openingSource = false;
    uint64_t openingSessionId = 0;
    uint64_t vramNoticeSession = 0;   // the session the video-memory warning was shown for

    // The chain the UI edits, kept in step with the facade's pending settings.
    engine::EffectChain chain;
    engine::EffectChain acceptedChain;
    engine::NodeGraphLayout layout;
    engine::ChainGlobalSettings draftGlobals;
    const engine::EffectChain& activeRuntime() const {
        return chain.mode == engine::ChainMode::Node ? acceptedChain : chain;
    }
    void initializeDefaultNodePositions() {
        if (chain.mode != engine::ChainMode::Node) return;
        for (uint32_t i = 0; i < chain.nodeCount; ++i)
            if (chain.nodes[i].viewX == 0 && chain.nodes[i].viewY == 0) {
                chain.nodes[i].viewX = float(i % 4) * 260;
                chain.nodes[i].viewY = float(i / 4) * 300;
            }
    }
    void restoreConfiguration(const engine::ChainConfiguration& c) {
        draftGlobals = c.editor && c.editor->globals ? *c.editor->globals :
            static_cast<const engine::ChainGlobalSettings&>(c);
        acceptedChain = c.chain;
        chain = c.editor ? c.editor->nodes : c.chain;
        if (c.editor) layout = c.editor->layout;
        else if (chain.mode == engine::ChainMode::Node) layout.initialize(chain);
        selectedNr = c.selectedNr; selectedColour = c.selectedColour;
        // Resolve the old QML-only layout fallback once so duplication, wires
        // and saved coordinates use the same physical rectangles.
        if (!c.editor) initializeDefaultNodePositions();
    }
    engine::ChainValidation validation;
    engine::ChainSession session;
    engine::ChainSession savedSession;
    QTimer* sessionSaveTimer = nullptr;
    // 音量 is a shell preference (2026-11-08); a drag is written once it settles.
    QTimer* volumeSaveTimer = nullptr;
    int selectedNr=-1;
    int nrIndex() const {
        if(selectedNr>=0&&uint32_t(selectedNr)<chain.nodeCount&&
           chain.nodes[selectedNr].type==engine::EffectType::NrEnhance)return selectedNr;
        for(uint32_t i=0;i<chain.nodeCount;++i)if(chain.nodes[i].type==engine::EffectType::NrEnhance)return int(i);
        return -1;
    }
    engine::ChainNode* nrNode(){const int i=nrIndex();return i>=0?&chain.nodes[i]:nullptr;}
    int selectedColour=-1;
    int colourIndex() const {
        if(selectedColour>=0&&uint32_t(selectedColour)<chain.nodeCount&&
           chain.nodes[selectedColour].type==engine::EffectType::Color)return selectedColour;
        for(uint32_t i=0;i<chain.nodeCount;++i)if(chain.nodes[i].type==engine::EffectType::Color)return int(i);
        return -1;
    }

    // The window the engine presents into, and the last file the UI opened.
    HWND videoWindow = nullptr;
    std::wstring sourceLabel;
    // The live source kind and label ("" for a file), for the page headers.
    QString liveKind, liveLabel;
    // A live open waiting for the 极简 switch to settle (openAfterCinema); the
    // generation lets a newer open or a stop cancel it.
    int liveOpenGen = 0;
    QString pendingLiveText;
    // 调色 history: each commit pushes the layer's previous state; a slider
    // drag (edits under 600 ms apart on one layer) is one step.
    struct ColourStep { int index = -1; engine::ColorSettings colour{}; };
    std::vector<ColourStep> colourUndo, colourRedo;
    std::optional<engine::ColorSettings> colourClip;
    qint64 colourLastPushMs = 0;
    int colourLastIndex = -1;
    bool colourReplaying = false;
    // Poster data URL cache, per session.
    uint64_t posterSession = 0;
    QString posterUrl;

    // Keeps the display and system awake while a video plays (AppShell rule: running,
    // not failed, not a still image, transport Playing). Acquired and released on
    // this GUI thread, as SetThreadExecutionState requires.
    PlaybackPowerGuard power;
    std::function<void()> preOpen;

    engine::PlayerOptions options;
    QString initialPageOverride;
    QString currentPage;
    QString remotePlayHost, remotePlayPin;

    QTimer* timer = nullptr;

    // Where the user's own files live (the facade resolves the same default).
    std::filesystem::path dataDir;
    // --- subtitles (ported from the Win32 shell, same semantics) ---
    std::unique_ptr<engine::SubtitleLoader> subLoader;
    // Embedded-subtitle scan of a file on a server (#48): reads only while playback has its own
    // buffer. The gate follows the engine's network statistics (tickSubtitles); null for local files.
    std::shared_ptr<std::atomic<bool>> subScanGate;
    bool subScanOpen = false;
    std::unique_ptr<engine::SubtitleLoader> subFileLoader;
    uint64_t subFileGeneration = 0;
    std::wstring subFilePath;
    uint64_t subGeneration = 0;
    std::vector<engine::SubtitleTrack> subTracks;   // external files first, then the loader's
    size_t subExternal = 0;                          // user-loaded files at the front
    size_t subLoaded = 0;                            // loader tracks currently held
    int subPrimary = -1, subSecondary = -1;
    bool subPrimaryChosen = false, subSecondaryChosen = false;
    QString subStatus;
    QString subText;
    int subBottomInset = 0;
    HWND subOverlay = nullptr;
    bool subOverlayFailed = false;
    std::wstring subMedia;
    std::atomic<bool> subAligning{false}, subAlignReady{false};
    std::atomic<int> subAlignOffset{0};
    std::mutex subAlignMutex;
    std::wstring subAlignDetail;
    uint64_t subAlignGeneration = 0;
    int subAlignTrack = -1;
    // Declared after everything the worker touches, so it is joined first.
    std::jthread subAlignWorker;
    QVariantMap prefs;
  // Custom: the display whose HDR curve is currently applied (see syncHdrDisplayProfile).
  QString hdrProfileDisplay;
  // Custom: the title whose curve is currently applied (see syncHdrTitleProfile).
  QString hdrTitleApplied;
  HMONITOR hdrIdentityMonitor=nullptr;
  QString hdrIdentityKey;
  bool hdrRestoring=false,hdrCoverageObserved=false,hdrMemoryDirty=false;
  QTimer* hdrMemorySaveTimer=nullptr;
  // Custom: how many remembered titles survive; the oldest fall off first.
  int kHdrTitleProfileLimit = 60;
    QString lastScreenshot;
    QString pendingScreenshot;
    qint64 pendingScreenshotDeadline=0;
    bool holdKeyDown = false;
    std::vector<sink::RenderEndpoint> audioEndpoints;
    int uiDeviceGeneration = 0;   // interface scene graphs recreated after a lost device
    // Settings → 组件与许可. Read once and on refreshComponentList(): the property used to
    // notify with every engine snapshot, so each 60 Hz tick re-read the manifest and
    // rebuilt the page's 49 rows (audit 2026-10-15 B5).
    QVariantList componentList; bool componentListRead = false;
    bool liveCompanionCap = false;   // updateLiveCompanionCompat
    // --- PS5 Remote Play ---
    QVariantMap ps5Form;
    QString ps5Status;
    std::wstring ps5Profile;                 // file name inside profileDirectory()
    // Oviora: set by connectPsn() for the one ps5Connect() it makes; a PS5 over PSN.
    std::shared_ptr<const remoteplay::PsnRoute> pendingPsn;
    QString pendingPsnName;
    QVariantList ps5ProfileList;
    bool ps5Watching = false;
    // The connecting screen / error card (RemotePlay.cpp updateConnection)
    QVariantMap connection;
    QString connectionError;      // kept after a failed connection until the card is closed
    bool connectionPsn = false;   // the current/last connection goes over PSN
    QString connectionName;       // the console's name for the connecting screen ("" = unknown)
    bool connectionSeen = false;  // a connection was opening or live in this attempt
    qint64 connectionStartMs = 0;
    std::filesystem::path ps5EditTarget;  // "Düzenle": the registered console whose file a new pairing replaces
    int pairRevision = 0;                 // grows with every saved pairing (the dialog sees an edit finish)
    QString wakeWaitKey;          // a woken console waited for (lanWake), connected when it answers as ready
    std::chrono::steady_clock::time_point wakeDeadline{};
    struct Ps5Outcome { std::wstring message; std::vector<std::pair<std::string, std::string>> hosts; std::wstring savedPath; bool refreshProfiles = false; };
    std::mutex ps5Mutex;
    Ps5Outcome ps5Outcome;
    std::atomic<bool> ps5Done{false};
    bool ps5Busy = false;
    QTimer* controllerTimer = nullptr;
    // Game input (bridge/GameInput.cpp), parsed from the preferences by loadGameInput().
    struct GameInput {
        bool menuCombo = true;
        uint32_t menuButtons = 0;                 // ControllerState bits; 0: no combination
        std::array<uint8_t, 16> padSource{};      // PS5 button i is pressed by pad button padSource[i]
        bool padRemapped = false;
        float rumble = 1.f, haptics = 1.f;
        bool keyboard = true;                     // keyboardMouse && keyboardPlay
        QHash<int, uint32_t> keys;                // Qt key -> PS5 action (old Chiaki button values)
    } gameInput;
    bool gameKeysAllowed = false;
    QHash<int, uint32_t> keysDown;               // key held -> the action it pressed
    uint32_t keysHeld = 0, keysTapped = 0;       // actions; a tap is sent for one tick at least
    uint32_t keysSent = 0;                       // last keyboard actions logged
    bool menuComboHeld = false;
    int padPressed = 0;
    bool padCaptureOn = false;
    QTimer* padCaptureTimer = nullptr;
    // Oviora home LAN list: one discovery at a time on its own thread, every few seconds.
    bool lanSearch = true;
    bool lanLoaded = false;
    std::atomic<bool> lanDone{false};
    std::mutex lanMutex;
    std::vector<remoteplay::DiscoveredConsole> lanFound, lanResult;
    std::chrono::steady_clock::time_point lanNext{};
    // bridge/StreamSession.cpp. Auto connect: the paired PS5 (profile id) to connect to once it
    // is found after the start; given up after a while or when another stream starts.
    QString autoConnectPending;
    bool autoConnectWoke = false;
    std::chrono::steady_clock::time_point autoConnectUntil{};
    // The microphone: wanted for this stream (the menu tile), the stream it belongs to.
    bool micWanted = false;
    uint64_t micSession = 0;
    QStringList micDevices;
    bool micDevicesRead = false;
    // The PC going to sleep (WM_POWERBROADCAST, PBT_APMSUSPEND) reaches every Qt window.
    struct PowerFilter final : QAbstractNativeEventFilter {
        std::function<void()> suspending;
        bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
            const auto* msg = static_cast<const MSG*>(message);
            if (type == "windows_generic_MSG" && msg->message == WM_POWERBROADCAST && msg->wParam == PBT_APMSUSPEND && suspending) suspending();
            return false;
        }
    } powerFilter;
    std::chrono::steady_clock::time_point lastSuspend{};
    // Mouse aim and the mouse touchpad (bridge/MouseAim.cpp), old MouseAim / mouse_touch_enabled.
    struct MouseAim {
        enum Action { None = 0, R2, L2, R1, L1, R3, L3, Square, Cross, Circle, Triangle, ActionCount };
        // Settings (preference mouseAim; keyboardMouse switches it off too)
        bool enabled = false;
        int sensitivity = 50, accel = 30;
        bool invertY = false;
        int actions[3] = {R2, L2, R3};           // left, right, middle
        int toggleKey = Qt::Key_F8;
        bool touch = true;                        // preference mouseTouch (and keyboardMouse)
        // Capture
        bool wanted = true;                       // the toggle key's choice; true again for every stream
        bool captured = false;
        bool test = false;                        // --test-mouse: never clip or hide the real cursor
        QPointer<QWindow> window;                 // the window captured into
        HWND hwnd = nullptr;
        uint64_t session = 0;                     // the stream wanted belongs to
        // Raw movement since the last tick, the smoothed speed (px/s) and the stick it gives
        double accX = 0, accY = 0, velX = 0, velY = 0;
        bool absKnown = false; long absX = 0, absY = 0;
        std::chrono::steady_clock::time_point lastTick{}, lastMotion{}, capturedAt{};
        bool down[3] = {false, false, false};
        uint32_t buttons = 0; uint8_t l2 = 0, r2 = 0; int16_t rightX = 0, rightY = 0;
        uint32_t loggedButtons = 0; int16_t loggedX = 0, loggedY = 0; uint8_t loggedL2 = 0, loggedR2 = 0;
        std::chrono::steady_clock::time_point loggedAt{};
        uint64_t rawPackets = 0;
        // The mouse touchpad: one finger while the left button drags over the picture,
        // another button clicks the touchpad.
        int8_t touchId = -1; uint16_t touchX = 0, touchY = 0; bool touchClick = false;
        uint8_t nextTouchId = 0x60;
    } mouseAim;
    struct RawMouseFilter final : QAbstractNativeEventFilter {
        std::function<void(const RAWMOUSE&)> received;
        HWND target = nullptr;
        bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
            const auto* msg = static_cast<const MSG*>(message);
            if (type != "windows_generic_MSG" || msg->message != WM_INPUT || !target || msg->hwnd != target || !received) return false;
            RAWINPUT raw{}; UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return false;
            if (raw.header.dwType == RIM_TYPEMOUSE) received(raw.data.mouse);
            return false; // DefWindowProc still frees the input
        }
    } rawMouseFilter;
#ifdef OVIORA_ENABLE_REMOTEPLAY
    remoteplay::Microphone mic;
    remoteplay::ControllerInput controller;
    // The SDL pad reader the PS5 stream uses; acquired while a session runs.
    enum PadUser : unsigned { PadPs5 = 1, PadMapping = 2 };
    unsigned padUsers = 0;
    bool acquirePad(unsigned user) {
        const bool ok = controller.start(); // no-op once initialized
        padUsers |= user;
        return ok;
    }
    void releasePad(unsigned user) {
        padUsers &= ~user;
        if (!padUsers) controller.stop();
    }
#endif
    std::jthread lanWorker;
    std::jthread ps5Worker;                  // last: joined before what it touches
    sink::ActiveRenderEndpoint audioActive;
    int audioPollTicks = 0;
    // --- protection drawing (#8) ---
    QString protDraw;                         // "" | rect | ellipse
    bool protExclusion=false;
    HWND protOverlay = nullptr;
    std::unique_ptr<RegionDrawOverlay> drawOverlay;   // takes the drag over the picture
    // --- statistics panel over the picture ---
    std::unique_ptr<StatsOverlay> statsOverlay;
    bool statsWanted = false;
    QRectF statsRect;                         // logical pixels of the main window
    int statsTicks = 0;
    QSet<QString> statsStages;                // GPU stage rows shown since the panel appeared
    // --- performance orbs / status light (#13) ---
    ui::live_status::DashboardHistory history;
    int historyTicks = 0;
    QString runStatus, runLevel, runDetail;
    QStringList runOptions;
    double effectiveOutputFps=-1;
    // FG output hint for the 倍率 row; 可变刷新 level (gfx::vrrLevel) of the
    // presenting monitor, or of the main window's monitor while idle.
    QString fgDisplayHint;
    int vrrState=0;
    // The shown monitor's Windows HDR (-1 unknown) and refresh rate.
    int displayHdr=-1;double displayRefreshHz=0;
    // 刷新显示器状态: the display generation it asked for, until the shown monitor
    // answers (displayChecking also covers any query in flight).
    bool displayChecking=false,displayRecheckPending=false,displayChanged=false;
    uint64_t displayRecheckGeneration=0;
    std::chrono::steady_clock::time_point displayRecheckSince{};
    DisplayChangeFilter displayChangeFilter;
    std::atomic<double> gpuUtil{-1.0};
    // Hardware adapters in Windows' high-performance order (discrete first). The
    // sampler only counts engines of the adapter whose LUID is in gpuMonitorLuid
    // (0: every adapter, the old behaviour, used only when none was found).
    struct GpuAdapter { QString id, name; uint32_t luidHigh = 0, luidLow = 0; };
    std::vector<GpuAdapter> gpuAdapters;
    std::atomic<uint64_t> gpuMonitorLuid{0};
    QString gpuMonitorName;
    // LUIDs that have "GPU Engine" counter instances. DXGI can list one physical GPU
    // twice (a virtual display driver such as GameViewer's exposes a second adapter
    // with the GPU's own name); only the real one has engines to measure.
    static std::set<uint64_t> gpuEngineLuids() {
        std::set<uint64_t> found;
        DWORD counterBytes = 0, instanceBytes = 0;
        if (PdhEnumObjectItemsW(nullptr, nullptr, L"GPU Engine", nullptr, &counterBytes, nullptr, &instanceBytes, PERF_DETAIL_WIZARD, 0) != PDH_MORE_DATA)
            return found;
        std::vector<wchar_t> counters(counterBytes + 1), instances(instanceBytes + 1);
        if (PdhEnumObjectItemsW(nullptr, nullptr, L"GPU Engine", counters.data(), &counterBytes, instances.data(), &instanceBytes, PERF_DETAIL_WIZARD, 0) != ERROR_SUCCESS)
            return found;
        for (const wchar_t* name = instances.data(); *name; name += wcslen(name) + 1) {
            unsigned high = 0, low = 0;
            if (const wchar_t* at = wcsstr(name, L"luid_"); at && swscanf_s(at, L"luid_0x%x_0x%x", &high, &low) == 2)
                found.insert(uint64_t(high) << 32 | low);
        }
        return found;
    }
    void enumerateGpuAdapters() {
        const auto measured = gpuEngineLuids();
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf()))); ++i) {
            DXGI_ADAPTER_DESC1 d{};
            if (FAILED(adapter->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;
            const uint64_t luid = uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32 | d.AdapterLuid.LowPart;
            if (!measured.empty() && !measured.count(luid)) {
                oviora::log::info("perf", std::format("gpu adapter skipped (no engine counters) luid={:08X}:{:08X} {}", uint32_t(luid >> 32), uint32_t(luid),
                    QString::fromWCharArray(d.Description).toStdString()));
                continue;
            }
            // LUIDs change at every boot, so the saved choice is vendor:device:subsystem,
            // with an ordinal for two identical cards.
            QString id = QStringLiteral("%1:%2:%3").arg(d.VendorId, 4, 16, QLatin1Char('0')).arg(d.DeviceId, 4, 16, QLatin1Char('0')).arg(d.SubSysId, 8, 16, QLatin1Char('0'));
            int same = 0;
            for (const auto& a : gpuAdapters) if (a.id.section('#', 0, 0) == id) ++same;
            if (same) id += QStringLiteral("#%1").arg(same);
            gpuAdapters.push_back({id, QString::fromWCharArray(d.Description).trimmed(), uint32_t(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart});
            oviora::log::info("perf", std::format("gpu adapter {} id={} luid={:08X}:{:08X} dedicatedMiB={}", gpuAdapters.size() - 1,
                id.toStdString(), uint32_t(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart, d.DedicatedVideoMemory >> 20));
        }
    }
    // The monitored GPU also computes when 设置 -> "同时用这块显卡运算" is on
    // and a specific GPU (not 自动) is chosen; otherwise the default order.
    QString computeGpuName;
    uint64_t computeLuid = 0;
    void applyComputeGpu() {
        const QString wanted = prefString("monitorGpu");
        const GpuAdapter* chosen = nullptr;
        for (const auto& a : gpuAdapters) if (!wanted.isEmpty() && a.id == wanted) chosen = &a;
        const bool follow = prefBool("computeOnMonitorGpu", false) && chosen;
        computeLuid = follow ? (uint64_t(chosen->luidHigh) << 32 | chosen->luidLow) : 0;
        computeGpuName = follow ? chosen->name : QString();
        gfx::setPreferredAdapterLuid(computeLuid);
        oviora::log::info("perf", std::format("compute gpu follow={} wanted={} using={}", follow, wanted.isEmpty() ? "auto" : wanted.toStdString(),
            follow ? chosen->name.toStdString() : "default order (highest-performance adapter)"));
    }
    void resolveGpuMonitor() {
        const QString wanted = prefString("monitorGpu");
        const GpuAdapter* chosen = gpuAdapters.empty() ? nullptr : &gpuAdapters.front();
        for (const auto& a : gpuAdapters) if (!wanted.isEmpty() && a.id == wanted) chosen = &a;
        gpuMonitorLuid = chosen ? (uint64_t(chosen->luidHigh) << 32 | chosen->luidLow) : 0;
        gpuMonitorName = chosen ? chosen->name : QString();
        oviora::log::info("perf", std::format("gpu monitor wanted={} using={}", wanted.isEmpty() ? "auto" : wanted.toStdString(),
            chosen ? chosen->name.toStdString() : "all adapters"));
    }
    double gpuPublished = -2.0;
    double rateRatio = -1.0;
    std::deque<double> frameTimes;           // frame interval ms, newest last
    // --- design gap: presentation, compare, aspect ---
    engine::PresentationSettings presentation;
    int compareMode = 0;
    bool compareBase = false;
    double compareSplit = 0.5;
    int aspectMode = 0, aspectApplied = 0;
    double viewZoom = 1.0;
    // What the last applied aspect was computed from (applyAspect skips an unchanged one).
    QSize aspectClient, aspectImage;
    double aspectDar = -1;
    uint64_t aspectSession = 0;
    std::jthread gpuSampler;                 // joined before gpuUtil goes away
    std::filesystem::path prefsFile() const { return dataDir / L"qml-preferences.v1.json"; }
    void loadPrefs() {
        QFile file(QString::fromStdWString(prefsFile().wstring()));
        if (!file.exists()) return;
        if (!file.open(QIODevice::ReadOnly)) { oviora::log::warn("ui-prefs", "qml preferences unreadable; defaults used"); return; }
        QJsonParseError error{};
        const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            oviora::log::warn("ui-prefs", "qml preferences damaged; defaults used, file kept");
            return;
        }
        prefs = doc.object().toVariantMap();
    }
    bool savePrefs() {
        const QString path = QString::fromStdWString(prefsFile().wstring());
        // A fresh data directory may not exist yet: the first preference saved
        // on a new profile used to fail and was silently kept only in memory.
        std::error_code ec;
        std::filesystem::create_directories(prefsFile().parent_path(), ec);
        if(ec)return false;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        const auto bytes = QJsonDocument(QJsonObject::fromVariantMap(prefs)).toJson(QJsonDocument::Indented);
        if (file.write(bytes) != bytes.size()) return false;
        return file.commit();
    }
    int prefInt(const char* key, int fallback) const { return prefs.contains(key) ? prefs.value(key).toInt() : fallback; }
    bool prefBool(const char* key, bool fallback) const { return prefs.contains(key) ? prefs.value(key).toBool() : fallback; }
    QString prefString(const char* key) const { return prefs.value(key).toString(); }
    Impl(engine::EngineController& e, std::filesystem::path dataDirectory)
        : engine(e), facade(e, dataDirectory),
          dataDir(dataDirectory.empty() ? runtime::localDataDirectory() : dataDirectory) {
        // Same high-performance DXGI ordering as the engine. This only seeds a
        // fresh session; loading saved settings below preserves manual choices.
        gfx::ComPtr<IDXGIFactory6> factory;
        if(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)))) {
            gfx::ComPtr<IDXGIAdapter1> adapter;
            for(UINT i=0;SUCCEEDED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)));++i) {
                DXGI_ADAPTER_DESC1 desc{};
                if(FAILED(adapter->GetDesc1(&desc))||(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
                auto initial=facade.pendingSettings();
                initial.nrRuntime=desc.VendorId==0x1002?engine::NrRuntime::LmxxfAmd:engine::NrRuntime::Ampere;
                if(desc.VendorId==0x1002||desc.VendorId==0x8086){
                    initial.opticalFlowBackend=engine::OpticalFlowBackend::AmdFidelityFx;
                    initial.videoSrQuality=engine::kVideoSrFsr;
                }
                initial.frameGenerationBackend=engine::defaultFrameGenerationBackend(desc.VendorId);
                facade.setPending(initial);
                oviora::log::info("nr-default",std::format("adapter={} selected={}",utf8Of(std::wstring(desc.Description)).toStdString(),engine::nrRuntimeName(initial.nrRuntime)));
                break;
            }
        }
        chain = engine::toChain(facade.pendingSettings());
        acceptedChain = chain;
        draftGlobals = engine::ChainGlobalSettings::capture(facade.pendingSettings());
        session = engine::ChainSession::initial(facade.pendingSettings());
        savedSession = session;
        validation = engine::validateChain(chain);
        options = engine::PlayerOptions::from(facade.pendingSettings());
    }

    engine::ChainSession currentSession() const {
        auto current = session;
        current.active = chain.mode;
        current.initialized[size_t(chain.mode)] = true;
        current.configurations[size_t(chain.mode)] = engine::ChainConfiguration::capture(
            activeRuntime(), facade.pendingSettings(), nrIndex(), colourIndex());
        // Custom: the static HDR output tuning does not travel through the runtime
        // configuration yet, so take it straight from the draft globals at the point
        // the session is built. Without this the values took effect and then came
        // back as defaults on the next start.
        current.configurations[size_t(chain.mode)].hdrCurve = draftGlobals.hdrCurve;
        current.configurations[size_t(chain.mode)].hdrMetadata = draftGlobals.hdrMetadata;
        if (chain.mode == engine::ChainMode::Node) {
            auto document = std::make_shared<engine::NodeEditorDocument>();
            document->nodes = chain; document->layout = layout; document->globals = draftGlobals;
            current.configurations[size_t(chain.mode)].editor = std::move(document);
        }
        return current;
    }

    bool persistSession() {
        const auto current = currentSession();
        if (current == savedSession) return true;
        if (!facade.saveChainSession(current)) return false;
        session = savedSession = current;
        return true;
    }

    bool poll() {
        const auto before=facade.pendingSettings();
        auto frame = facade.poll();
        const auto restored=facade.pendingSettings();
        const bool fgRollback=before.frameGenerationBackend!=restored.frameGenerationBackend||before.multiplier!=restored.multiplier||before.vfgQuality!=restored.vfgQuality;
        const bool nrRollback=before.nrRuntime!=restored.nrRuntime;
        if(nrRollback) {
            for(auto* c:{&chain,&acceptedChain})for(uint32_t i=0;i<c->nodeCount;++i)
                if(c->nodes[i].type==engine::EffectType::NrEnhance)c->nodes[i].nr.runtime=restored.nrRuntime;
        }
        if(fgRollback){
            const auto restoreFg=[&](engine::EffectChain& value){
                if(restored.multiplier>1)value.fgMultiplier=restored.multiplier;
                for(uint32_t i=0;i<value.nodeCount;++i)
                    if(value.nodes[i].type==engine::EffectType::FrameGeneration)value.nodes[i].enabled=restored.multiplier>1;
            };
            restoreFg(chain);restoreFg(acceptedChain);
            if(draftGlobals.fgBackend==before.frameGenerationBackend)draftGlobals.fgBackend=restored.frameGenerationBackend;
            if(draftGlobals.vfgQuality==before.vfgQuality)draftGlobals.vfgQuality=restored.vfgQuality;
            options=engine::PlayerOptions::from(restored,engine::runtimeOrder(acceptedChain));
        }
        if (!frame.changed && haveSnapshot) return fgRollback||nrRollback;
        snapshot = frame.snapshot;
        publishedRevision = frame.revision;
        haveSnapshot = true;
        if (openingSource && openingSessionId != 0 && snapshot.sessionId == openingSessionId &&
            (snapshot.running || snapshot.image || snapshot.failed ||
             snapshot.transport == engine::TransportState::Empty ||
             snapshot.transport == engine::TransportState::Stopping))
            openingSource = false;
        return fgRollback||nrRollback;
    }

    // The chain is the source of truth for the stage settings; every commit
    // writes it back over the struct so the two can never disagree.
    bool commit(engine::EnhancementSettings settings) {
        settings.content=engine::currentContentRate(settings.content);
        // Custom: the static HDR output tuning belongs to neither transaction, so
        // keep the draft globals in sync in BOTH modes. Only the node branch below
        // refreshed them, which is why a change made in list mode took effect and
        // then came back as a default after a restart (the session capture reads the
        // tuning from the draft globals).
        draftGlobals.hdrCurve = settings.hdrCurve;
        draftGlobals.hdrMetadata = settings.hdrMetadata;
        if (chain.mode == engine::ChainMode::Node) {
            const auto previous = facade.pendingSettings();
            const auto oldGlobals = draftGlobals;
            draftGlobals = engine::ChainGlobalSettings::capture(settings);
            // Non-node controls (e.g. audio) may still change while a wire is
            // incomplete. Keep runtime globals separate from that transaction.
            // Custom: HDR brightness is one of those controls - it is not part of a
            // node edit, so the restore below must not put the previous value back
            // (field report: a new target peak took effect and then reverted on the
            // next start, because this line overwrote it before the session was
            // captured).
            facade.setPending(settings);
            if (revalidate(true, &previous)) return true;
            draftGlobals = oldGlobals; facade.setPending(previous);
            revalidate();
            return false;
        }
        const auto& runtime = activeRuntime();
        engine::fromChain(runtime, settings);
        if (const auto reason = unavailableEffects(settings); !reason.isEmpty()) {
            oviora::log::warn("effect-availability", reason.toStdString());
            return false;
        }
        if(!facade.applySettings(settings,engine::runtimeOrder(runtime)))return false;
        options = engine::PlayerOptions::from(settings,engine::runtimeOrder(runtime));
        return true;
    }

    bool revalidate(bool apply=false, const engine::EnhancementSettings* prior=nullptr) {
        auto projected = std::make_unique<engine::EffectChain>(chain);
        bool incomplete = false;
        if (chain.mode == engine::ChainMode::Node) {
            validation = layout.validate(chain);
            if (!validation.accepted) return false;
            // Even detached controls must obey the persistent parameter contract.
            auto check = std::make_unique<engine::ChainConfiguration>(
                engine::ChainConfiguration::capture(acceptedChain, facade.pendingSettings()));
            auto document = std::make_shared<engine::NodeEditorDocument>();
            document->nodes = chain; document->layout = layout; document->globals = draftGlobals; check->editor = document;
            if (!check->valid()) { validation = {false, "节点参数无效，未提交"}; return false; }
            validation = layout.project(chain, *projected);
            if (!validation.accepted) {
                uint32_t end = layout.inputNext;
                while (end > engine::NodeGraphLayout::Output)
                    end = layout.next[size_t(layout.indexOf(chain, end))];
                // Incomplete wires are editable/savable. A complete but illegal
                // path is rejected atomically by the caller, never auto-sorted.
                if (end != engine::NodeGraphLayout::Input) return false;
                *projected = acceptedChain; incomplete = true;
            }
        } else validation = engine::validateChain(chain);
        if(!validation.accepted && !incomplete)return false;
        const auto previous = prior ? *prior : facade.pendingSettings();
        auto s = facade.pendingSettings();
        if (chain.mode == engine::ChainMode::Node && !incomplete) {
            draftGlobals.apply(s);
            // A detached singleton owns editable values, not live resources.
            if (!projected->firstOf(engine::EffectType::SuperResolution)) {
                s.srTarget = previous.srTarget; s.videoSrQuality = previous.videoSrQuality;
                s.dlssSrModel = previous.dlssSrModel;
                s.srPrescale = previous.srPrescale; s.srOutput = previous.srOutput;
                s.dualSrEnabled=previous.dualSrEnabled;s.dualSrTarget=previous.dualSrTarget;s.dualSrQuality=previous.dualSrQuality;
            }
            if (!projected->firstOf(engine::EffectType::FrameGeneration)){
                s.frameGenerationBackend = previous.frameGenerationBackend;
                s.vfgQuality = previous.vfgQuality;
            }
        }
        engine::fromChain(*projected, s);
        s.content=engine::currentContentRate(s.content);
        if(const auto error=s.validate();!error.empty()){
            oviora::log::error("qml-settings",error);
            validation={false,"效果参数无效，未提交；详见诊断日志"};return false;
        }
        const auto order = engine::runtimeOrder(*projected);
        const bool changed = s != previous || order != engine::runtimeOrder(acceptedChain);
        if (apply && changed) {
            if (const auto reason = unavailableEffects(s); !reason.isEmpty()) {
                validation = {false, "当前硬件或包不支持所选效果，请查看组件页"};
                oviora::log::warn("effect-availability", reason.toStdString());
                return false;
            }
        }
        if(apply && changed && !facade.applySettings(s,order)){
            facade.setPending(previous);
            validation={false,"引擎未接受设置，已保留原状态"};
            return false;
        }
        if(!apply || !changed)facade.setPending(s);
        acceptedChain = *projected;
        options = engine::PlayerOptions::from(s,order);
        return true;
    }
};


uint32_t primaryGpuVendor();
bool fsr4Possible();
const DXGI_ADAPTER_DESC1* engineAdapterDesc();
bool vfgHardwarePossible();
unsigned normalizeFgMultiplier(unsigned value, engine::FrameGenerationBackend backend, unsigned cap);
QString fgEffectId(engine::FrameGenerationBackend backend);
int xessCeiling(const engine::PlayerSnapshot& snapshot);
void ensureGdiplus();

} // namespace oviora::ui
