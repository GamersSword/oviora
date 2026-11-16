#include "oviora/ui/PlayerUiFacade.h"

#include "oviora/RuntimePaths.h"
#include "oviora/Log.h"
#include "oviora/engine/PresetStore.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <format>
#include <iomanip>
#include <sstream>

namespace oviora::ui {
namespace {
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? size_t(n) : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(n > 0 ? size_t(n) : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
// Network buffering keeps the playback position frozen, so its own numbers have
// to count as a change. Rounded to what the panel prints so the 100 ms stat
// refresh does not repaint on every poll.
bool sameNetwork(const oviora::media::NetworkStats& a, const oviora::media::NetworkStats& b) {
    return a.active == b.active && a.live == b.live && a.state == b.state && a.bandwidthLow == b.bandwidthLow &&
        a.reconnects == b.reconnects && a.reconnectAttempt == b.reconnectAttempt && a.stalls == b.stalls &&
        a.corruptPackets == b.corruptPackets && a.corruptFrames == b.corruptFrames && a.skippedFrames == b.skippedFrames &&
        a.message == b.message &&
        std::llround(a.bufferedSeconds * 10) == std::llround(b.bufferedSeconds * 10) &&
        std::llround(a.targetSeconds * 10) == std::llround(b.targetSeconds * 10) &&
        std::llround(a.downloadBytesPerSecond / 1.0e5) == std::llround(b.downloadBytesPerSecond / 1.0e5);
}
// Only the fields the UI can show are compared. Comparing the whole struct is
// fine too, but calling it out keeps the intent visible: a change here is what
// makes the UI repaint.
bool sameSnapshot(const oviora::engine::PlayerSnapshot& a, const oviora::engine::PlayerSnapshot& b) {
    return sameNetwork(a.network, b.network) &&
        a.presentationStatus==b.presentationStatus&&a.presentationEffective==b.presentationEffective&&
        a.presentationRevision==b.presentationRevision&&a.presentationProviderOwned==b.presentationProviderOwned&&
        a.transport == b.transport && a.sessionId == b.sessionId &&
        a.duration == b.duration &&
        a.status == b.status && a.savedFramePath == b.savedFramePath && a.backendWarning == b.backendWarning && a.sourceNotice == b.sourceNotice &&
        a.applying == b.applying && a.rejectedRevision == b.rejectedRevision &&
        a.desired == b.desired && a.applied == b.applied && a.nrActive == b.nrActive &&
        a.srActive == b.srActive && a.fgActive == b.fgActive && a.failed == b.failed &&
        a.volume == b.volume && a.muted == b.muted && a.selectedAudioTrack == b.selectedAudioTrack &&
        a.audioTracks.size() == b.audioTracks.size() && a.running == b.running &&
        a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight &&
        a.sourceDisplayAspect == b.sourceDisplayAspect && a.sourceRotationDegrees == b.sourceRotationDegrees &&
        a.capture == b.capture && a.image == b.image &&
        a.playbackRate == b.playbackRate && a.vramRunawayMiB == b.vramRunawayMiB && a.vramFullscreenUnsafe == b.vramFullscreenUnsafe &&
        a.metrics.resolution.base.width == b.metrics.resolution.base.width &&
        a.metrics.resolution.base.height == b.metrics.resolution.base.height &&
        a.metrics.resolution.nr.width == b.metrics.resolution.nr.width &&
        a.metrics.resolution.nr.height == b.metrics.resolution.nr.height &&
        a.metrics.resolution.output.width == b.metrics.resolution.output.width &&
        a.metrics.resolution.output.height == b.metrics.resolution.output.height &&
        a.metrics.resolution.firstSrOutput == b.metrics.resolution.firstSrOutput &&
        a.metrics.resolution.dualSrApplied == b.metrics.resolution.dualSrApplied &&
        a.metrics.resolution.srInput == b.metrics.resolution.srInput &&
        a.metrics.resolution.srOutput == b.metrics.resolution.srOutput &&
        a.metrics.resolution.srPrescaleIneffective == b.metrics.resolution.srPrescaleIneffective &&
        a.metrics.resolution.srOutputIneffective == b.metrics.resolution.srOutputIneffective;
}
} // namespace

PlayerUiFacade::PlayerUiFacade(oviora::engine::EngineController& engine, std::filesystem::path dataDirectory)
    : engine_(engine),
      presets_((dataDirectory.empty() ? oviora::runtime::localDataDirectory() : dataDirectory) / L"presets.v1"),
      dataDirectory_(dataDirectory.empty() ? oviora::runtime::localDataDirectory() : dataDirectory),
      chainSessionStore_(dataDirectory_ / L"chain-session.v1") {}

bool PlayerUiFacade::loadChainSession(oviora::engine::ChainSession& session) {
    const bool loaded = chainSessionStore_.load(session);
    error_ = chainSessionStore_.error();
    return loaded;
}

bool PlayerUiFacade::saveChainSession(const oviora::engine::ChainSession& session) {
    if (chainSessionStore_.save(session)) return true;
    error_ = chainSessionStore_.error(); return false;
}

PlayerUiFacade::Frame PlayerUiFacade::poll() {
    // The engine owns the snapshot lock; taking it here and comparing keeps the
    // UI free of the read-modify pattern it used to do at 4 Hz.
    const auto snapshot = engine_.snapshot();
    if(pendingNrRevision_&&snapshot.rejectedRevision==pendingNrRevision_&&pending_.nrRuntime==requestedNrRuntime_) {
        pending_.nrRuntime=snapshot.desired.nrRuntime;pendingNrRevision_=0;
        for(uint32_t i=0;i<pending_.nrLayerCount&&i<pending_.nrLayers.size();++i)
            pending_.nrLayers[i].runtime=pending_.nrRuntime;
        oviora::log::info("ui-nr-rollback",std::format("rejectedRevision={} restoredRuntime={}",snapshot.rejectedRevision,oviora::engine::nrRuntimeName(pending_.nrRuntime)));
    }
    // A GPU/provider failure arrives after command admission. Match the exact
    // accepted revision so a stale rejection cannot overwrite a newer choice.
    // Opening a source assigns a new revision to the already accepted request.
    // VFG/ANR startup/runtime recovery keeps that revision and disables only FG.
    // Never reconcile a rejection older than the UI's latest accepted edit.
    const bool recoveredGraphFg=snapshot.rejectedRevision>=pendingFgRevision_&&
        snapshot.desired.revision==snapshot.rejectedRevision&&
        (requestedFgBackend_==oviora::engine::FrameGenerationBackend::Vfg||requestedFgBackend_==oviora::engine::FrameGenerationBackend::Anr)&&
        snapshot.desired.frameGenerationBackend==requestedFgBackend_&&
        snapshot.desired.vfgQuality==requestedVfgQuality_&&
        requestedFgMultiplier_>1&&snapshot.desired.multiplier==1&&
        !snapshot.backendWarning.empty();
    if(pendingFgRevision_&&(snapshot.rejectedRevision==pendingFgRevision_||recoveredGraphFg)&&
       pending_.frameGenerationBackend==requestedFgBackend_&&pending_.multiplier==requestedFgMultiplier_&&
       pending_.vfgQuality==requestedVfgQuality_){
        pending_.frameGenerationBackend=snapshot.desired.frameGenerationBackend;
        pending_.multiplier=snapshot.desired.multiplier;
        pending_.vfgQuality=snapshot.desired.vfgQuality;pendingFgRevision_=0;
        oviora::log::info("ui-fg-rollback",std::format("rejectedRevision={} restoredBackend={} restoredMultiplier={}",
            snapshot.rejectedRevision,int(pending_.frameGenerationBackend),pending_.multiplier));
    }
    Frame frame;
    frame.snapshot = snapshot;
    // Oviora: position and the frame counters move on every video frame; comparing them made the
    // whole QML scene repaint at the stream's frame rate. They now only refresh the UI (stats,
    // timings) twice a second; every other field still updates at once.
    const auto now = std::chrono::steady_clock::now();
    frame.changed = !haveLast_ || !sameSnapshot(snapshot, last_) ||
        ((snapshot.frames != last_.frames || snapshot.generated != last_.generated || snapshot.position != last_.position) &&
         now - lastChange_ >= std::chrono::milliseconds(500));
    if (frame.changed) {
        last_ = snapshot;
        lastChange_ = now;
        haveLast_ = true;
        ++revision_;
    }
    frame.revision = revision_;
    return frame;
}

bool PlayerUiFacade::applySettings(const oviora::engine::EnhancementSettings& savedSettings,
                                  std::optional<oviora::engine::ChainRuntimeOrder> order) {
    auto settings=savedSettings;
    settings.content=oviora::engine::currentContentRate(settings.content);
    uint64_t acceptedRevision=0;
    if (!engine_.requestSettings(settings, order,&acceptedRevision)) return false;
    oviora::log::info("ui-settings-request", std::format(
        "accepted srTarget={} quality={} fgBackend={} multiplier={} flowBackend={} srPrescale={} srOutput={}",
        int(settings.srTarget), settings.videoSrQuality, int(settings.frameGenerationBackend), settings.multiplier, int(settings.opticalFlowBackend),
        uint32_t(settings.srPrescale), uint32_t(settings.srOutput)));
    pending_ = settings;
    pendingFgRevision_=acceptedRevision;
    pendingNrRevision_=acceptedRevision;requestedNrRuntime_=settings.nrRuntime;
    requestedFgBackend_=settings.frameGenerationBackend;requestedFgMultiplier_=settings.multiplier;
    requestedVfgQuality_=settings.vfgQuality;
    return true;
}

bool PlayerUiFacade::applying() const {
    // The engine publishes both the desired and the applied settings; a
    // difference means a transaction is still in flight.
    return engine_.snapshot().applying;
}

bool PlayerUiFacade::applyPreset(size_t index) {
    const auto& entries = presets_.entries();
    if (index >= entries.size()) { error_ = L"预设不存在"; return false; }
    auto settings = pending_;
    oviora::engine::PresetLibrary::apply(entries[index], settings);
    return applySettings(settings);
}

bool PlayerUiFacade::savePreset(const oviora::engine::PresetEntry& entry, bool replace) {
    if (!presets_.put(entry, replace)) { error_ = presets_.error(); return false; }
    return true;
}

bool PlayerUiFacade::importLegacyStores() {
    // The library has to be read first: importing into an unloaded library would
    // save it as built-ins plus imports and overwrite the user's own presets.v1.
    // A corrupt file stays loaded-as-corrupt, and the library then refuses to save.
    if (!presetsLoaded_) {
        presetsLoaded_ = true;
        // 2.0 ships no built-in presets (user decision 2026-09-29).
        presets_.setIncludeBuiltins(false);
        if (!presets_.load()) { error_ = presets_.error(); return false; }
        error_ = presets_.error(); // Successful legacy migration carries an explicit notice.
    }
    // The old files are read-only inputs: they stay on disk untouched so a
    // downgrade still finds them. Their positional schema is owned by
    // PresetStore, so the migration goes through that parser instead of
    // re-implementing it (and silently reading fields in the wrong order).
    std::vector<oviora::engine::PresetEntry> imported;
    auto readStore = [&imported](const std::filesystem::path& path, bool chainOnly) {
        if (!std::filesystem::exists(path)) return;
        oviora::engine::PresetStore store(path);
        if (!store.load()) return;   // corrupt or unsupported: leave it alone
        for (const auto& legacy : store.entries()) {
            if (legacy.name.empty()) continue;
            oviora::engine::PresetEntry entry;
            entry.name = legacy.name;
            entry.note = chainOnly ? L"由旧的 NR 预设导入" : L"由旧预设导入";
            entry.kind = oviora::engine::ChainMode::List;
            entry.builtin = false;
            if (chainOnly) {
                auto settings = legacy.settings;
                settings.color = {};
                settings.videoHdr = {};
                settings.multiplier = 1;
                entry.contents = oviora::engine::presetContentMask(oviora::engine::PresetContent::Chain);
                entry.chain = oviora::engine::toChain(settings);
            } else {
                entry.contents = oviora::engine::kPresetAllContent;
                entry.chain = oviora::engine::toChain(legacy.settings);
                entry.color = legacy.settings.color;
                entry.fg = {std::max(2u, legacy.settings.multiplier), legacy.settings.frameGenerationBackend};
                entry.audioSync = legacy.settings.audioSync;
                entry.audioOffsetMs = legacy.settings.audioOffsetMs;
            }
            imported.push_back(std::move(entry));
        }
    };
    readStore(dataDirectory_ / L"user-presets.v1", false);
    readStore(dataDirectory_ / L"nr-presets.v1", true);
    // Old colour looks are a separate store with its own schema; they keep their
    // own file and loader, so they are not duplicated into the library here.
    if (imported.empty()) return true;
    if (!presets_.importLegacy(imported)) { error_ = presets_.error(); return false; }
    return true;
}

std::filesystem::path PlayerUiFacade::preferencesPath() const {
    return dataDirectory_ / L"ui-session.v1";
}

bool PlayerUiFacade::savePreferencesLocked() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "OVIORA_UI_SESSION 1\n";
    // Format v1 keeps its two retired sections (recent files, last capture
    // session), written empty, so older builds still read the file.
    out << 0 << '\n';
    out << 0 << ' ' << std::quoted(std::string()) << ' ' << std::quoted(std::string()) << ' '
        << std::quoted(std::string()) << ' ' << 0.0 << '\n';
    out << (reducedMotion_ ? 1 : 0) << ' ' << std::quoted(utf8(defaultPage_)) << '\n';
    const auto temporary = std::filesystem::path(preferencesPath()).concat(L".tmp");
    { std::ofstream f(temporary, std::ios::binary | std::ios::trunc); if (!f) { error_ = L"界面状态写入失败"; return false; } f << out.str(); if (!f) { error_ = L"界面状态写入失败"; return false; } }
    if (!MoveFileExW(temporary.c_str(), preferencesPath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        error_ = L"界面状态替换失败";
        return false;
    }
    return true;
}

bool PlayerUiFacade::savePreferences() { return savePreferencesLocked(); }

bool PlayerUiFacade::loadPreferences() {
    // Missing file is the normal first-run case.
    if (!std::filesystem::exists(preferencesPath())) return true;
    std::ifstream f(preferencesPath(), std::ios::binary);
    if (!f) { error_ = L"界面状态无法读取"; return false; }
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::istringstream in(data);
    in.imbue(std::locale::classic());
    std::string magic;
    int version = 0;
    size_t count = 0;
    if (!(in >> magic >> version) || magic != "OVIORA_UI_SESSION" || version != 1) { error_ = L"界面状态版本不支持"; return false; }
    if (!(in >> count) || count > 32) { error_ = L"界面状态条目数非法"; return false; }
    // Retired sections of format v1 (recent files, last capture session): read and dropped.
    for (size_t i = 0; i < count; ++i) {
        std::string path, label;
        if (!(in >> std::quoted(path) >> std::quoted(label))) { error_ = L"界面状态条目损坏"; return false; }
    }
    {
        int valid = 0;
        double fps = 0;
        std::string device, format, preset;
        if (!(in >> valid >> std::quoted(device) >> std::quoted(format) >> std::quoted(preset) >> fps)) { error_ = L"界面状态条目损坏"; return false; }
    }
    // The shell preferences follow. A file from a build that predates them simply
    // has no tail, and that is not an error: the defaults stand.
    int reduced = 0;
    std::string defaultPage;
    if (in >> reduced >> std::quoted(defaultPage)) {
        reducedMotion_ = reduced != 0;
        if (!defaultPage.empty()) defaultPage_ = wide(defaultPage);
    }
    return true;
}
}
