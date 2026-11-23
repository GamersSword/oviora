#include "oviora/gfx/NvidiaAppProfile.h"
#include "oviora/Log.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <string>

// Minimal NVAPI driver settings (DRS) ABI, checked against NVIDIA/nvapi (MIT): function IDs from
// nvapi_interface.h, NVDRS_SETTING_V1 / NVDRS_APPLICATION_V4 / NVDRS_PROFILE_V1 layouts and the
// VRR_APP_OVERRIDE setting from nvapi.h and NvApiDriverSettings.h. Same loading rule as
// NvidiaVrr.h: System32 nvapi64.dll only. No SDK header or runtime is distributed.
namespace oviora::gfx {
namespace {
using U16Str = uint16_t[2048];  // NvAPI_UnicodeString
#pragma pack(push, 4)
struct Binary { uint32_t length; uint8_t data[4096]; };
union Value { uint32_t u32; Binary binary; U16Str text; uint64_t u64; float f32; };
struct Setting {
    uint32_t version;
    U16Str name;
    uint32_t id, type, location, isCurrentPredefined, isPredefinedValid;
    Value predefined, current;
};
#pragma pack(pop)
struct Application {
    uint32_t version, isPredefined;
    U16Str appName, friendlyName, launcher, fileInFolder;
    uint32_t flags;
    U16Str commandLine;
};
struct Profile {
    uint32_t version;
    U16Str name;
    uint32_t gpuSupport, isPredefined, numOfApps, numOfSettings;
};
static_assert(sizeof(Setting) == 0x3020 && sizeof(Application) == 0x500C && sizeof(Profile) == 0x1014);
constexpr uint32_t ver(size_t size, uint32_t v) { return uint32_t(size) | (v << 16); }
constexpr uint32_t kGsyncAppState = 0x10A879CF;         // VRR_APP_OVERRIDE_ID ("G-SYNC")
constexpr uint32_t kGsyncAppRequestState = 0x10A879AC;  // VRR_APP_OVERRIDE_REQUEST_STATE_ID
constexpr uint32_t kFixedRefresh = 4;                   // VRR_APP_OVERRIDE_FIXED_REFRESH
constexpr int kSettingNotFound = -160;                  // NVAPI_SETTING_NOT_FOUND
constexpr int kExecutableNotFound = -166;               // NVAPI_EXECUTABLE_NOT_FOUND
constexpr int kProfileNotFound = -163;                  // NVAPI_PROFILE_NOT_FOUND

void copy(U16Str& out, const wchar_t* text) {
    size_t i = 0;
    for (; text[i] && i + 1 < 2048; ++i) out[i] = uint16_t(text[i]);
    out[i] = 0;
}
}

const char* ensureFixedRefreshProfile() {
    if (GetEnvironmentVariableW(L"OVIORA_TEST_NO_GSYNC_PROFILE", nullptr, 0)) return "skipped (test)";
    using Query = void*(__cdecl*)(uint32_t);
    using Init = int(__cdecl*)();
    using CreateSession = int(__cdecl*)(void**);
    using SessionFn = int(__cdecl*)(void*);
    using FindApp = int(__cdecl*)(void*, const uint16_t*, void**, Application*);
    using FindProfile = int(__cdecl*)(void*, const uint16_t*, void**);
    using CreateProfile = int(__cdecl*)(void*, Profile*, void**);
    using CreateApp = int(__cdecl*)(void*, void*, Application*);
    using SettingFn = int(__cdecl*)(void*, void*, uint32_t, Setting*);
    using SetSetting = int(__cdecl*)(void*, void*, Setting*);

    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    HMODULE module = LoadLibraryExW((std::wstring(system) + L"\\nvapi64.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return "no NVIDIA driver";
    auto query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
    auto fn = [&](uint32_t id) { return query ? query(id) : nullptr; };
    auto init = reinterpret_cast<Init>(fn(0x0150E828));
    auto createSession = reinterpret_cast<CreateSession>(fn(0x0694D52E));
    auto destroySession = reinterpret_cast<SessionFn>(fn(0xDAD9CFF8));
    auto loadSettings = reinterpret_cast<SessionFn>(fn(0x375DBD6B));
    auto saveSettings = reinterpret_cast<SessionFn>(fn(0xFCBC7E14));
    auto findApp = reinterpret_cast<FindApp>(fn(0xEEE566B2));
    auto findProfile = reinterpret_cast<FindProfile>(fn(0x7E4A9A0B));
    auto createProfile = reinterpret_cast<CreateProfile>(fn(0xCC176068));
    auto createApp = reinterpret_cast<CreateApp>(fn(0x4347A9DE));
    auto getSetting = reinterpret_cast<SettingFn>(fn(0x73BF8338));
    auto setSetting = reinterpret_cast<SetSetting>(fn(0x577DD202));
    if (!init || !createSession || !destroySession || !loadSettings || !saveSettings || !findApp || !findProfile ||
        !createProfile || !createApp || !getSetting || !setSetting || init() != 0)
        return "NVAPI unavailable";

    void* session = nullptr;
    if (createSession(&session) != 0) return "no driver settings session";
    struct Close { SessionFn destroy; void* session; ~Close() { destroy(session); } } close{destroySession, session};
    if (int s = loadSettings(session); s != 0) {
        log::warn("gsync-profile", std::format("driver settings not loaded status={}", s));
        return "driver settings not loaded";
    }
    // The program this process runs as (Oviora.exe; a renamed copy gets its own entry)
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* exe = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
    U16Str exeName{};
    copy(exeName, exe);

    void* profile = nullptr;
    auto app = std::make_unique<Application>();
    app->version = ver(sizeof(Application), 4);
    int s = findApp(session, exeName, &profile, app.get());
    if (s == kExecutableNotFound) {
        U16Str profileName{};
        copy(profileName, L"Oviora");
        s = findProfile(session, profileName, &profile);
        if (s == kProfileNotFound) {
            auto p = std::make_unique<Profile>();
            p->version = ver(sizeof(Profile), 1);
            copy(p->name, L"Oviora");
            s = createProfile(session, p.get(), &profile);
        }
        if (s == 0) {
            auto a = std::make_unique<Application>();
            a->version = ver(sizeof(Application), 4);
            std::memcpy(a->appName, exeName, sizeof(exeName));
            copy(a->friendlyName, L"Oviora");
            s = createApp(session, profile, a.get());
        }
    }
    if (s != 0 || !profile) {
        log::warn("gsync-profile", std::format("profile not found or created status={}", s));
        return "profile not created";
    }
    bool changed = false;
    for (uint32_t id : {kGsyncAppState, kGsyncAppRequestState}) {
        auto setting = std::make_unique<Setting>();
        setting->version = ver(sizeof(Setting), 1);
        const int got = getSetting(session, profile, id, setting.get());
        if (got == 0 && setting->location == 0 && setting->current.u32 == kFixedRefresh) continue;
        if (got != 0 && got != kSettingNotFound)
            log::warn("gsync-profile", std::format("read setting 0x{:08X} status={}", id, got));
        auto value = std::make_unique<Setting>();
        value->version = ver(sizeof(Setting), 1);
        value->id = id;
        value->type = 0;  // NVDRS_DWORD_TYPE
        value->current.u32 = kFixedRefresh;
        if (int set = setSetting(session, profile, value.get()); set != 0) {
            log::warn("gsync-profile", std::format("set setting 0x{:08X} status={}", id, set));
            return "setting rejected";
        }
        changed = true;
    }
    if (!changed) return "already fixed refresh";
    if (int saved = saveSettings(session); saved != 0) {
        // -137 NVAPI_INVALID_USER_PRIVILEGE: this driver lets only administrators write profiles
        log::warn("gsync-profile", std::format("driver settings not saved status={}", saved));
        return "not saved";
    }
    return "written (G-SYNC off for Oviora from its next start)";
}
}
