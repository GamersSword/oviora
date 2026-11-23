#pragma once
// The QML front end's only view of the player.
//
// Design rules this file follows, all of them deliberate:
//
//   * QML never sees EngineController, D3D12, decoded frames or the audio
//     clock. It sees plain values and calls plain commands.
//   * The video is NOT part of the QML scene graph. It stays a native child
//     HWND with its own flip-model swapchain (measured in S3: 499/499 presents
//     at 0.205 ms while the QML scene animated at full panel rate). QML is drawn
//     around and above it. Compositing video into QML would cost exactly the
//     latency that is this product's advantage over OBS and PotPlayer.
//   * One timer drives updates, and it publishes only what changed: the facade
//     hands out a revision counter, so a 60 Hz tick does not mean 60 property
//     writes. This is what keeps the UI from becoming the latency bottleneck.
//
// Threading: every method here is called on the UI thread. Commands go through
// the facade, never executed inline, so a slow engine cannot block a frame of
// QML animation.
//
// Scope note: this exposes what the engine can actually do today. Playback
// speed measurement and requested file-preview rate are separate values.
#include <QObject>
#include <QString>
#include <QRectF>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "oviora/engine/EffectChain.h"
#include "oviora/engine/EnhancementSettings.h"
#include "oviora/ui/PlayerUiFacade.h"

class QQmlEngine;
class QWindow;
class QJsonArray;
class QJsonObject;
struct tagRAWMOUSE;
namespace oviora::remoteplay { struct ControllerState; struct WatchRoute; }
namespace oviora::ui {


class QmlPlayerBridge : public QObject {
    Q_OBJECT
    // --- professional-page readouts ---------------------------------------
    // Every one of these reports an engine value. Where the engine has no
    // measurement the getter says so. There is no "display fps": panel scanout
    // is not measured, and overlays such as RTSS also count Present calls, which
    // is the submit rate below (with its 1% low).
    Q_PROPERTY(QString outputSummary READ outputSummary NOTIFY snapshotChanged)
    Q_PROPERTY(QString srStageSummary READ srStageSummary NOTIFY snapshotChanged)
    Q_PROPERTY(QString srStageNote READ srStageNote NOTIFY snapshotChanged)
    Q_PROPERTY(bool fullscreenMemorySafe READ fullscreenMemorySafe NOTIFY snapshotChanged)
    Q_PROPERTY(double queuedFrames READ queuedFrames NOTIFY snapshotChanged)
    Q_PROPERTY(bool queuedFramesKnown READ queuedFramesKnown NOTIFY snapshotChanged)
    Q_PROPERTY(int skippedFrames READ skippedFrames NOTIFY snapshotChanged)
    Q_PROPERTY(QString flowBackend READ flowBackend NOTIFY snapshotChanged)
    Q_PROPERTY(int opticalFlowChoice READ opticalFlowChoice NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList stageTimings READ stageTimings NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantMap nodeTimings READ nodeTimings NOTIFY snapshotChanged)
    Q_PROPERTY(double stageBudgetMs READ stageBudgetMs NOTIFY snapshotChanged)
    // Existing measured enhancement-interval P95, NOT full-chain latency.
    Q_PROPERTY(double chainTotalMs READ chainTotalMs NOTIFY snapshotChanged)
    Q_PROPERTY(bool chainTotalMsKnown READ chainTotalMsKnown NOTIFY snapshotChanged)
    Q_PROPERTY(double scheduleP95Ms READ scheduleP95Ms NOTIFY snapshotChanged)

    // --- frame generation settings ----------------------------------------
    Q_PROPERTY(QString fgBackendName READ fgBackendName WRITE setFgBackendName NOTIFY settingsChanged)
    Q_PROPERTY(int vfgQuality READ vfgQuality WRITE setVfgQuality NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList fgBackendChoices READ fgBackendChoices NOTIFY effectCapabilitiesChanged)
    Q_PROPERTY(QString fgProviderText READ fgProviderText NOTIFY fgChoicesChanged)
    Q_PROPERTY(int fgMaxMultiplier READ fgMaxMultiplier NOTIFY snapshotChanged)
    Q_PROPERTY(bool fgEnabled READ fgEnabled WRITE setFgEnabled NOTIFY chainChanged)
    Q_PROPERTY(QString submitFpsLabel READ submitFpsLabel NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantList fgMultiplierChoices READ fgMultiplierChoices NOTIFY fgChoicesChanged)
    Q_PROPERTY(bool fgStrict READ fgStrict WRITE setFgStrict NOTIFY settingsChanged)
    Q_PROPERTY(bool fgLowQueue READ fgLowQueue WRITE setFgLowQueue NOTIFY settingsChanged)
    Q_PROPERTY(int nvidiaLowLatency READ nvidiaLowLatency WRITE setNvidiaLowLatency NOTIFY presentationChanged)
    Q_PROPERTY(bool nvidiaLowLatencyAvailable READ nvidiaLowLatencyAvailable NOTIFY effectCapabilitiesChanged)

    // --- colour settings ---------------------------------------------------
    Q_PROPERTY(double colorExposure READ colorExposure WRITE setColorExposure NOTIFY settingsChanged)
    Q_PROPERTY(double colorContrast READ colorContrast WRITE setColorContrast NOTIFY settingsChanged)
    Q_PROPERTY(double colorSaturation READ colorSaturation WRITE setColorSaturation NOTIFY settingsChanged)
    Q_PROPERTY(double colorTemperature READ colorTemperature WRITE setColorTemperature NOTIFY settingsChanged)

    // --- shell preferences -------------------------------------------------
    // Kept by the facade so they survive a restart, and read by the shell.
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion NOTIFY settingsChanged)
    // Counts interface graphics devices recreated after a loss (a GPU reset, logs 56). VIcon
    // re-feeds its path when it changes: Qt 6.8.3's curve renderer leaves recreated nodes empty.
    Q_PROPERTY(int uiDeviceGeneration READ uiDeviceGeneration NOTIFY uiDeviceGenerationChanged)
    Q_PROPERTY(QString defaultPage READ defaultPage WRITE setDefaultPage NOTIFY settingsChanged)
    Q_PROPERTY(QString defaultPageLabel READ defaultPageLabel NOTIFY settingsChanged)
    // Used by the shell to open on the saved page. Set from the command line in
    // tests so a screenshot does not depend on clicking the dock.
    Q_PROPERTY(QString initialPage READ initialPage WRITE setInitialPage NOTIFY settingsChanged)
    // The page the shell is showing, pushed by the shell so a command can tell
    // where the user is: opening a file from home goes to the player, and
    // opening one from the professional page stays there.
    Q_PROPERTY(QString currentPage READ currentPage WRITE setCurrentPage NOTIFY settingsChanged)

    // --- settings-page reporting -------------------------------------------
    Q_PROPERTY(QString remotePlayState READ remotePlayState NOTIFY snapshotChanged)
    // The loaded runtime components, as the engine reports them. A hand-written
    // list here could drift from what the package actually contains.
    Q_PROPERTY(QVariantList componentList READ componentList NOTIFY componentListChanged)

    Q_PROPERTY(bool captureForceSdr READ captureForceSdr WRITE setCaptureForceSdr NOTIFY settingsChanged)
    Q_PROPERTY(bool amdNrGpu READ amdNrGpu CONSTANT)
    Q_PROPERTY(QVariantMap effectCapabilities READ effectCapabilities NOTIFY effectCapabilitiesChanged)
    Q_PROPERTY(QVariantList nrRuntimeChoices READ nrRuntimeChoices NOTIFY effectCapabilitiesChanged)
    Q_PROPERTY(QVariantList srBackendChoices READ srBackendChoices NOTIFY effectCapabilitiesChanged)


    // Subtitles (P4-c): the engine's SubtitleLoader reads the embedded tracks and
    // the same-name external file; the old shell's SubtitleOverlay draws them as a
    // layered child of the native video window, so the picture's window region
    // also clips the subtitles away from QML popups. Style lives in preferences.
    Q_PROPERTY(QVariantList subtitleTracks READ subtitleTracks NOTIFY subtitlesChanged)
    Q_PROPERTY(int subtitlePrimary READ subtitlePrimary WRITE setSubtitlePrimary NOTIFY subtitlesChanged)
    Q_PROPERTY(int subtitleSecondary READ subtitleSecondary WRITE setSubtitleSecondary NOTIFY subtitlesChanged)
    Q_PROPERTY(int subtitleOffsetMs READ subtitleOffsetMs WRITE setSubtitleOffsetMs NOTIFY subtitlesChanged)
    Q_PROPERTY(QString subtitleStatus READ subtitleStatus NOTIFY subtitlesChanged)
    Q_PROPERTY(bool subtitleAligning READ subtitleAligning NOTIFY subtitlesChanged)
    // The text the overlay is drawing now (joined lines), for tests and the a11y name.
    Q_PROPERTY(QString subtitleText READ subtitleText NOTIFY subtitleTextChanged)
    // Extra space the shell keeps free at the bottom (fullscreen controls).
    Q_PROPERTY(int subtitleBottomInset READ subtitleBottomInset WRITE setSubtitleBottomInset NOTIFY subtitlesChanged)

    // --- ps5 dialog --------------------------------------------------------
    Q_PROPERTY(QString remotePlayHost READ remotePlayHost WRITE setRemotePlayHost NOTIFY settingsChanged)
    Q_PROPERTY(QString remotePlayPin READ remotePlayPin WRITE setRemotePlayPin NOTIFY settingsChanged)
    // P4-e PS5 Remote Play, ported from the 1.4.4 panel: saved pairings (shared
    // %LOCALAPPDATA%\Oviora\remoteplay, encrypted), discovery, pairing, Account ID lookup,
    // wake, stream profile, decode/sampling, view-only, login PIN, controller
    // forwarding and gyro calibration. `ps5` carries the form state.
    Q_PROPERTY(QVariantMap ps5 READ ps5 NOTIFY ps5Changed)
    Q_PROPERTY(QVariantList ps5Profiles READ ps5Profiles NOTIFY ps5Changed)
    // Oviora home "Bu ağda": paired PS5s and PS5s found on this network, searched every few seconds.
    // Each: key, name, address, state (ready/standby/""), discovered, registered, app, display, duid, manual.
    Q_PROPERTY(QVariantList lanConsoles READ lanConsoles NOTIFY lanChanged)
    Q_PROPERTY(bool lanSearch READ lanSearch WRITE setLanSearch NOTIFY lanChanged)
    // Consoles hidden from the home's "Bu ağda" list (Konsollar → Gizlenen konsollar): [{id, name}].
    Q_PROPERTY(QVariantList hiddenConsoles READ hiddenConsoles NOTIFY lanChanged)
    // The running PS5 stream as the old Oviora's statistics showed it (bridge/StreamSession.cpp):
    // active, width, height, fps, mbps, requestedMbps, packetLoss (0..1), rttMs (-1 unknown),
    // framesLost, skipped, linkReady. Empty values while no PS5 stream runs.
    Q_PROPERTY(QVariantMap streamStats READ streamStats NOTIFY snapshotChanged)
    // The statistics panel drawn over the picture (a layered child of the video window, so it can
    // be see-through): where it is, in the main window's logical pixels; empty when hidden.
    Q_PROPERTY(QRectF statsOverlayRect READ statsOverlayRect NOTIFY statsOverlayChanged)
    // The microphone to the PS5: on for this stream (the menu tile), and the recording devices
    // (Ses ve ağ → Mikrofon; preferences.micDevice holds the chosen name, "" = Windows' default).
    Q_PROPERTY(bool micOn READ micOn NOTIFY micChanged)
    Q_PROPERTY(QStringList microphones READ microphones NOTIFY audioDevicesChanged)
    // Game input (bridge/GameInput.cpp): the shell says when keys may go to the PS5 (the
    // picture is shown, no menu or dialog over it); the buttons held on the pad right now
    // (ControllerState bits, before the button mapping) while a stream or padCapture reads it.
    Q_PROPERTY(bool gameKeysAllowed READ gameKeysAllowed WRITE setGameKeysAllowed NOTIFY gameKeysAllowedChanged)
    Q_PROPERTY(int padPressed READ padPressed NOTIFY padPressedChanged)
    // Mouse aim (bridge/MouseAim.cpp): the pointer is captured and turns the right stick;
    // mouseAimOn: the setting is on (and Klavye ve fare), whatever the capture.
    Q_PROPERTY(bool mouseAimCaptured READ mouseAimCaptured NOTIFY mouseAimChanged)
    Q_PROPERTY(bool mouseAimOn READ mouseAimOn NOTIFY mouseAimChanged)

    // Shell preferences that are not enhancement settings: screenshot folder,
    // subtitle look, audio output. Stored in <data>/qml-preferences.v1.json and
    // validated key by key in setPreference(); anything unknown is refused.
    Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY preferencesChanged)
    Q_PROPERTY(QString gpuPriorityStatus READ gpuPriorityStatus NOTIFY preferencesChanged)
    // The interface language in use (zh-CN / zh-TW / en / ja), after "auto" is resolved;
    // preferences.language holds the choice itself.
    Q_PROPERTY(QString uiLanguage READ uiLanguage NOTIFY preferencesChanged)
    Q_PROPERTY(QString screenshotDirectory READ screenshotDirectory NOTIFY preferencesChanged)
    Q_PROPERTY(QString lastScreenshot READ lastScreenshot NOTIFY preferencesChanged)
    // The Ctrl+O drawer's ground: a file URL of the frame taken when it opened ("" while none)
    Q_PROPERTY(QString menuBackdrop READ menuBackdrop NOTIFY menuBackdropChanged)
    Q_PROPERTY(QString dataDirectory READ dataDirectory CONSTANT)
    // Rebindable shortcuts: action -> key sequence text (defaults merged in).
    Q_PROPERTY(QVariantMap shortcuts READ shortcuts NOTIFY preferencesChanged)
    // The interface scale chosen for the next start (0 = follow Windows).
    Q_PROPERTY(int uiScaleActive READ uiScaleActive CONSTANT)
    Q_PROPERTY(bool obsGameCaptureActive READ obsGameCaptureActive CONSTANT)
    // This run draws the interface in software because RivaTuner (RTSS) was running at start
    // (设置 → 监控软件兼容, main.cpp): its OSD then sits on the video only.
    Q_PROPERTY(bool overlayCompatActive READ overlayCompatActive CONSTANT)
    // This run draws the interface in software: 设置 → 界面绘制 (software unless GPU is
    // chosen) or one of the compatibility rules above (oviora/ui/InterfaceRenderer.h).
    Q_PROPERTY(bool softwareUiActive READ softwareUiActive CONSTANT)
    // Audio output (P4-d): the active WASAPI render endpoints by device id, the
    // stored choice (preferences.audioDevice, empty = system default) and what
    // the renderer actually opened, including a fallback to the default.
    Q_PROPERTY(QVariantList audioDevices READ audioDevices NOTIFY audioDevicesChanged)
    // The GPU the "GPU 占用" orb measures (设置 → 通用与外观): [{id, label}], the first
    // entry ("", 自动) picks the high-performance (discrete) GPU. Field report
    // 2026-10-02: the orb used to add the integrated and the discrete GPU together.
    Q_PROPERTY(QVariantList gpuMonitorChoices READ gpuMonitorChoices CONSTANT)
    Q_PROPERTY(QString gpuMonitorName READ gpuMonitorName NOTIFY preferencesChanged)
    // 设置 -> "同时用这块显卡运算" (preference computeOnMonitorGpu): the GPU picked
    // for monitoring also runs enhancement, frame generation and export.
    Q_PROPERTY(QString computeGpuHint READ computeGpuHint NOTIFY preferencesChanged)
    Q_PROPERTY(QString audioOutputStatus READ audioOutputStatus NOTIFY audioDevicesChanged)
    Q_PROPERTY(bool audioOutputFallback READ audioOutputFallback NOTIFY audioDevicesChanged)
    Q_PROPERTY(QString logFile READ logFile CONSTANT)

    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)

    // --- playback ----------------------------------------------------------
    Q_PROPERTY(QString statusText READ statusText NOTIFY snapshotChanged)
    Q_PROPERTY(bool running READ running NOTIFY snapshotChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY snapshotChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY snapshotChanged)
    Q_PROPERTY(bool isImage READ isImage NOTIFY snapshotChanged)
    Q_PROPERTY(bool isCapture READ isCapture NOTIFY snapshotChanged)
    Q_PROPERTY(bool hasSource READ hasSource NOTIFY snapshotChanged)
    Q_PROPERTY(double position READ position NOTIFY snapshotChanged)
    Q_PROPERTY(double duration READ duration NOTIFY snapshotChanged)
    Q_PROPERTY(double progress READ progress NOTIFY snapshotChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY snapshotChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY snapshotChanged)

    // --- source ------------------------------------------------------------
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY snapshotChanged)
    Q_PROPERTY(QString sourceSummary READ sourceSummary NOTIFY snapshotChanged)
    Q_PROPERTY(int sourceWidth READ sourceWidth NOTIFY snapshotChanged)
    Q_PROPERTY(int sourceHeight READ sourceHeight NOTIFY snapshotChanged)
    Q_PROPERTY(double sourceFps READ sourceFps NOTIFY snapshotChanged)
    Q_PROPERTY(int sourceRotation READ sourceRotation NOTIFY snapshotChanged)
    Q_PROPERTY(int previewRotation READ previewRotation NOTIFY compareChanged)
    // The film's display aspect, so the window can snap to it in cinema mode
    // exactly as the prototype's fitAspect() does. 0 when nothing is open.
    Q_PROPERTY(double sourceAspect READ sourceAspect NOTIFY snapshotChanged)
    Q_PROPERTY(double previewAspect READ previewAspect NOTIFY compareChanged)

    // --- live performance --------------------------------------------------
    // Submit FPS only. There is deliberately no "display FPS": the user asked
    // for it to be left out (MSI Afterburner reads the real panel rate, and a
    // number we cannot measure must not be shown as if we measured it).
    Q_PROPERTY(double submitFps READ submitFps NOTIFY snapshotChanged)
    Q_PROPERTY(bool submitFpsKnown READ submitFpsKnown NOTIFY snapshotChanged)
    // 1% low of the presents over the last ten seconds (FramePaceWindow).
    Q_PROPERTY(double submitLowFps READ submitLowFps NOTIFY snapshotChanged)
    Q_PROPERTY(bool submitLowFpsKnown READ submitLowFpsKnown NOTIFY snapshotChanged)
    Q_PROPERTY(double lateMs READ lateMs NOTIFY snapshotChanged)
    Q_PROPERTY(double lateP95Ms READ lateP95Ms NOTIFY snapshotChanged)
    Q_PROPERTY(QString metricsSummary READ metricsSummary NOTIFY snapshotChanged)
    Q_PROPERTY(bool nrActive READ nrActive NOTIFY snapshotChanged)
    Q_PROPERTY(bool nrAutoActive READ nrAutoActive NOTIFY snapshotChanged)
    Q_PROPERTY(int nrAutoPercent READ nrAutoPercent NOTIFY snapshotChanged)
    Q_PROPERTY(QString nrAutoStatus READ nrAutoStatus NOTIFY snapshotChanged)
    Q_PROPERTY(bool srActive READ srActive NOTIFY snapshotChanged)
    Q_PROPERTY(bool fgActive READ fgActive NOTIFY snapshotChanged)
    Q_PROPERTY(double captureFps READ captureFps NOTIFY snapshotChanged)
    Q_PROPERTY(double captureDropped READ captureDropped NOTIFY snapshotChanged)
    Q_PROPERTY(QString backendWarning READ backendWarning NOTIFY snapshotChanged)

    // --- settings (what the UI shows) --------------------------------------
    Q_PROPERTY(bool nrEnabled READ nrEnabled WRITE setNrEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int selectedNrLayer READ selectedNrLayer WRITE setSelectedNrLayer NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList nrLayers READ nrLayers NOTIFY nrLayersChanged)
    // List mode's NR master switch: true while any NR layer is on.
    Q_PROPERTY(bool nrAnyEnabled READ nrAnyEnabled NOTIFY nrLayersChanged)
    Q_PROPERTY(bool srEnabled READ srEnabled WRITE setSrEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int fgMultiplier READ fgMultiplier WRITE setFgMultiplier NOTIFY settingsChanged)
    Q_PROPERTY(bool lowLatency READ lowLatency WRITE setLowLatency NOTIFY settingsChanged)
    Q_PROPERTY(bool nrTemporal READ nrTemporal WRITE setNrTemporal NOTIFY settingsChanged)
    Q_PROPERTY(double nrHoldStrength READ nrHoldStrength WRITE setNrHoldStrength NOTIFY settingsChanged)
    Q_PROPERTY(int nrReuseMode READ nrReuseMode WRITE setNrReuseMode NOTIFY settingsChanged)
    Q_PROPERTY(bool nrReuseActive READ nrReuseActive NOTIFY snapshotChanged)
    Q_PROPERTY(QString nrReuseStatus READ nrReuseStatus NOTIFY snapshotChanged)
    Q_PROPERTY(double nrHoldTolerance READ nrHoldTolerance WRITE setNrHoldTolerance NOTIFY settingsChanged)
    Q_PROPERTY(bool videoHdr READ videoHdr WRITE setVideoHdr NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap videoHdrParams READ videoHdrParams NOTIFY videoHdrParamsChanged)
    // Spatial anti-aliasing (#53): the list card edits the first instance, created
    // on first use; node cards edit their own node by chain index.
    Q_PROPERTY(bool antiAliasEnabled READ antiAliasEnabled WRITE setAntiAliasEnabled NOTIFY antiAliasChanged)
    Q_PROPERTY(QVariantMap antiAliasParams READ antiAliasParams NOTIFY antiAliasChanged)
    // Custom: static HDR output tuning (globals, every field live).
    Q_PROPERTY(bool hdrCurve READ hdrCurve WRITE setHdrCurve NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap hdrCurveParams READ hdrCurveParams NOTIFY settingsChanged)
    Q_PROPERTY(bool hdrMetadataEnabled READ hdrMetadataEnabled WRITE setHdrMetadataEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap hdrMetadataParams READ hdrMetadataParams NOTIFY settingsChanged)
    // One-click tuning presets (see hdrTuningPresets); "custom" when the sliders
    // no longer match any of them.
    Q_PROPERTY(QString hdrTuningPreset READ hdrTuningPreset NOTIFY settingsChanged)
    // One dial for the whole curve (0..200%, 100 = the preset as written) and the
    // display's own peak brightness (0 = ask the system). Both live next to the
    // baseline, so moving them never leaves the selected preset.
    Q_PROPERTY(int hdrCurveStrength READ hdrCurveStrength WRITE setHdrCurveStrength NOTIFY settingsChanged)
    Q_PROPERTY(int hdrDisplayPeakNits READ hdrDisplayPeakNits WRITE setHdrDisplayPeakNits NOTIFY settingsChanged)
    // What the system reports about the display, for the peak field's reset button.
    Q_PROPERTY(QVariantMap hdrDisplayInfo READ hdrDisplayInfo NOTIFY snapshotChanged)
    // Custom: the source's own HDR numbers (container metadata), the one-click
    // recommendation derived from them, and the warning that compares the film's
    // peak with what the curve will actually cap.
    Q_PROPERTY(QVariantMap hdrSourceInfo READ hdrSourceInfo NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantMap hdrRecommendation READ hdrRecommendation NOTIFY snapshotChanged)
    // Custom: whether the running source is SDR. Used only to word the recommendation
    // row (an SDR source has no HDR metadata, so "no metadata" would be misleading).
    Q_PROPERTY(bool sourceIsSdr READ sourceIsSdr NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantMap hdrPeakWarning READ hdrPeakWarning NOTIFY snapshotChanged)
    // Custom: one remembered curve per display device, so moving the window to the
    // projector or the TV applies the settings tuned on that device.
    Q_PROPERTY(int hdrDisplayProfileCount READ hdrDisplayProfileCount NOTIFY preferencesChanged)
    // Custom: which preset system the panel shows. "legacy" is the original seven
    // presets, "axes" is content x environment. A UI choice kept in preferences:
    // both modes write the very same curve, so the saved session is unaffected.
    Q_PROPERTY(QString hdrPresetMode READ hdrPresetMode WRITE setHdrPresetMode NOTIFY preferencesChanged)

    // Which preset system established the curve in force. Kept apart from hdrPresetMode
    // on purpose: switching the panel view must not let the other system claim the curve,
    // so only an actual apply moves this.
    Q_PROPERTY(QString hdrAppliedFrom READ hdrAppliedFrom WRITE setHdrAppliedFrom NOTIFY preferencesChanged)
    Q_PROPERTY(QVariantMap hdrAxes READ hdrAxes NOTIFY preferencesChanged)
    Q_PROPERTY(QVariantMap hdrAxesMatch READ hdrAxesMatch NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap hdrDials READ hdrDials NOTIFY settingsChanged)
    // Custom: the curve sampled by the same function the shader runs, for the panel
    // graph (see hdrCurveLuminance).
    Q_PROPERTY(QVariantMap hdrCurveGraph READ hdrCurveGraph NOTIFY settingsChanged)
    // Custom: the live coverage measured by the presenter's probe (see the panel's
    // live meter). Invalid until a probe has completed.
    Q_PROPERTY(QVariantMap hdrCurveCoverage READ hdrCurveCoverage NOTIFY snapshotChanged)
    Q_PROPERTY(bool hdrTitleMemory READ hdrTitleMemory WRITE setHdrTitleMemory NOTIFY preferencesChanged)
    Q_PROPERTY(bool hdrAutoRecommend READ hdrAutoRecommend WRITE setHdrAutoRecommend NOTIFY preferencesChanged)
    Q_PROPERTY(int hdrTitleCount READ hdrTitleCount NOTIFY preferencesChanged)
    Q_PROPERTY(int nrStyle READ nrStyle WRITE setNrStyle NOTIFY settingsChanged)
    Q_PROPERTY(double nrIntensity READ nrIntensity WRITE setNrIntensity NOTIFY settingsChanged)
    Q_PROPERTY(double nrTone READ nrTone WRITE setNrTone NOTIFY settingsChanged)
    Q_PROPERTY(double nrStructure READ nrStructure WRITE setNrStructure NOTIFY settingsChanged)
    Q_PROPERTY(double nrSkin READ nrSkin WRITE setNrSkin NOTIFY settingsChanged)
    Q_PROPERTY(bool nrAutoMask READ nrAutoMask WRITE setNrAutoMask NOTIFY settingsChanged)
    Q_PROPERTY(bool nrUiCorrection READ nrUiCorrection WRITE setNrUiCorrection NOTIFY settingsChanged)
    Q_PROPERTY(int videoSrQuality READ videoSrQuality WRITE setVideoSrQuality NOTIFY settingsChanged)
    Q_PROPERTY(int dlssSrModel READ dlssSrModel WRITE setDlssSrModel NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList dlssSrModelChoices READ dlssSrModelChoices CONSTANT)
    // Three-step SR (RTX Video SR). srPrescale: 0 off, 1..4 720p/900p/1080p/1440p,
    // 150..190 a manual 50..90 %. srOutput: 0 keep the SR result, 1 source size, 2..6 2K/4K/5K/6K/7K.
    Q_PROPERTY(int srPrescale READ srPrescale WRITE setSrPrescale NOTIFY settingsChanged)
    Q_PROPERTY(bool dualSrEnabled READ dualSrEnabled WRITE setDualSrEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int dualSrTargetIndex READ dualSrTargetIndex WRITE setDualSrTargetIndex NOTIFY settingsChanged)
    Q_PROPERTY(int dualSrQuality READ dualSrQuality WRITE setDualSrQuality NOTIFY settingsChanged)
    Q_PROPERTY(int srOutput READ srOutput WRITE setSrOutput NOTIFY settingsChanged)
    Q_PROPERTY(bool protectionEnabled READ protectionEnabled WRITE setProtectionEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap protectionState READ protectionState NOTIFY protectionChanged)
    Q_PROPERTY(QVariantMap exclusionState READ exclusionState NOTIFY protectionChanged)
    Q_PROPERTY(QVariantMap nrStabilizerState READ nrStabilizerState NOTIFY nrStabilizerChanged)
    Q_PROPERTY(bool colorEnabled READ colorEnabled WRITE setColorEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int audioOffsetMs READ audioOffsetMs WRITE setAudioOffsetMs NOTIFY settingsChanged)
    // 1.4.4 声音补偿: 0 自动(软件估算) / 1 手动 / 2 关闭; live sources only.
    Q_PROPERTY(int audioSyncMode READ audioSyncMode WRITE setAudioSyncMode NOTIFY settingsChanged)
    Q_PROPERTY(bool audioSyncLive READ audioSyncLive NOTIFY snapshotChanged)
    Q_PROPERTY(double audioCompensationMs READ audioCompensationMs NOTIFY snapshotChanged)
    // Performance orbs and the status light (1.4.4 live status dashboard).
    Q_PROPERTY(double gpuUtilization READ gpuUtilization NOTIFY perfChanged)
    Q_PROPERTY(bool gpuUtilizationKnown READ gpuUtilizationKnown NOTIFY perfChanged)
    Q_PROPERTY(QString runStatus READ runStatus NOTIFY perfChanged)
    Q_PROPERTY(QString runStatusLevel READ runStatusLevel NOTIFY perfChanged)
    Q_PROPERTY(QString runStatusDetail READ runStatusDetail NOTIFY perfChanged)
    // Live FG output hint ("当前帧率…超出屏幕刷新率，建议降至…"), empty when the
    // output fits; a suggestion only, the multiplier is never changed.
    Q_PROPERTY(QString fgDisplayHint READ fgDisplayHint NOTIFY perfChanged)
    Q_PROPERTY(QStringList runOptions READ runOptions NOTIFY perfChanged)
    Q_PROPERTY(double effectiveOutputFps READ effectiveOutputFps NOTIFY perfChanged)
    // Achieved output rate / effective policy target; -1 while unknown.
    Q_PROPERTY(double outputRateRatio READ outputRateRatio NOTIFY perfChanged)
    // Frame interval history (ms, newest last) for the frame-time line.
    Q_PROPERTY(QVariantList frameTimes READ frameTimes NOTIFY perfChanged)
    // 补帧 / 光流 (design gap A1-A5): motion estimation quality 0 性能 1 平衡 2 质量,
    // AMD half-resolution flow, content cadence (engine::ContentRate order).
    Q_PROPERTY(int flowQuality READ flowQuality WRITE setFlowQuality NOTIFY settingsChanged)
    Q_PROPERTY(bool amdFlowHalf READ amdFlowHalf WRITE setAmdFlowHalf NOTIFY settingsChanged)
    // 边缘神盾: refined optical-flow motion for XeSS (EnhancementSettings::fgMotionRefine).
    Q_PROPERTY(bool edgeAegis READ edgeAegis WRITE setEdgeAegis NOTIFY settingsChanged)
    // DLSS 性能模式: NVIDIA's DLSS-G performance mode (EnhancementSettings::dlssgLite);
    // available when the package carries its runtime (runtime/experimental/dlssg-lite).
    Q_PROPERTY(bool dlssgLite READ dlssgLite WRITE setDlssgLite NOTIFY settingsChanged)
    // VFG 自动调控: EnhancementSettings::vfgAutoRegulate.
    Q_PROPERTY(bool vfgAutoRegulate READ vfgAutoRegulate WRITE setVfgAutoRegulate NOTIFY settingsChanged)
    Q_PROPERTY(bool dlssgLiteAvailable READ dlssgLiteAvailable CONSTANT)
    Q_PROPERTY(QString dlssgLiteHint READ dlssgLiteHint NOTIFY snapshotChanged)
    Q_PROPERTY(int contentRate READ contentRate WRITE setContentRate NOTIFY settingsChanged)
    Q_PROPERTY(int hdrOutputMode READ hdrOutputMode WRITE setHdrOutputMode NOTIFY settingsChanged)
    Q_PROPERTY(int fgMotionSource READ fgMotionSource WRITE setFgMotionSource NOTIFY settingsChanged)
    Q_PROPERTY(int srMotionSource READ srMotionSource WRITE setSrMotionSource NOTIFY settingsChanged)
    Q_PROPERTY(int nrMotionSource READ nrMotionSource WRITE setNrMotionSource NOTIFY settingsChanged)
    // Presentation: display sync 0 允许撕裂 1 垂直同步 2 自动; output cap 0 关闭
    // 1 跟随显示器 2 自定义 (+ fps). Owned by XeSS when presentationOwned.
    Q_PROPERTY(int displaySync READ displaySync WRITE setDisplaySync NOTIFY presentationChanged)
    // 可变刷新 switch (persisted with the presentation settings) and the
    // presenting monitor's VRR level: 0 unknown, 1 the monitor cannot do VRR
    // (switch greyed), 2 it can but the driver setting is off, 3 possible,
    // 4 active; vrrHint explains it beside the switch.
    Q_PROPERTY(bool variableRefresh READ variableRefresh WRITE setVariableRefresh NOTIFY presentationChanged)
    Q_PROPERTY(int vrrState READ vrrState NOTIFY perfChanged)
    Q_PROPERTY(QString vrrHint READ vrrHint NOTIFY perfChanged)
    // Display state of the shown monitor for the 信号 card: Windows HDR (-1
    // unknown, 0 off, 1 on), refresh rate (0 unknown) and a short VRR summary.
    // Everything is read on events only (startup, another monitor, a display
    // change, the switch turned on); 刷新显示器状态 (recheckDisplay) asks the
    // driver again, and displayChecking is true until that answer arrives.
    Q_PROPERTY(int displayHdr READ displayHdr NOTIFY perfChanged)
    Q_PROPERTY(double displayRefreshHz READ displayRefreshHz NOTIFY perfChanged)
    Q_PROPERTY(QString vrrSummary READ vrrSummary NOTIFY perfChanged)
    Q_PROPERTY(bool displayChecking READ displayChecking NOTIFY perfChanged)
    Q_PROPERTY(int outputRateMode READ outputRateMode WRITE setOutputRateMode NOTIFY presentationChanged)
    Q_PROPERTY(double outputCustomFps READ outputCustomFps WRITE setOutputCustomFps NOTIFY presentationChanged)
    Q_PROPERTY(bool presentationOwned READ presentationOwned NOTIFY snapshotChanged)
    Q_PROPERTY(QString presentationStatus READ presentationStatus NOTIFY snapshotChanged)
    // 显示: compare 0 增强 1 原画 2 分屏; base false 输入原画 / true 增强前底图;
    // split 0..1 of the picture. Aspect 0 适应 1 原始 2 填充.
    Q_PROPERTY(int compareMode READ compareMode WRITE setCompareMode NOTIFY compareChanged)
    Q_PROPERTY(bool compareBase READ compareBase WRITE setCompareBase NOTIFY compareChanged)
    Q_PROPERTY(double compareSplit READ compareSplit NOTIFY compareChanged)
    Q_PROPERTY(int aspectMode READ aspectMode WRITE setAspectMode NOTIFY compareChanged)
    Q_PROPERTY(double viewZoom READ viewZoom WRITE setViewZoom NOTIFY compareChanged)
    // Measured audio/video skew of a live source (ms); unknown otherwise.
    Q_PROPERTY(double audioSkewMs READ audioSkewMs NOTIFY snapshotChanged)
    Q_PROPERTY(bool audioSkewKnown READ audioSkewKnown NOTIFY snapshotChanged)
    // P5 (2026-09-29): what is playing, for the page headers.
    // sourceKind: "" | file | image | capture | ps5 | screen.
    Q_PROPERTY(QString sourceKind READ sourceKind NOTIFY snapshotChanged)
    Q_PROPERTY(QString sourceTitle READ sourceTitle NOTIFY snapshotChanged)
    // "1080p60 · YUY2 · SDR": reported values only.
    Q_PROPERTY(QString sourceFormatText READ sourceFormatText NOTIFY snapshotChanged)
    Q_PROPERTY(QString sourceRateText READ sourceRateText NOTIFY snapshotChanged)
    // 调色 undo / redo / copy / paste for the selected colour layer.
    Q_PROPERTY(bool colourCanUndo READ colourCanUndo NOTIFY settingsChanged)
    Q_PROPERTY(bool colourCanRedo READ colourCanRedo NOTIFY settingsChanged)
    Q_PROPERTY(bool colourCanPaste READ colourCanPaste NOTIFY settingsChanged)
    // A live source (PS5 / capture) that is still opening: what the picture area
    // says meanwhile ("" once it plays or when nothing is opening).
    Q_PROPERTY(QString liveOpeningText READ liveOpeningText NOTIFY snapshotChanged)
    // Oviora: the connecting screen and the error card (see RemotePlay.cpp updateConnection)
    Q_PROPERTY(QVariantMap connection READ connection NOTIFY connectionChanged)
    // Protection regions as the engine holds them: [{left,top,right,bottom,ellipse}].
    Q_PROPERTY(QVariantList protectionRegions READ protectionRegions NOTIFY protectionChanged)
    Q_PROPERTY(QString protectionDrawShape READ protectionDrawShape NOTIFY protectionDrawChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY snapshotChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY snapshotChanged)
    // True while the engine has not yet applied what the UI asked for. The UI
    // shows the pending value and marks it, rather than lying about the state.
    Q_PROPERTY(bool applying READ applying NOTIFY snapshotChanged)

    // --- effect chain ------------------------------------------------------
    // The chain as data. List mode renders it; node mode renders the same
    // objects with positions. Both edit one chain, so switching modes never
    // loses work.
    Q_PROPERTY(QVariantList chain READ chain NOTIFY chainChanged)
    Q_PROPERTY(QVariantList effectCatalog READ effectCatalog NOTIFY chainChanged)
    Q_PROPERTY(QString chainError READ chainError NOTIFY chainChanged)
    Q_PROPERTY(bool chainValid READ chainValid NOTIFY chainChanged)

    // --- dialogs the UI opens ----------------------------------------------
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    // The name of the preset currently in effect, or "自定义" when the
    // settings no longer match any stored preset. Derived, never guessed.
    Q_PROPERTY(QString currentPresetName READ currentPresetName NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY snapshotChanged)
    Q_PROPERTY(int selectedAudioTrack READ selectedAudioTrack WRITE setSelectedAudioTrack NOTIFY snapshotChanged)
    Q_PROPERTY(QString colorStatus READ colorStatus NOTIFY snapshotChanged)
    Q_PROPERTY(QString videoHdrStatus READ videoHdrStatus NOTIFY snapshotChanged)

    // The super-resolution target, which is what actually decides the export size
    // in this engine (0 = source, then Qhd/Uhd4K/Uhd8K). A separate "export
    // resolution" field does not exist, so the UI exposes the real control instead
    // of a picker that would change nothing.
    Q_PROPERTY(int srTargetIndex READ srTargetIndex WRITE setSrTargetIndex NOTIFY settingsChanged)
    Q_PROPERTY(QString srTargetLabel READ srTargetLabel NOTIFY settingsChanged)
    // The design's save flow lists what the preset will contain before naming it:
    // each part, its current summary, and whether it is being saved.
    Q_PROPERTY(QVariantList presetSaveParts READ presetSaveParts NOTIFY chainChanged)
    // Which preset the app opens with, as the manage page's "set default" uses.
    Q_PROPERTY(int defaultPresetIndex READ defaultPresetIndex NOTIFY presetsChanged)

    // --- colour page -------------------------------------------------------
    // The engine's ColorSettings carries a large parameter set. Rather than one
    // property per field, the page reads and writes them by name through one pair
    // of calls, and colourParameters() reports the catalogue the UI builds its
    // controls from - so a control can only exist for a parameter that is real.
    Q_PROPERTY(QVariantList colourParameters READ colourParameters NOTIFY colourParamsChanged)
    // The same catalogue grouped by section, so the page can nest its collapsible
    // sub-panels without re-grouping in QML.
    Q_PROPERTY(QVariantList colourGroups READ colourGroups NOTIFY colourParamsChanged)
    Q_PROPERTY(int selectedColourLayer READ selectedColourLayer WRITE setSelectedColourLayer NOTIFY settingsChanged)
    Q_PROPERTY(QVariantMap colourState READ colourState NOTIFY settingsChanged)
    // Imported .cube files (ColorLutStore, the folder the engine resolves) and
    // the saved colour looks (ColorLookStore): a colour preset is one Color
    // node's settings, separate from the whole-chain presets.
    Q_PROPERTY(QVariantList lutLibrary READ lutLibrary NOTIFY colourLibraryChanged)
    Q_PROPERTY(QVariantList colourLooks READ colourLooks NOTIFY colourLibraryChanged)

public:
    bool amdNrGpu() const;
    QVariantMap effectCapabilities() const;
    QVariantList nrRuntimeChoices() const;
    QVariantList srBackendChoices() const;
    // RTX Video SR family / tier helpers for the SR lists (see Enhancement.cpp).
    Q_INVOKABLE QString srAlgorithmOf(int quality) const;
    Q_INVOKABLE int srTierOf(int quality) const;
    Q_INVOKABLE int srQualityFor(const QString& algorithm,int tier) const;
    Q_INVOKABLE QVariantList srTierChoices(int quality) const;
    Q_INVOKABLE QString srQualityLabel(int quality,bool withFamily) const;
    Q_INVOKABLE QVariantMap effectAvailability(const QString& id) const;
    // `engine` must outlive the bridge. `dataDirectory` holds ui-session.v1 and
    // the preset library; empty means the default user data directory.
    QmlPlayerBridge(engine::EngineController& engine, std::filesystem::path dataDirectory = {},
                    QObject* parent = nullptr);
    ~QmlPlayerBridge() override;

    QString outputSummary() const;
    QString srStageSummary() const;
    QString srStageNote() const;
    double queuedFrames() const;
    bool queuedFramesKnown() const;
    int skippedFrames() const;
    QString flowBackend() const;
    int opticalFlowChoice() const;
    Q_INVOKABLE bool setOpticalFlowChoice(int backend);
    QVariantList stageTimings() const;
    QVariantMap nodeTimings() const;
    QVariantMap colourStateAt(int index) const;
    double stageBudgetMs() const;
    double chainTotalMs() const;
    bool chainTotalMsKnown() const;
    double scheduleP95Ms() const;

    QString fgBackendName() const;
    QVariantList fgBackendChoices() const;
    QString fgProviderText() const;
    void setFgBackendName(const QString& value);
    int vfgQuality() const;
    void setVfgQuality(int value);
    int fgMaxMultiplier() const;
    bool fgEnabled() const;
    void setFgEnabled(bool enabled);
    QString submitFpsLabel() const;
    QVariantList fgMultiplierChoices() const;
    bool fgStrict() const;
    void setFgStrict(bool value);
    bool fgLowQueue() const;
    int nvidiaLowLatency() const;
    void setNvidiaLowLatency(int value);
    bool nvidiaLowLatencyAvailable() const;
    void setFgLowQueue(bool value);

    double colorExposure() const;
    void setColorExposure(double v);
    double colorContrast() const;
    void setColorContrast(double v);
    double colorSaturation() const;
    void setColorSaturation(double v);
    double colorTemperature() const;
    void setColorTemperature(double v);

    bool reducedMotion() const;
    int uiDeviceGeneration() const;
    // main.cpp: a window's scene graph came back after its graphics device was lost.
    void noteUiDeviceRecreated(const QString& window);
    void setReducedMotion(bool value);
    QString defaultPage() const;
    void setDefaultPage(const QString& value);
    QString defaultPageLabel() const;
    QString initialPage() const;
    void setInitialPage(const QString& value);
    QString currentPage() const;
    void setCurrentPage(const QString& value);
    QString remotePlayState() const;
    QVariantList componentList() const;
    QVariantList readComponentList() const;
    bool captureForceSdr() const;
    void setCaptureForceSdr(bool value);


    QVariantList subtitleTracks() const;
    int subtitlePrimary() const;
    void setSubtitlePrimary(int index);
    int subtitleSecondary() const;
    void setSubtitleSecondary(int index);
    int subtitleOffsetMs() const;
    void setSubtitleOffsetMs(int ms);
    QString subtitleStatus() const;
    bool subtitleAligning() const;
    QString subtitleText() const;
    int subtitleBottomInset() const;
    void setSubtitleBottomInset(int value);
    Q_INVOKABLE void loadSubtitleDialog();
    Q_INVOKABLE bool loadSubtitleFile(const QString& path);
    Q_INVOKABLE void nudgeSubtitle(int deltaMs);
    Q_INVOKABLE void cycleSubtitle(bool secondary);
    Q_INVOKABLE void toggleSubtitles();
    Q_INVOKABLE void autoAlignSubtitle();

    QVariantMap ps5() const;
    QVariantList ps5Profiles() const;
    QVariantList lanConsoles() const;
    bool lanSearch() const;
    void setLanSearch(bool on);
    // A paired PS5 connects; one not paired yet opens the PS5 dialog with its address.
    Q_INVOKABLE bool lanConnect(const QString& key);
    Q_INVOKABLE void lanWake(const QString& key);
    Q_INVOKABLE void lanForget(const QString& key);
    // Oviora: edit a registered console (address, or a new pairing for another PSN account that replaces it)
    Q_INVOKABLE void lanEdit(const QString& key);
    Q_INVOKABLE bool ps5SaveEdit();
    Q_INVOKABLE void ps5EndEdit();
    Q_INVOKABLE void lanRename(const QString& key, const QString& name);
    Q_INVOKABLE QString lanName(const QString& key) const;
    // Hides a console found on this network from the home (old: hideHost); lanUnhide brings it back.
    Q_INVOKABLE void lanHide(const QString& key);
    Q_INVOKABLE void lanUnhide(const QString& id);
    QVariantList hiddenConsoles() const;
    // Old Oviora's "PS5'i dinlenmeye al": rest mode for the PS5, then the stream ends.
    Q_INVOKABLE void ps5Rest();
    QVariantMap streamStats() const;
    QRectF statsOverlayRect() const;
    // QML says when the panel may show (stream on screen, Ses ve ağ → İstatistikler on)
    Q_INVOKABLE void setStatsOverlayShown(bool shown);
    // Test runs: the drawing window over the picture gets a drag / Esc from this app itself
    Q_INVOKABLE void testMaskDrag(double x0, double y0, double x1, double y1);
    Q_INVOKABLE void testMaskEscape();
    bool micOn() const;
    QStringList microphones() const;
    Q_INVOKABLE void toggleMicrophone();
    Q_INVOKABLE void ps5Load();
    Q_INVOKABLE bool ps5Set(const QString& key, const QVariant& value);
    Q_INVOKABLE void ps5SelectProfile(const QString& id);
    Q_INVOKABLE void ps5Scan();
    Q_INVOKABLE void ps5Pair(const QString& pin);
    Q_INVOKABLE bool ps5Connect();
    // Oviora: a PS5 over the internet (PSN hole punching), leased by the Oviora server.
    // route is a remoteplay::PsnRoute; the video settings are the PS5 dialog's ones.
    bool connectPsn(std::shared_ptr<const void> route, const QString& nickname);
    // Oviora: watch a friend's "İzlet" broadcast inside the native player.
    bool connectWatch(std::shared_ptr<remoteplay::WatchRoute> route, const QString& title);
    // A PS5 session is running or opening.
    bool remotePlayActive() const;
    // Oviora: PS5s paired on the home network are kept in the user's Oviora account. The app hands
    // over the server's call(), which does nothing while signed out.
    using AccountCall = std::function<void(const QString& action, const QJsonObject& body,
                                           std::function<void(const QJsonObject& reply, const QString& error)> done)>;
    void setAccountCall(AccountCall call);
    // At start and at sign-in: the account's consoles missing here become pairings, and the
    // pairings the account lacks are uploaded.
    void syncAccountConsoles();
    Q_INVOKABLE void ps5Wake();
    Q_INVOKABLE void ps5Forget();
    Q_INVOKABLE void ps5OpenAccountChecker();
    Q_INVOKABLE void ps5SendLoginPin(const QString& pin);
    Q_INVOKABLE void ps5Cancel();
    Q_INVOKABLE void ps5Calibrate();
    bool gameKeysAllowed() const;
    void setGameKeysAllowed(bool on);
    int padPressed() const;
    // The button mapping dialog reads the pad while it is open, with or without a stream.
    Q_INVOKABLE void padCapture(bool on);
    // A Qt key's name as the key settings show it (QKeySequence portable text).
    Q_INVOKABLE QString keyName(int key) const;
    bool mouseAimCaptured() const;
    bool mouseAimOn() const;
    // The mouse touchpad (Tuşlar → Fareyle dokunmatik yüzey), from the shell's pointer handler
    // over the picture: phase 0 press, 1 move, 2 release; x, y 0..1 across the picture; button a
    // Qt::MouseButton. Returns true when the PS5 took it.
    Q_INVOKABLE bool mouseTouch(int phase, double x, double y, int button);
    // --test-mouse: raw mouse input as WM_INPUT would bring it (RAWMOUSE fields), and the test
    // mode that never clips or hides the real cursor.
    void testRawMouse(int dx, int dy, unsigned buttonFlags);
    void setMouseAimTest(bool on);
    QString remotePlayHost() const;
    void setRemotePlayHost(const QString& value);
    QString remotePlayPin() const;
    void setRemotePlayPin(const QString& value);

    QVariantMap preferences() const;
    QString gpuPriorityStatus() const;
    QString uiLanguage() const;
    // main.cpp hands over the QML engine so a language change retranslates the scene.
    void setQmlEngine(QQmlEngine* engine);
    Q_INVOKABLE void rememberWindowSize(int width, int height);
    Q_INVOKABLE bool setPreference(const QString& key, const QVariant& value);
    // Available area (taskbar excluded) of the screen holding a point, or of the screen the
    // pointer is on at start. QML's Screen.desktopAvailable* span the whole virtual desktop,
    // which centred the window across two of three monitors (field report 2026-10-01).
    Q_INVOKABLE QRect screenAvailableAt(int x, int y) const;
    Q_INVOKABLE QRect launchScreenAvailable() const;
    QString screenshotDirectory() const;
    QString lastScreenshot() const;
    QString dataDirectory() const;
    QString logFile() const;
    Q_INVOKABLE void chooseScreenshotDirectory();
    QVariantMap shortcuts() const;
    Q_INVOKABLE bool setShortcut(const QString& action, const QString& sequence);
    Q_INVOKABLE void resetShortcuts();
    int uiScaleActive() const;
    bool obsGameCaptureActive() const;
    bool overlayCompatActive() const;
    bool softwareUiActive() const;
    // RivaTuner Statistics Server is running now (gfx::rivaTunerRunning).
    Q_INVOKABLE bool rivaTunerRunning() const;
    // An overlay that 监控软件兼容 reacts to is running now: RTSS, NVIDIA's in-game overlay
    // or GamePP (gfx/PresentationHooks.h).
    Q_INVOKABLE bool presentationOverlayRunning() const;
    Q_INVOKABLE void openFeedbackPage();
    QVariantList audioDevices() const;
    QVariantList gpuMonitorChoices() const;
    QString computeGpuHint() const;
    QString gpuMonitorName() const;
    QString audioOutputStatus() const;
    bool audioOutputFallback() const;
    Q_INVOKABLE void refreshAudioDevices();
    Q_INVOKABLE void openScreenshotDirectory();
    Q_INVOKABLE void openLogFolder();
    Q_INVOKABLE void openDataFolder();
    QString appName() const;
    QString version() const;

    // --- property accessors (Q_PROPERTY READ targets) -----------------------
    // Split into read-only and read-write so a reviewer can see at a glance
    // which values the UI can command and which it can only display.
    QString statusText() const;
    bool running() const;
    bool paused() const;
    bool failed() const;
    bool isImage() const;
    bool isCapture() const;
    bool hasSource() const;
    bool openingSource() const;
    double position() const;
    double duration() const;
    double progress() const;
    QString positionText() const;
    QString durationText() const;

    QString sourceName() const;
    QString sourceSummary() const;
    int sourceWidth() const;
    int sourceHeight() const;
    double sourceFps() const;
    int sourceRotation() const;
    int previewRotation() const;
    Q_INVOKABLE void rotatePictureClockwise();
    double sourceAspect() const;
    double previewAspect() const;

    double submitFps() const;
    bool submitFpsKnown() const;
    double submitLowFps() const;
    bool submitLowFpsKnown() const;
    double lateMs() const;
    double lateP95Ms() const;
    QString metricsSummary() const;
    bool nrActive() const;
    bool nrAutoActive() const;
    int nrAutoPercent() const;
    QString nrAutoStatus() const;
    bool srActive() const;
    bool fgActive() const;
    double captureFps() const;
    double captureDropped() const;
    QString backendWarning() const;

    bool applying() const;
    double volume() const;
    void setVolume(double value);
    bool muted() const;
    void setMuted(bool value);

    bool nrEnabled() const;
    int selectedNrLayer() const;
    void setSelectedNrLayer(int index);
    void setNrEnabled(bool value);
    bool srEnabled() const;
    void setSrEnabled(bool value);
    int fgMultiplier() const;
    void setFgMultiplier(int value);
    bool lowLatency() const;
    void setLowLatency(bool value);
    bool nrTemporal() const;
    void setNrTemporal(bool value);
    // Stage-5 output stabiliser (anti-flicker). Global rather than per-node:
    // it runs once, after the whole NR stack and the residual composite.
    double nrHoldStrength() const;
    int nrReuseMode() const;
    void setNrReuseMode(int value);
    bool nrReuseActive() const;
    QString nrReuseStatus() const;
    void setNrHoldStrength(double value);
    double nrHoldTolerance() const;
    void setNrHoldTolerance(double value);
    bool videoHdr() const;
    void setVideoHdr(bool value);
    bool hdrCurve() const;
    void setHdrCurve(bool enabled);
    QVariantMap hdrCurveParams() const;
    Q_INVOKABLE bool setHdrCurveParameter(const QString& key, double value);
    bool hdrMetadataEnabled() const;
    void setHdrMetadataEnabled(bool enabled);
    QVariantMap hdrMetadataParams() const;
    Q_INVOKABLE bool setHdrMetadataParameter(const QString& key, double value);
    // Commit + re-assert the tuning in the places the session capture reads, so a
    // change takes effect and survives a restart (the same trap the earlier
    // per-scene brightness block hit: a plain commit left it out of the pending
    // settings the session is written from).
    bool commitHdrTuning(engine::EnhancementSettings settings);
    QString hdrTuningPreset() const;
    Q_INVOKABLE bool applyHdrTuningPreset(const QString& id);
    // Custom: apply the recommendation above in one click.
    Q_INVOKABLE bool applyRecommendedHdrTuning();
    // Custom: forget every remembered display profile (the current device is kept
    // and re-learned from the settings in use).
    Q_INVOKABLE void forgetHdrDisplayProfiles();
    Q_INVOKABLE bool applyHdrAxesPreset(const QString& content, const QString& environment);
    Q_INVOKABLE bool setHdrDial(const QString& key, int level);
    Q_INVOKABLE void forgetHdrTitles();
    // The strength dial and the display peak (stored next to the baseline).
    int hdrCurveStrength() const;
    void setHdrCurveStrength(int percent);
    int hdrDisplayPeakNits() const;
    void setHdrDisplayPeakNits(int nits);
    QVariantMap hdrDisplayInfo() const;
    // Custom: see the hdrSourceInfo/hdrRecommendation/hdrPeakWarning properties.
    QVariantMap hdrSourceInfo() const;
    QVariantMap hdrRecommendation() const;
    bool sourceIsSdr() const;
    unsigned effectiveHdrDisplayPeakNits() const;
    QVariantMap hdrPeakWarning() const;
    int hdrDisplayProfileCount() const;
    QString hdrPresetMode() const;
    QString hdrAppliedFrom() const;
    void setHdrAppliedFrom(const QString& from);
    void setHdrPresetMode(const QString& mode);
    QVariantMap hdrAxes() const;
    QVariantMap hdrAxesMatch() const;
    QVariantMap hdrDials() const;
    QVariantMap hdrCurveGraph() const;
    QVariantMap hdrCurveCoverage() const;
    Q_INVOKABLE void observeHdrCoverage(bool on);
    bool hdrTitleMemory() const;
    void setHdrTitleMemory(bool on);
    bool hdrAutoRecommend() const;
    void setHdrAutoRecommend(bool on);
    int hdrTitleCount() const;
    void syncHdrTitleProfile();
    void flushHdrMemory();
    bool loadHdrTitleProfile(const QString& title);
    bool storeHdrTitleProfile(const QString& title);
    bool loadHdrProfile(const QString& id);
    bool storeHdrProfile(const QString& id);
    QString hdrDisplayProfileId() const;
    void syncHdrDisplayProfile();
    QVariantMap videoHdrParams() const;
    Q_INVOKABLE bool setVideoHdrParameter(const QString& key, double value);
    bool antiAliasEnabled() const;
    void setAntiAliasEnabled(bool enabled);
    QVariantMap antiAliasParams() const;
    Q_INVOKABLE QVariantMap antiAliasParamsAt(int index) const;
    Q_INVOKABLE bool setAntiAliasParameter(const QString& key, double value);
    Q_INVOKABLE bool setAntiAliasParameterAt(int index, const QString& key, double value);
    int nrStyle() const;
    void setNrStyle(int value);
    double nrIntensity() const;
    void setNrIntensity(double value);
    double nrTone() const;
    void setNrTone(double value);
    double nrStructure() const;
    void setNrStructure(double value);
    double nrSkin() const;
    void setNrSkin(double value);
    bool nrAutoMask() const;
    void setNrAutoMask(bool value);
    bool nrUiCorrection() const;
    void setNrUiCorrection(bool value);
    int videoSrQuality() const;
    void setVideoSrQuality(int value);
    int dlssSrModel() const;
    void setDlssSrModel(int value);
    QVariantList dlssSrModelChoices() const;
    int srPrescale() const;
    bool dualSrEnabled() const;
    void setDualSrEnabled(bool enabled);
    int dualSrTargetIndex() const;
    void setDualSrTargetIndex(int index);
    int dualSrQuality() const;
    void setDualSrQuality(int quality);
    void setSrPrescale(int value);
    int srOutput() const;
    void setSrOutput(int value);
    bool protectionEnabled() const;
    void setProtectionEnabled(bool value);
    QVariantMap protectionState() const;
    QVariantMap exclusionState() const;
    QVariantMap nrStabilizerState() const;
    Q_INVOKABLE bool beginMaskDraw(bool allEffects,const QString& shape);
    Q_INVOKABLE bool setMaskEnabled(bool allEffects,bool enabled);
    Q_INVOKABLE bool setMaskFeather(bool allEffects,double pixels);
    Q_INVOKABLE bool removeMaskRegion(bool allEffects,int index);
    Q_INVOKABLE bool clearMaskRegions(bool allEffects);
    Q_INVOKABLE bool setStabilizerParameter(int index,const QString& key,double value);
    Q_INVOKABLE bool setProtectionRegion(double left, double top, double right, double bottom, double feather);
    bool colorEnabled() const;
    void setColorEnabled(bool value);
    int audioOffsetMs() const;
    void setAudioOffsetMs(int value);
    int audioSyncMode() const;
    void setAudioSyncMode(int value);
    bool audioSyncLive() const;
    double audioCompensationMs() const;
    double gpuUtilization() const;
    bool gpuUtilizationKnown() const;
    QString runStatus() const;
    QString runStatusLevel() const;
    QString runStatusDetail() const;
    QStringList runOptions() const;
    double effectiveOutputFps() const;
    double outputRateRatio() const;
    QVariantList frameTimes() const;
    int flowQuality() const;
    int hdrOutputMode() const;
    void setHdrOutputMode(int value);
    int fgMotionSource() const;
    void setFgMotionSource(int value);
    int srMotionSource() const;
    void setSrMotionSource(int value);
    int nrMotionSource() const;
    void setNrMotionSource(int value);
    void setFlowQuality(int value);
    bool amdFlowHalf() const;
    void setAmdFlowHalf(bool value);
    bool edgeAegis() const;
    void setEdgeAegis(bool value);
    bool dlssgLite() const;
    void setDlssgLite(bool value);
    bool vfgAutoRegulate() const;
    void setVfgAutoRegulate(bool value);
    bool dlssgLiteAvailable() const;
    QString dlssgLiteHint() const;
    int contentRate() const;
    void setContentRate(int value);
    int displaySync() const;
    void setDisplaySync(int value);
    bool variableRefresh() const;
    void setVariableRefresh(bool value);
    int vrrState() const;
    QString vrrHint() const;
    int displayHdr() const;
    double displayRefreshHz() const;
    QString vrrSummary() const;
    bool displayChecking() const;
    Q_INVOKABLE void recheckDisplay();
    QString fgDisplayHint() const;
    Q_INVOKABLE void setPresentationFullscreen(bool value);
    bool fullscreenMemorySafe() const;
    int outputRateMode() const;
    void setOutputRateMode(int value);
    double outputCustomFps() const;
    void setOutputCustomFps(double value);
    bool presentationOwned() const;
    QString presentationStatus() const;
    int compareMode() const;
    void setCompareMode(int value);
    bool compareBase() const;
    void setCompareBase(bool value);
    double compareSplit() const;
    int aspectMode() const;
    void setAspectMode(int value);
    double viewZoom() const;
    void setViewZoom(double value);
    double audioSkewMs() const;
    bool audioSkewKnown() const;
    QString sourceKind() const;
    QString sourceTitle() const;
    QString sourceFormatText() const;
    QString sourceRateText() const;
    bool colourCanUndo() const;
    bool colourCanRedo() const;
    bool colourCanPaste() const;
    Q_INVOKABLE bool colourUndo();
    Q_INVOKABLE bool colourRedo();
    Q_INVOKABLE void colourCopy();
    Q_INVOKABLE bool colourPaste();
    QString liveOpeningText() const;
    QVariantMap connection() const;
    Q_INVOKABLE void clearConnectionError();
    // The console's on-screen keyboard (connection.keyboard): the whole text, then accept or reject
    Q_INVOKABLE bool keyboardSetText(const QString& text);
    Q_INVOKABLE bool keyboardAccept();
    Q_INVOKABLE bool keyboardReject();
    Q_INVOKABLE void importPresetDialog();
    Q_INVOKABLE bool importPreset(const QString& path);
    Q_INVOKABLE void exportPresetDialog(int index);
    Q_INVOKABLE bool exportPreset(int index, const QString& path);
    // Split position from a pointer on the picture host (fractions of the host).
    Q_INVOKABLE void setCompareSplitAt(double hostFractionX);
    // List mode: an NR layer's model and residual back to their defaults.
    Q_INVOKABLE bool resetNrLayer(int index);
    QVariantList protectionRegions() const;
    QString protectionDrawShape() const;
    // Drawing on the picture: arm with "rect"/"ellipse" ("" cancels), then
    // report the drag in picture-host pixels; commit adds the region.
    Q_INVOKABLE bool beginProtectionDraw(const QString& shape);
    Q_INVOKABLE void updateProtectionDraw(double x0, double y0, double x1, double y1);
    Q_INVOKABLE bool commitProtectionDraw(double x0, double y0, double x1, double y1);
    Q_INVOKABLE bool removeProtectionRegion(int index);
    Q_INVOKABLE bool clearProtectionRegions();
    Q_INVOKABLE bool setProtectionFeather(double pixels);

    QVariantList chain() const;
    QVariantList nrLayers() const;
    Q_INVOKABLE bool setNrLayerParameter(int index, const QString& key, double value);
    Q_INVOKABLE QString nrAutoSelectionReason(int index) const;
    QVariantList effectCatalog() const;
    QString chainError() const;
    bool chainValid() const;

    QVariantList presets() const;
    QString currentPresetName() const;
    QVariantList audioTracks() const;
    int selectedAudioTrack() const;
    void setSelectedAudioTrack(int index);
    QString colorStatus() const;
    QString videoHdrStatus() const;

    int srTargetIndex() const;
    QString srTargetLabel() const;
    void setSrTargetIndex(int index);
    QVariantList presetSaveParts() const;
    int defaultPresetIndex() const;
    QVariantList colourParameters() const;
    QVariantList colourGroups() const;
    int selectedColourLayer() const;
    void setSelectedColourLayer(int index);
    QVariantMap colourState() const;
    Q_INVOKABLE double colourParameter(const QString& name) const;
    Q_INVOKABLE bool setColourParameter(const QString& name, double value);
    Q_INVOKABLE bool resetColourParameter(const QString& name);
    Q_INVOKABLE bool setColourCurve(int channel, const QVariantList& points);
    Q_INVOKABLE bool setColourWheel(int zone, double hue, double saturation, double luminance);
    Q_INVOKABLE bool setColourOption(const QString& name, int value);
    Q_INVOKABLE bool resetColourGroup(int group);
    QVariantList lutLibrary() const;
    QVariantList colourLooks() const;
    Q_INVOKABLE void importLutDialog();
    Q_INVOKABLE bool importLut(const QString& path);
    // Empty name clears the LUT from the selected Color node.
    Q_INVOKABLE bool setColourLut(const QString& name);
    Q_INVOKABLE bool saveColourLook(const QString& name, bool replace);
    Q_INVOKABLE bool applyColourLook(int index);
    Q_INVOKABLE bool deleteColourLook(int index);
    Q_INVOKABLE void importColourLookDialog();
    Q_INVOKABLE bool importColourLook(const QString& path);
    Q_INVOKABLE void exportColourLookDialog(int index);
    Q_INVOKABLE bool exportColourLook(int index, const QString& path);

    // The native HWND the engine presents into. The UI creates it as a child
    // window of the QML window and hands it over; the bridge never draws QML
    // into it and Qt never touches its swapchain.
    void attachVideoWindow(qulonglong nativeHandle);

    // Run immediately before the engine opens a source. The presenter reads the
    // host window's client size once, when it initialises, and builds the
    // swapchain from it; a window that is still 0x0 at that moment yields a 1x1
    // surface that never recovers. The UI supplies this hook so the geometry is
    // settled at the one moment that matters, instead of racing a timer.
    void setPreOpenHook(std::function<void()> hook);

    // --- commands exposed to QML -------------------------------------------
    // Every one returns immediately; none touches the GPU or waits for the
    // engine.
    Q_INVOKABLE void openPs5Dialog();
    Q_INVOKABLE void openAudioDialog();
    Q_INVOKABLE void openSubtitleDialog();
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void stopPlayback();
    Q_INVOKABLE void openTestVideo(const QString& path);
    // V held down: the engine shows the unprocessed frame until release
    // (AppShell holdOriginal -> engine.comparison(1, false)).
    Q_INVOKABLE void holdOriginal(bool held);
    // UI events the shell logs (fullscreen, lock) under the given channel.
    Q_INVOKABLE void logUi(const QString& channel, const QString& text);
    // Limits a window's pointer input to a rectangle (QWindow::setMask); the
    // fullscreen bar window uses it so only its pill takes the pointer.
    Q_INVOKABLE void setWindowMask(QObject* window, const QRectF& rect);
    Q_INVOKABLE void takeScreenshot();
    // The stream menu opened: the engine writes the frame on screen as a small-quality JPEG in this app's
    // cache (not when a screenshot is being written); menuBackdrop names it once it is there
    Q_INVOKABLE void requestMenuBackdrop(double widthFraction = 0.45, bool leftEdge = false);
    // The menu closed: the frame is deleted and forgotten
    Q_INVOKABLE void clearMenuBackdrop();
    QString menuBackdrop() const;
    Q_INVOKABLE void quit();
    Q_INVOKABLE void restartApplication();
    Q_INVOKABLE void restartForOverlayCompatibility();
    // Returns this page's controls to the engine defaults (the design's
    // "重置本页"); it does not touch presets or other pages.
    Q_INVOKABLE void resetCurrentPage();

    // Chain editing. The validator decides whether an edit is allowed where it
    // was asked (frame generation is pinned last, RTX Video HDR immediately
    // before it) and reports why when it is not.
    Q_INVOKABLE int addEffect(const QString& type);
    Q_INVOKABLE int duplicateNrLayer(int index);
    Q_INVOKABLE bool removeEffect(int index);
    Q_INVOKABLE bool moveEffect(int from, int to);
    Q_INVOKABLE bool setEffectEnabled(int index, bool enabled);
    // Every NR layer off at once; on again restores the layers that were on.
    Q_INVOKABLE bool setAllNrEnabled(bool enabled);
    bool nrAnyEnabled() const;
    Q_INVOKABLE QVariantMap effectDescriptor(const QString& type) const;

    // Presets. A preset carries selected parts, so there is one preset concept,
    // not several.
    Q_INVOKABLE bool applyPresetIndex(int index);
    Q_INVOKABLE bool savePresetAs(const QString& name, int contentsMask);
    Q_INVOKABLE bool deletePreset(int index);
    Q_INVOKABLE bool renamePreset(int index, const QString& name);
    Q_INVOKABLE bool duplicatePreset(int index);
    Q_INVOKABLE bool setDefaultPreset(int index);

    Q_INVOKABLE QString formatTime(double seconds) const;

    // Diagnostics for the settings page: what the engine actually reports,
    // never a summary written by hand.
    Q_INVOKABLE QString diagnosticsReport() const;
    Q_INVOKABLE void openProjectPage();
    Q_INVOKABLE void copyDiagnostics();
    // Re-reads the runtime manifest (Settings → 组件与许可 calls it when shown).
    Q_INVOKABLE void refreshComponentList();

signals:
    void ps5Changed();
    void connectionChanged();
    void gameKeysAllowedChanged();
    void padPressedChanged();
    void mouseAimChanged();
    // The pad's stream menu combination (Genel → Yayın menüsü) was pressed.
    void streamMenuRequested();
    void lanChanged();
    void micChanged();
    void audioDevicesChanged();
    void subtitlesChanged();
    void subtitleTextChanged();
    void preferencesChanged();
    void menuBackdropChanged();
    void colourLibraryChanged();
    // Emitted only when the underlying snapshot actually changed, so QML
    // bindings do not re-evaluate 60 times a second for nothing.
    void snapshotChanged();
    void componentListChanged();
    void perfChanged();
    void protectionDrawChanged();
    void statsOverlayChanged();
    void presentationChanged();
    void compareChanged();
    void settingsChanged();
    void fgChoicesChanged();
    void effectCapabilitiesChanged();
    void chainChanged();
    // A slider edits one group's values without touching the chain's structure,
    // labels or positions; only that group notifies (audit 2026-10-15 B1).
    void nrLayersChanged();
    void videoHdrParamsChanged();
    void antiAliasChanged();
    void colourParamsChanged();
    void protectionChanged();
    void nrStabilizerChanged();
    void presetsChanged();
    void uiDeviceGenerationChanged();
    // User-facing notices the UI shows as a transient message: a rejected
    // chain edit, an export that finished, a failed open.
    void notice(const QString& text, bool isError);
    // A presentation-hooking overlay appeared in a run whose interface draws on the GPU
    // ("rtss": its OSD is kept out of this process; "nvidia": NVIDIA's in-game overlay and
    // "gamepp": GamePP, which cannot be kept out); a restart draws the interface in software.
    void overlayRestartSuggested(const QString& kind);
    // The UI moves to a page by name when a command implies it.
    void navigate(const QString& page);

private:
    // Structural change: every chain-derived property notifies.
    void emitChainChanged();
    // Value edit of one group; a change of the chain's validity notifies everything.
    void emitParameterChanged(void (QmlPlayerBridge::*group)(), bool wasValid);
    // DLAA anti-aliasing needs the DLSS SR runtime and adapter; false (with a notice) otherwise.
    bool dlaaAllowed();
    // 抖音直播伴侣 running (gfx/PresentationHooks.h): Realtime GPU priority is capped at
    // High while it runs and restored when it exits; 监控软件兼容 "off" leaves it alone.
    void updateLiveCompanionCompat();
    void applyLiveCompanionCompat(bool running);
    // The settings the UI is showing: the facade's pending copy, which the
    // engine has been asked to apply. Every settings property reads this, so
    // they can never disagree with each other.
    engine::EnhancementSettings settings() const;
    void startGpuSampler();
    void requestApplicationRestart(bool overlayCompatibility);
    void updateRunStatus();
    // vrrHint's text for a VRR level (the 重新检测 result notice uses it too).
    QString vrrHintText(int level) const;
    QString vrrSummaryText(int level) const;
    void applyPresentation();
    void applyAspect(bool force);
    // Live sources started from the start page: switch to 极简 first, show the
    // connecting note, and start the engine once the page and window have settled.
    void openAfterCinema(const QString& pending, std::function<void()> open);
    void rememberSource(const QString& kind, const QString& label);
    // Account consoles (bridge/RemotePlay.cpp): uploads one saved pairing; merges the account's list.
    void putAccountConsole(const QString& profileId);
    void takeAccountConsoles(const QJsonArray& consoles);
    // Edits either mask node in the active chain; rolls back when refused.
    QVariantMap maskState(bool allEffects) const;
    bool editProtection(const std::function<bool(engine::ChainNode&)>& edit,bool allEffects=false);
    engine::ColorSettings selectedColourSettings() const;
    bool commitColour(const engine::ColorSettings& colour);
    // Pushes a stored preference to whatever consumes it (subtitle overlay,
    // audio endpoint). Called after load and after every accepted change.
    void applyPreference(const QString& key);
    // Hold-to-compare key (press/release), handled for the whole application.
    bool eventFilter(QObject* watched, QEvent* event) override;
    // Writes the shell preferences now (a pending volume included).
    void flushPreferences();
    void refreshSubtitles(const std::wstring& media);
    void tickSubtitles();
    // Confirms a screenshot the engine was asked to write (takeScreenshot).
    void tickScreenshot();
    void tickPs5();
    void updateConnection();
    void tickWakeWait();
    // Logs the shortcut map (at start and after every change).
    void logShortcuts();
    void tickLan();
    // bridge/StreamSession.cpp: the microphone, auto connect at start and the PC going to sleep.
    void tickStreamSession();
    void tickStatsOverlay();
    void followStatsOverlay();
    void tickAutoConnect();
    void pcSuspending();
    // Game input (bridge/GameInput.cpp): the parsed controller/keyboard preferences, one
    // controller tick (pad + keyboard to the PS5, feedback back), and the keyboard play filter.
    void loadGameInput();
    void pollController();
    bool gameKeyEvent(QObject* watched, QEvent* event);
    void releaseGameKeys();
    // Mouse aim (bridge/MouseAim.cpp): settings, capture, raw input, the per-tick stick and
    // the window events it takes (the pointer while captured, its toggle key and Esc).
    void loadMouseAim();
    void updateMouseAim();
    void mouseAimCapture(QWindow* window);
    void mouseAimRelease(const char* why);
    void mouseAimRaw(const tagRAWMOUSE& m);
    void mouseAimTick(remoteplay::ControllerState& state);
    bool mouseAimEvent(QObject* watched, QEvent* event);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oviora::ui
