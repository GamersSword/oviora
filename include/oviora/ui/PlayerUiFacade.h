#pragma once
#include "oviora/engine/EngineController.h"
#include "oviora/engine/PresetLibrary.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace oviora::ui {
// The interface the user interface talks to.
//
// The QML front end must not hold EngineController, D3D12 objects, decoded
// frames or the audio clock, and it must not poll a 70-field snapshot at a
// fixed rate. This facade:
//   * exposes one snapshot per change, tagged with a counter the UI compares
//     (so a binding updates only what moved);
//   * turns commands into queued actions and never blocks the UI thread;
//   * owns the user-facing services: the preset library and the shell preferences.
//
// It is deliberately free of Win32 and Qt so the same code serves both the
// transitional Win32 shell and the QML front end.
class PlayerUiFacade {
public:
    // A snapshot plus the counter the UI uses to skip unchanged data.
    struct Frame {
        oviora::engine::PlayerSnapshot snapshot;
        uint64_t revision = 0;      // increments whenever `snapshot` changed
        bool changed = false;
    };

    explicit PlayerUiFacade(oviora::engine::EngineController& engine,
                            std::filesystem::path dataDirectory = {});

    // --- snapshot -----------------------------------------------------------
    // Cheap: compares the engine's snapshot against the last one handed out and
    // only reports a change when something actually differs.
    Frame poll();
    const oviora::engine::PlayerSnapshot& last() const { return last_; }
    uint64_t revision() const { return revision_; }

    // --- commands ----------------------------------------------------------
    // Applied to the engine's desired settings, keeping the facade's copy in
    // step so the UI can show the pending value before the engine confirms it.
    bool applySettings(const oviora::engine::EnhancementSettings& settings,
                       std::optional<oviora::engine::ChainRuntimeOrder> order = std::nullopt);
    oviora::engine::EnhancementSettings pendingSettings() const { return pending_; }
    void setPending(oviora::engine::EnhancementSettings settings) {
        settings.content=oviora::engine::currentContentRate(settings.content);
        pending_ = std::move(settings);
    }
    // True while the engine has not yet applied what the UI asked for.
    bool applying() const;

    // --- presets -----------------------------------------------------------
    oviora::engine::PresetLibrary& presets() { return presets_; }
    const oviora::engine::PresetLibrary& presets() const { return presets_; }
    bool applyPreset(size_t index);
    // Saves the settings the UI is currently showing (pending, not applied -
    // the user saves what they see).
    bool savePreset(const oviora::engine::PresetEntry& entry, bool replace);
    // Loads the preset library (once), then migrates the older single-purpose
    // stores into it, so nobody loses their presets. Never overwrites an
    // existing name.
    bool importLegacyStores();

    // --- shell preferences --------------------------------------------------
    // Reduced motion turns off the springs and transitions. Default page is what
    // the app opens on (home / minimal / professional). Both persist here so they
    // survive a restart, and both are preferences rather than enhancement settings:
    // they never affect the picture.
    bool reducedMotion() const { return reducedMotion_; }
    void setReducedMotion(bool value) { reducedMotion_ = value; savePreferences(); }
    const std::wstring& defaultPage() const { return defaultPage_; }
    void setDefaultPage(std::wstring value) { defaultPage_ = std::move(value); savePreferences(); }

    // --- persistence -------------------------------------------------------
    bool loadPreferences();
    bool savePreferences();
    bool loadChainSession(oviora::engine::ChainSession&);
    bool saveChainSession(const oviora::engine::ChainSession&);
    const std::wstring& error() const { return error_; }

private:
    oviora::engine::EngineController& engine_;
    oviora::engine::PresetLibrary presets_;
    bool presetsLoaded_ = false;
    std::filesystem::path dataDirectory_;
    oviora::engine::ChainSessionStore chainSessionStore_;
    oviora::engine::PlayerSnapshot last_{};
    oviora::engine::EnhancementSettings pending_{};
    uint64_t pendingFgRevision_=0;
    uint64_t pendingNrRevision_=0;
    oviora::engine::NrRuntime requestedNrRuntime_=oviora::engine::NrRuntime::Ampere;
    oviora::engine::FrameGenerationBackend requestedFgBackend_=oviora::engine::FrameGenerationBackend::Dlss;
    uint32_t requestedFgMultiplier_=1;
    uint32_t requestedVfgQuality_=1;
    uint64_t revision_ = 0;
    bool haveLast_ = false;
    // Frame counters alone refresh the UI at most twice a second (see poll())
    std::chrono::steady_clock::time_point lastChange_{};
    bool reducedMotion_ = false;
    std::wstring defaultPage_ = L"home";
    std::wstring error_;
    std::filesystem::path preferencesPath() const;
    bool savePreferencesLocked();
};
}
