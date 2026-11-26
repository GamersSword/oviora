// QML bridge: RemotePlay responsibilities.
#include "../QmlPlayerBridge_p.h"
#include "oviora/remoteplay/ChiakiBackend.h"
#include <QDateTime>

namespace oviora::ui {
// --- PS5 Remote Play (P4-e) ---------------------------------------------------
namespace {
constexpr uint32_t kPs5Bitrates[] = {5000, 10000, 15000, 20000, 30000, 50000, 80000, 100000};
#ifdef OVIORA_ENABLE_REMOTEPLAY
// WritePrivateProfileString does not make the folder: someone who only plays over PSN has no pairing (so no
// folder) and every quality / bit rate choice was lost on the next start
std::filesystem::path ps5Settings() {
    const auto directory = remoteplay::profileDirectory();
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory / L"settings.ini";
}
#endif
}
QVariantMap QmlPlayerBridge::ps5() const {
    QVariantMap out = impl_->ps5Form;
    out["status"] = impl_->ps5Status;
    out["busy"] = impl_->ps5Busy;
    out["profile"] = QString::fromStdWString(impl_->ps5Profile);
    out["editing"] = !impl_->ps5EditTarget.empty();
    out["pairRevision"] = impl_->pairRevision;
#ifdef OVIORA_ENABLE_REMOTEPLAY
    out["available"] = true;
    const auto& s = impl_->snapshot;
    out["active"] = s.remotePlay && (s.running || s.transport == engine::TransportState::Opening || s.transport == engine::TransportState::Stopping);
    const auto c = impl_->controller.capabilities();
    out["controller"] = c.connected;
    out["gyro"] = c.gyro && c.accel;
    out["calibrating"] = c.calibrating;
#else
    out["available"] = false;
#endif
    return out;
}
QVariantList QmlPlayerBridge::ps5Profiles() const { return impl_->ps5ProfileList; }

void QmlPlayerBridge::ps5Load() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    try {
        const auto directory = remoteplay::profileDirectory();
        if (i.ps5Profile.empty()) {
            wchar_t last[80]{};
            GetPrivateProfileStringW(L"RemotePlay", L"LastProfile", L"", last, 80, ps5Settings().c_str());
            const std::wstring name = last;
            if (!name.empty() && name.find_first_of(L"/\\:") == std::wstring::npos) i.ps5Profile = name;
        }
        i.ps5ProfileList.clear();
        unsigned failures = 0;
        auto add = [&](const std::filesystem::path& path) {
            remoteplay::ProfileLoadError error{};
            auto saved = remoteplay::loadProfile(path, &error);
            if (saved) i.ps5ProfileList << QVariantMap{{"id", QString::fromStdWString(path.filename().wstring())},
                                                      {"label", QString::fromStdString(saved->host)},
                                                      {"consoleId", QString::fromStdString(saved->consoleId)}};
            else if (error != remoteplay::ProfileLoadError::Missing) ++failures;
        };
        add(directory / "remoteplay-profile.dat");
        std::error_code ec; size_t scanned = 0;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end && i.ps5ProfileList.size() < 64 && scanned < 256; it.increment(ec), ++scanned) {
            const auto id = it->path().stem().wstring();
            if (it->path().extension() == ".dat" && id.size() == 32 &&
                std::all_of(id.begin(), id.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }))
                add(it->path());
        }
        bool known = false;
        for (const auto& p : i.ps5ProfileList) known = known || p.toMap().value("id").toString() == QString::fromStdWString(i.ps5Profile);
        if (!known) i.ps5Profile = i.ps5ProfileList.isEmpty() ? std::wstring{} : i.ps5ProfileList.front().toMap().value("id").toString().toStdWString();
        // Form defaults, then the selected pairing's own values.
        i.ps5Form = QVariantMap{{"host", QString()}, {"account", QString()}, {"quality", 3}, {"codec", 0}, {"bitrate", 2}, {"viewOnly", false},
            {"decode", int(std::min(2u, GetPrivateProfileIntW(L"RemotePlay", L"DecodeMode", 0, ps5Settings().c_str())))},
            {"sampling", int(std::min(1u, GetPrivateProfileIntW(L"RemotePlay", L"FineSampling", 1, ps5Settings().c_str())))}};
        if (!i.ps5Profile.empty())
            if (auto saved = remoteplay::loadProfile(directory / i.ps5Profile)) {
                i.ps5Form["host"] = QString::fromStdString(saved->host);
                i.ps5Form["account"] = QString::fromStdString(remoteplay::accountIdToBase64(saved->credentials.accountId));
                i.ps5Form["quality"] = (saved->video.height == 1080 ? 2 : 0) + (saved->video.fps == 60 ? 1 : 0);
                i.ps5Form["codec"] = int(saved->video.codec);
                const auto found = std::find(std::begin(kPs5Bitrates), std::end(kPs5Bitrates), saved->video.bitrateKbps);
                i.ps5Form["bitrate"] = found == std::end(kPs5Bitrates) ? 2 : int(found - std::begin(kPs5Bitrates));
                i.ps5Form["viewOnly"] = saved->viewOnly;
            }
        // Oviora: the settings page's choices win over what a pairing last used
        for (const auto* key : {L"quality", L"codec", L"bitrate", L"viewOnly"}) {
            const int v = int(GetPrivateProfileIntW(L"Stream", key, -1, ps5Settings().c_str()));
            if (v >= 0) i.ps5Form[QString::fromWCharArray(key)] = QString::fromWCharArray(key) == QLatin1String("viewOnly") ? QVariant(v != 0) : QVariant(v);
        }
        // Old Oviora's "Otomatik" bit rate: the resolution's own rate (10 Mbps at 720p, 15 at
        // 1080p) until the slider is moved; a home set saved with a bit rate keeps it.
        const int homeAuto = int(GetPrivateProfileIntW(L"Stream", L"bitrateAuto", -1, ps5Settings().c_str()));
        i.ps5Form["bitrateAuto"] = homeAuto >= 0 ? homeAuto != 0 : GetPrivateProfileIntW(L"Stream", L"bitrate", 100, ps5Settings().c_str()) == 100;
        // The internet set (old Oviora's "Internet" card). Match 1913's remote default
        // (720p, 60 fps, H.265) with automatic bit rate (8 Mbps, see ps5Video) so frames
        // stay smaller per burst on long lines. A value the user picked in [StreamInternet] wins.
        const auto internet = [](const wchar_t* key, int fallback, int high) {
            const int v = int(GetPrivateProfileIntW(L"StreamInternet", key, -1, ps5Settings().c_str()));
            return v >= 0 && v <= high ? v : fallback;
        };
        i.ps5Form["remoteQuality"] = internet(L"quality", 1, 3);
        i.ps5Form["remoteCodec"] = internet(L"codec", 1, 2);
        i.ps5Form["remoteBitrate"] = internet(L"bitrate", 1, 7);
        i.ps5Form["remoteBitrateAuto"] = internet(L"bitrateAuto", 1, 1) != 0;
        // Yayın → Uyarlanabilir kalite (old adaptiveQuality), both sets. Not set by the user: off
        // at home, on over the internet (ps5Connect); the toggle shows the home value.
        const int adaptive = int(GetPrivateProfileIntW(L"Stream", L"adaptive", -1, ps5Settings().c_str()));
        i.ps5Form["adaptive"] = adaptive > 0;
        i.ps5Form["adaptiveSet"] = adaptive >= 0;
        // Yayın → Akıcılık tamponu: 1 Otomatik (on for internet streams), 0 Kapalı
        i.ps5Form["smoothing"] = GetPrivateProfileIntW(L"Stream", L"smoothing", 1, ps5Settings().c_str()) != 0;
        if (i.ps5Status.isEmpty())
            i.ps5Status = failures ? tr("有配对存档无法读取或解密，详情见日志。请使用原 Windows 账户；不要删除原件。")
                        : !i.ps5Profile.empty() ? tr("已加载保存的主机，直接点“连接”，无需重新填写 8 位配对码。")
                        : tr("首次使用：PS5 设置 → 系统 → 远程游玩 → 关联设备，填写账号 ID 与 8 位配对码后配对。");
        oviora::log::info("remoteplay-ui", std::format("profiles={} readFailures={} folder={}{}", i.ps5ProfileList.size(), failures,
            QString::fromStdWString(directory.wstring()).toStdString(), remoteplay::profileDirectoryIsTestInstall() ? " (test install)" : ""));
    } catch (...) {
        impl_->ps5Status = tr("无法读取远程游玩目录；原有配对文件不会被改动。");
    }
#else
    impl_->ps5Status = tr("此版本未包含 PS5 远程游玩组件。");
#endif
    emit ps5Changed();
    emit lanChanged();
}
bool QmlPlayerBridge::ps5Set(const QString& key, const QVariant& value) {
    static const QHash<QString, std::pair<int, int>> ranges{{"quality", {0, 3}}, {"codec", {0, 2}}, {"bitrate", {0, 7}}, {"decode", {0, 2}}, {"sampling", {0, 1}},
                                                            {"remoteQuality", {0, 3}}, {"remoteCodec", {0, 2}}, {"remoteBitrate", {0, 7}}};
    if (ranges.contains(key)) {
        bool ok = false; const int n = value.toInt(&ok); const auto [low, high] = ranges.value(key);
        if (!ok || n < low || n > high) return false;
        impl_->ps5Form[key] = n;
        // Moving the bit rate slider leaves "Otomatik" (old Oviora)
        if (key == QLatin1String("bitrate") || key == QLatin1String("remoteBitrate")) {
            const bool home = key == QLatin1String("bitrate");
            impl_->ps5Form[home ? QStringLiteral("bitrateAuto") : QStringLiteral("remoteBitrateAuto")] = false;
#ifdef OVIORA_ENABLE_REMOTEPLAY
            WritePrivateProfileStringW(home ? L"Stream" : L"StreamInternet", L"bitrateAuto", L"0", ps5Settings().c_str());
#endif
        }
    } else if (key == QLatin1String("bitrateAuto") || key == QLatin1String("remoteBitrateAuto")) {
        impl_->ps5Form[key] = value.toBool();
#ifdef OVIORA_ENABLE_REMOTEPLAY
        WritePrivateProfileStringW(key == QLatin1String("bitrateAuto") ? L"Stream" : L"StreamInternet", L"bitrateAuto", value.toBool() ? L"1" : L"0", ps5Settings().c_str());
#endif
    } else if (key == QLatin1String("viewOnly")) impl_->ps5Form[key] = value.toBool();
    else if (key == QLatin1String("adaptive") || key == QLatin1String("smoothing")) {
        impl_->ps5Form[key] = value.toBool();
        if (key == QLatin1String("adaptive")) impl_->ps5Form["adaptiveSet"] = true;
#ifdef OVIORA_ENABLE_REMOTEPLAY
        WritePrivateProfileStringW(L"Stream", key.toStdWString().c_str(), value.toBool() ? L"1" : L"0", ps5Settings().c_str());
#endif
    }
    else if (key == QLatin1String("host") || key == QLatin1String("account")) impl_->ps5Form[key] = value.toString().trimmed();
    else return false;
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (key == QLatin1String("quality") || key == QLatin1String("codec") || key == QLatin1String("bitrate") || key == QLatin1String("viewOnly"))
        WritePrivateProfileStringW(L"Stream", key.toStdWString().c_str(), std::to_wstring(impl_->ps5Form.value(key).toInt()).c_str(), ps5Settings().c_str());
    if (key == QLatin1String("remoteQuality") || key == QLatin1String("remoteCodec") || key == QLatin1String("remoteBitrate")) {
        const std::wstring name = key.mid(6).toLower().toStdWString(); // remoteQuality -> quality
        WritePrivateProfileStringW(L"StreamInternet", name.c_str(), std::to_wstring(impl_->ps5Form.value(key).toInt()).c_str(), ps5Settings().c_str());
    }
    if (key == QLatin1String("decode"))
        WritePrivateProfileStringW(L"RemotePlay", L"DecodeMode", std::to_wstring(impl_->ps5Form.value(key).toInt()).c_str(), ps5Settings().c_str());
    if (key == QLatin1String("sampling"))
        WritePrivateProfileStringW(L"RemotePlay", L"FineSampling", impl_->ps5Form.value(key).toInt() ? L"1" : L"0", ps5Settings().c_str());
#endif
    emit ps5Changed();
    return true;
}
void QmlPlayerBridge::ps5SelectProfile(const QString& id) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (impl_->ps5Busy) return;
    impl_->ps5Profile = id.toStdWString();
    WritePrivateProfileStringW(L"RemotePlay", L"LastProfile", impl_->ps5Profile.c_str(), ps5Settings().c_str());
    impl_->ps5Status.clear();
    ps5Load();
#else
    Q_UNUSED(id);
#endif
}
#ifdef OVIORA_ENABLE_REMOTEPLAY
namespace {
// The home-network set, or the internet one (old Oviora's two cards on the Yayin page).
remoteplay::VideoProfile ps5Video(const QVariantMap& f, bool internet = false) {
    const auto value = [&](const char* home, const char* remote) { return f.value(internet ? remote : home).toInt(); };
    remoteplay::VideoProfile p; const int q = value("quality", "remoteQuality");
    p.width = q < 2 ? 1280 : 1920; p.height = q < 2 ? 720 : 1080; p.fps = (q == 0 || q == 2) ? 30 : 60;
    p.codec = static_cast<remoteplay::Codec>(std::clamp(value("codec", "remoteCodec"), 0, 2));
    // "Otomatik": at home the lib's preset rate for the resolution (10 Mbps at 720p, 15 at 1080p);
    // over the internet 8 Mbps, which a lossy long line carries with room to spare.
    const bool automatic = f.value(internet ? "remoteBitrateAuto" : "bitrateAuto").toBool();
    p.bitrateKbps = automatic ? (internet ? 8000u : p.height == 1080 ? 15000u : 10000u) : kPs5Bitrates[std::clamp(value("bitrate", "remoteBitrate"), 0, 7)];
    return p;
}
std::string asciiOnly(const QString& s) {
    std::string out;
    for (const QChar c : s) { if (c.unicode() > 127) return {}; out.push_back(char(c.unicode())); }
    return out;
}
// A new pairing file with a random name; empty when no name could be made.
std::filesystem::path newProfilePath() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) return {};
    std::string name; const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
    for (size_t k = 0; k < sizeof(guid); ++k) name += std::format("{:02x}", bytes[k]);
    return remoteplay::profileDirectory() / (name + ".dat");
}
bool validConsoleId(const QString& id) {
    return id.size() == 12 && std::all_of(id.begin(), id.end(), [](QChar c) { return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'); });
}
// The account keeps the 16-byte keys as 32 lowercase hex characters.
std::string keyHex(const void* data) {
    std::string out; const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t k = 0; k < 16; ++k) out += std::format("{:02x}", bytes[k]);
    return out;
}
bool keyFromHex(const QString& hex, void* out) {
    const auto nibble = [](QChar c) -> int {
        const auto u = c.unicode();
        return u >= '0' && u <= '9' ? u - '0' : u >= 'a' && u <= 'f' ? u - 'a' + 10 : u >= 'A' && u <= 'F' ? u - 'A' + 10 : -1;
    };
    if (hex.size() != 32) return false;
    auto* bytes = static_cast<unsigned char*>(out);
    for (int k = 0; k < 16; ++k) {
        const int high = nibble(hex[2 * k]), low = nibble(hex[2 * k + 1]);
        if (high < 0 || low < 0) return false;
        bytes[k] = static_cast<unsigned char>(high * 16 + low);
    }
    return true;
}
// One pairing as the account stores it (local_console_put). Never logged.
QJsonObject accountConsole(const remoteplay::NativeConnectRequest& r, const QString& name) {
    std::string regist = keyHex(r.credentials.registrationKey.data()), rp = keyHex(r.credentials.sessionKey.data());
    const bool account = std::any_of(r.credentials.accountId.begin(), r.credentials.accountId.end(), [](auto b) { return b != 0; });
    QJsonObject c{{"id", QString::fromStdString(r.consoleId)}, {"name", name.left(64)},
                  {"regist_key", QString::fromLatin1(regist.data(), qsizetype(regist.size()))},
                  {"rp_key", QString::fromLatin1(rp.data(), qsizetype(rp.size()))}, {"rp_key_type", 0},
                  {"account_id", account ? QString::fromStdString(remoteplay::accountIdToBase64(r.credentials.accountId)) : QString()},
                  {"host", QString::fromStdString(r.host)}, {"ps5", true}};
    SecureZeroMemory(regist.data(), regist.size()); SecureZeroMemory(rp.data(), rp.size());
    return c;
}
}
#endif
// One worker at a time, as the 1.4.4 panel; results are picked up in tickPs5().
#define OVIORA_PS5_LAUNCH(body) \
    do { if (impl_->ps5Busy) return; if (impl_->ps5Worker.joinable()) impl_->ps5Worker.join(); \
         impl_->ps5Busy = true; impl_->ps5Done = false; impl_->ps5Status = tr("正在处理…可随时取消"); emit ps5Changed(); \
         impl_->ps5Worker = std::jthread([this, fn = std::move(body)](std::stop_token stop) mutable { \
             Impl::Ps5Outcome result; \
             try { result = fn(stop); } catch (...) { result.message = L"操作失败；请检查网络、输入和可用磁盘空间。"; } \
             { std::lock_guard lock(impl_->ps5Mutex); impl_->ps5Outcome = std::move(result); } impl_->ps5Done = true; }); } while (0)
void QmlPlayerBridge::ps5Scan() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto work = [](std::stop_token stop) {
        Impl::Ps5Outcome out; auto report = remoteplay::discoverLocalPs5(stop);
        for (const auto& line : report.diagnostics) oviora::log::info("remoteplay-discovery", line);
        for (auto& h : report.hosts) {
            std::string id = h.consoleId;
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            out.hosts.push_back({h.host, id});
        }
        out.message = stop.stop_requested() ? L"已取消查找" : out.hosts.empty()
            ? (report.error ? std::format(L"主机搜索发生错误（{}），详情见日志；可手填 IP。", report.error) : L"搜索完成，未发现 PS5；确认同一局域网，或手动填写 IP。")
            : std::format(L"发现 {} 台 PS5。", out.hosts.size());
        return out;
    };
    OVIORA_PS5_LAUNCH(work);
#endif
}
void QmlPlayerBridge::ps5Pair(const QString& pinText) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto host = asciiOnly(impl_->ps5Form.value("host").toString()), id = asciiOnly(impl_->ps5Form.value("account").toString()), pin = asciiOnly(pinText);
    auto account = remoteplay::accountIdFromBase64(id); if (!account) account = remoteplay::accountIdFromDecimal(id);
    if (!remoteplay::validHost(host) || !account || !remoteplay::parsePairingPin(pin)) {
        impl_->ps5Status = tr("请填写有效主机地址、Account ID 和 8 位配对码。"); emit ps5Changed(); return;
    }
    // Same host + account reuses its file; otherwise a fresh random name. Editing a registered
    // console replaces its file, so a pairing for another PSN account changes the account.
    std::filesystem::path target = impl_->ps5EditTarget;
    if (target.empty()) for (const auto& p : impl_->ps5ProfileList) {
        const auto path = remoteplay::profileDirectory() / p.toMap().value("id").toString().toStdWString();
        if (auto saved = remoteplay::loadProfile(path); saved && saved->host == host && saved->credentials.accountId == *account) target = path;
    }
    if (target.empty()) {
        target = newProfilePath();
        if (target.empty()) { impl_->ps5Status = tr("无法分配配对存档，请重试。"); emit ps5Changed(); return; }
    }
    const auto format = ps5Video(impl_->ps5Form);
    auto work = [host = std::move(host), account = *account, pin = std::move(pin), format, target](std::stop_token stop) mutable {
        auto result = remoteplay::pairLocalPs5(host, account, pin, stop); SecureZeroMemory(pin.data(), pin.size());
        Impl::Ps5Outcome out;
        if (result.result.ok && !stop.stop_requested()) {
            remoteplay::NativeConnectRequest request; request.host = host; request.video = format; request.credentials = std::move(result.credentials);
            for (auto byte : result.mac) request.consoleId += std::format("{:02x}", byte);
            if (request.consoleId == "000000000000") request.consoleId.clear();
            if (remoteplay::saveProfile(target, request)) { out.savedPath = target.filename().wstring(); out.message = L"配对成功并已加密保存；现在可以连接。"; }
            else out.message = L"配对成功，但保存失败；请检查用户目录权限。";
        } else out.message = result.canceled || stop.stop_requested() ? L"已取消配对" : std::format(L"配对失败（{}），检查 PS5 配对码、Account ID 与网络。", result.result.code);
        return out;
    };
    OVIORA_PS5_LAUNCH(work);
#else
    Q_UNUSED(pinText);
#endif
}
bool QmlPlayerBridge::ps5Connect() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    auto psn = std::move(i.pendingPsn); i.pendingPsn.reset();
    if (i.ps5Busy) return false;
    source::RemotePlayConnectDesc desc;
    std::string host;
    if (psn) {
        // Oviora: over PSN, no pairing on this PC; the label is the console's name.
        host = asciiOnly(i.pendingPsnName);
        desc.request.video = ps5Video(i.ps5Form, true);
        desc.request.psn = std::move(psn);
    } else {
    const auto path = i.ps5Profile.empty() ? std::filesystem::path{} : remoteplay::profileDirectory() / i.ps5Profile;
    auto saved = path.empty() ? std::nullopt : remoteplay::loadProfile(path);
    host = asciiOnly(i.ps5Form.value("host").toString());
    if (!saved || !remoteplay::validHost(host)) { i.ps5Status = tr("请先完成配对，并填写有效主机地址。"); emit ps5Changed(); return false; }
    saved->host = host; saved->video = ps5Video(i.ps5Form); saved->viewOnly = i.ps5Form.value("viewOnly").toBool();
    if (!remoteplay::saveProfile(path, *saved)) { i.ps5Status = tr("保存连接设置失败，请检查目录权限。"); emit ps5Changed(); return false; }
    desc.request = std::move(*saved);
    }
    const bool overPsn = desc.request.psn != nullptr;
    // Uyarlanabilir kalite: the user's choice; never set, on for internet streams only.
    // OVIORA_TEST_ADAPTIVE=1: on for a test run without touching the setting.
    // Off unless the user turns it on: not yet tried over PSN on a real PS5 (an untested message once broke its Remote Play)
    const bool adaptive = i.ps5Form.value("adaptiveSet").toBool() && i.ps5Form.value("adaptive").toBool();
    desc.request.adaptiveResolution = adaptive || qEnvironmentVariableIntValue("OVIORA_TEST_ADAPTIVE") == 1;
    // Akıcılık tamponu "Otomatik": a frame or two of hold for internet streams, none at home.
    desc.smoothing = overPsn && i.ps5Form.value("smoothing").toBool();
    // OVIORA_TEST_PS5_720P=1: a 720p request for a test run, the saved settings untouched.
    if (qEnvironmentVariableIntValue("OVIORA_TEST_PS5_720P") == 1) {
        desc.request.video.width = 1280; desc.request.video.height = 720;
        desc.request.video.bitrateKbps = std::min(desc.request.video.bitrateKbps, 10000u);
        oviora::log::info("remoteplay-ui", "test switch: 720p request");
    }
    if (desc.request.adaptiveResolution)
        oviora::log::info("remoteplay-ui", std::format("adaptive quality: the console is offered a lower resolution too{}",
            adaptive ? "" : " (test switch)"));
    const int decode = std::clamp(i.ps5Form.value("decode").toInt(), 0, 2);
    desc.decodeMode = static_cast<source::RemotePlayConnectDesc::DecodeMode>(decode);
    desc.highQualitySampling = i.ps5Form.value("sampling").toInt() == 1;
    WritePrivateProfileStringW(L"RemotePlay", L"DecodeMode", std::to_wstring(decode).c_str(), ps5Settings().c_str());
    WritePrivateProfileStringW(L"RemotePlay", L"FineSampling", desc.highQualitySampling ? L"1" : L"0", ps5Settings().c_str());
    const bool viewOnly = desc.request.viewOnly;
    flushPreferences();
    // A new attempt: the connecting screen starts over, an old error card goes
    i.connectionPsn = overPsn; i.connectionSeen = false; i.connectionError.clear();
    // The session id is cleared with the flag: the previous session's id left over here
    // ended "opening" on the next snapshot while the old source still ran.
    i.openingSource = true; i.openingSessionId = 0; i.sourceLabel = L"PS5 · " + std::wstring(host.begin(), host.end());
    auto request = std::make_shared<source::RemotePlayConnectDesc>(std::move(desc));
    openAfterCinema(tr("正在连接 PS5 · %1").arg(QString::fromStdString(host)), [this, request, viewOnly] {
        auto& i = *impl_;
        if (i.preOpen) i.preOpen();
        i.engine.previewView({});
        i.engine.openRemotePlay(i.videoWindow, std::move(*request), i.options);
        i.openingSessionId = i.engine.snapshot().sessionId;
        refreshSubtitles({});
        // Controller forwarding at 8 ms, as 1.4.4; view-only leaves the pad to the PS5.
        // Started with the session: the poll stops the pad when no session runs.
        if (!viewOnly) {
            if (!i.acquirePad(Impl::PadPs5)) oviora::log::warn("remoteplay-input", "SDL gamepad initialization failed");
            loadGameInput();
            if (!i.controllerTimer) {
                i.controllerTimer = new QTimer(this);
                connect(i.controllerTimer, &QTimer::timeout, this, [this] {
                    auto& j = *impl_;
                    const auto s = j.engine.snapshot();
                    if (s.remotePlay && (s.running || s.transport == engine::TransportState::Opening)) {
                        pollController(); // pad + keyboard play (bridge/GameInput.cpp)
                    } else {
                        j.engine.remotePlayController({}); j.releasePad(Impl::PadPs5); j.controllerTimer->stop();
                        releaseGameKeys(); j.menuComboHeld = false; updateMouseAim();
                    }
                });
            }
            i.controllerTimer->start(8);
        } else {
            if (i.controllerTimer) i.controllerTimer->stop();
            i.releasePad(Impl::PadPs5);
        }
    });
    i.ps5Watching = true;
    if (!overPsn) rememberSource(QStringLiteral("ps5"), QString::fromStdString(host));
    i.ps5Status = tr("连接已开始。需要登录 PIN 时在下方提交。关闭面板不停止串流。");
    {
        const auto v = ps5Video(i.ps5Form, overPsn);
        oviora::log::info("remoteplay-ui", std::format("connect set={} {}p{} codec={} bitrate={}kbps{} decode={} viewOnly={} adaptive={} smoothing={}", overPsn ? "internet" : "home",
            v.height, v.fps, int(v.codec), v.bitrateKbps, i.ps5Form.value(overPsn ? "remoteBitrateAuto" : "bitrateAuto").toBool() ? " (auto)" : "", decode, viewOnly,
            adaptive, overPsn && i.ps5Form.value("smoothing").toBool()));
    }
    emit ps5Changed();
    return true;
#else
    return false;
#endif
}
bool QmlPlayerBridge::connectPsn(std::shared_ptr<const void> route, const QString& nickname) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (!route) return false;
    if (impl_->ps5Form.isEmpty()) ps5Load();
    impl_->pendingPsn = std::static_pointer_cast<const remoteplay::PsnRoute>(route);
    impl_->pendingPsnName = nickname.isEmpty() ? QStringLiteral("PSN") : nickname;
    impl_->connectionName = nickname;
    return ps5Connect();
#else
    Q_UNUSED(route); Q_UNUSED(nickname);
    return false;
#endif
}
bool QmlPlayerBridge::remotePlayActive() const {
    const auto s = impl_->engine.snapshot();
    return s.remotePlay && (s.running || s.transport == engine::TransportState::Opening);
}
// --- Oviora home: PS5s on this network ------------------------------------------------------------
QVariantList QmlPlayerBridge::lanConsoles() const {
    QVariantList out;
#ifdef OVIORA_ENABLE_REMOTEPLAY
    const auto& i = *impl_;
    std::vector<bool> used(i.lanFound.size(), false);
    auto foundFor = [&](const QString& id, const QString& host) -> int {
        for (size_t n = 0; n < i.lanFound.size(); ++n) {
            const auto& f = i.lanFound[n];
            if ((!id.isEmpty() && QString::fromStdString(f.consoleId).compare(id, Qt::CaseInsensitive) == 0) ||
                (id.isEmpty() && QString::fromStdString(f.host) == host)) return int(n);
        }
        return -1;
    };
    // Konsollar -> Gizlenen konsollar: a hidden console found on this network is not displayed
    QSet<QString> hidden;
    for (const auto& v : i.prefs.value(QStringLiteral("hiddenConsoles")).toList()) hidden.insert(v.toMap().value("id").toString());
    const QString autoConnect = i.prefString("autoConnect");
    auto entry = [&hidden, &autoConnect](const QString& key, const QString& name, const QString& address, const remoteplay::DiscoveredConsole* f, bool registered) {
        const QString id = f ? (f->consoleId.empty() ? QString::fromStdString(f->host) : QString::fromStdString(f->consoleId).toLower()) : QString();
        return QVariantMap{{"key", key}, {"name", name.isEmpty() ? QStringLiteral("PS5") : name}, {"address", address},
                           {"state", f ? (f->standby ? QStringLiteral("standby") : QStringLiteral("ready")) : QString()},
                           {"discovered", f != nullptr}, {"registered", registered},
                           {"app", f ? QString::fromStdString(f->runningApp) : QString()},
                           // The running game's title id (the subtitles' shared memory keeps lines per game)
                           {"titleId", f ? QString::fromStdString(f->runningTitleId) : QString()},
                           {"display", registered || id.isEmpty() || !hidden.contains(id)}, {"id", id},
                           // The name the PS5 announces itself (the same as on PSN), whatever the user calls it here
                           {"hostName", f ? QString::fromStdString(f->name) : QString()},
                           {"autoConnect", registered && key == autoConnect},
                           {"duid", QString()}, {"manual", false}};
    };
    // Konsolu düzenle → Ad: a name the user gave a registered console, over the one it announces
    const QVariantMap names = i.prefs.value(QStringLiteral("consoleNames")).toMap();
    for (const auto& v : i.ps5ProfileList) {
        const auto p = v.toMap();
        const int n = foundFor(p.value("consoleId").toString(), p.value("label").toString());
        const remoteplay::DiscoveredConsole* f = n >= 0 ? &i.lanFound[size_t(n)] : nullptr;
        if (f) used[size_t(n)] = true;
        const QString own = names.value(p.value("id").toString()).toString();
        out << entry(p.value("id").toString(), !own.isEmpty() ? own : f ? QString::fromStdString(f->name) : QString(),
                     f ? QString::fromStdString(f->host) : p.value("label").toString(), f, true);
    }
    for (size_t n = 0; n < i.lanFound.size(); ++n)
        if (!used[n]) out << entry(QStringLiteral("new:") + QString::fromStdString(i.lanFound[n].host), QString::fromStdString(i.lanFound[n].name),
                                   QString::fromStdString(i.lanFound[n].host), &i.lanFound[n], false);
#endif
    return out;
}
bool QmlPlayerBridge::lanSearch() const { return impl_->lanSearch; }
void QmlPlayerBridge::setLanSearch(bool on) {
    if (impl_->lanSearch == on) return;
    impl_->lanSearch = on;
    if (!on) { impl_->lanFound.clear(); }
    impl_->lanNext = {};
    emit lanChanged();
}
void QmlPlayerBridge::tickLan() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (!i.lanLoaded) { i.lanLoaded = true; ps5Load(); emit lanChanged(); }
    if (i.lanWorker.joinable() && i.lanDone.load()) {
        i.lanWorker.join();
        { std::lock_guard lock(i.lanMutex); if (i.lanSearch || !i.wakeWaitKey.isEmpty()) i.lanFound = std::move(i.lanResult); i.lanResult.clear(); }
        emit lanChanged();
        tickWakeWait();
    }
    tickAutoConnect();
    const auto now = std::chrono::steady_clock::now();
    // Not while a PS5 session runs or the PS5 dialog works: the console has one remote play slot
    // and discovery answers would only cost the stream.
    const bool streaming = remotePlayActive();
    // Waiting for a woken console: searched every 2 s, also with the search switched off
    const bool waking = !i.wakeWaitKey.isEmpty();
    // While streaming only every 30 s: one small question, so the game running on the PS5 (whichever
    // account plays it) stays known for the friends' list and Discord
    if ((!i.lanSearch && !waking) || i.lanWorker.joinable() || i.ps5Busy || now < i.lanNext) return;
    i.lanNext = now + std::chrono::seconds(waking ? 2 : streaming ? 30 : 8);
    i.lanDone = false;
    i.lanWorker = std::jthread([&i](std::stop_token stop) {
        auto report = remoteplay::discoverLocalPs5(stop);
        { std::lock_guard lock(i.lanMutex); i.lanResult = std::move(report.hosts); }
        i.lanDone = true;
    });
#endif
}
bool QmlPlayerBridge::lanConnect(const QString& key) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (key.startsWith(QStringLiteral("new:"))) {
        // Not paired yet: the PS5 dialog pairs it (Account ID + the 8-digit code on the PS5)
        i.ps5Profile.clear();
        i.ps5Form["host"] = key.mid(4);
        emit ps5Changed();
        openPs5Dialog();
        return false;
    }
    ps5SelectProfile(key);
    for (const auto& v : lanConsoles()) {
        const auto c = v.toMap();
        if (c.value("key").toString() != key) continue;
        if (!c.value("address").toString().isEmpty()) i.ps5Form["host"] = c.value("address");
        i.connectionName = c.value("name").toString();  // the connecting screen names the console
    }
    return ps5Connect();
#else
    Q_UNUSED(key);
    return false;
#endif
}
void QmlPlayerBridge::lanWake(const QString& key) {
    ps5SelectProfile(key);
    ps5Wake();
#ifdef OVIORA_ENABLE_REMOTEPLAY
    // As the old Oviora: wake, wait until the console answers as ready, then connect by itself
    auto& i = *impl_;
    i.wakeWaitKey = key;
    i.wakeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    i.lanNext = {};
    for (const auto& v : lanConsoles()) if (v.toMap().value("key").toString() == key) i.connectionName = v.toMap().value("name").toString();
    i.connectionError.clear();
    emit navigate(QStringLiteral("min"));
    updateConnection();
#endif
}
void QmlPlayerBridge::tickWakeWait() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (i.wakeWaitKey.isEmpty()) return;
    for (const auto& v : lanConsoles()) {
        const auto c = v.toMap();
        if (c.value("key").toString() != i.wakeWaitKey || c.value("state").toString() != QLatin1String("ready")) continue;
        const QString key = i.wakeWaitKey;
        i.wakeWaitKey.clear();
        oviora::log::info("remoteplay-ui", "woken console is ready; connecting");
        lanConnect(key);
        return;
    }
    if (std::chrono::steady_clock::now() > i.wakeDeadline) {
        i.wakeWaitKey.clear();
        i.connectionError = tr("PS5 açılmadı. Konsolun bekleme modunda (turuncu ışık) olduğundan ve aynı ağda bulunduğundan emin ol.");
        oviora::log::info("remoteplay-ui", "woken console did not answer in 90 s");
        updateConnection();
    }
#endif
}
void QmlPlayerBridge::lanEdit(const QString& key) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (key.startsWith(QStringLiteral("new:")) || impl_->ps5Busy) return;
    ps5SelectProfile(key);
    if (impl_->ps5Profile.empty()) return;
    impl_->ps5EditTarget = remoteplay::profileDirectory() / impl_->ps5Profile;
    for (const auto& v : lanConsoles()) {
        const auto c = v.toMap();
        if (c.value("key").toString() == key && c.value("discovered").toBool()) impl_->ps5Form["host"] = c.value("address");
    }
    emit ps5Changed();
    emit navigate(QStringLiteral("ps5"));
#else
    Q_UNUSED(key);
#endif
}
// Address only (no new code): the saved pairing keeps its keys and account
bool QmlPlayerBridge::ps5SaveEdit() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (i.ps5EditTarget.empty() || i.ps5Busy) return false;
    auto saved = remoteplay::loadProfile(i.ps5EditTarget);
    const auto host = asciiOnly(i.ps5Form.value("host").toString());
    if (!saved || !remoteplay::validHost(host)) { i.ps5Status = tr("Geçerli bir konsol adresi gir."); emit ps5Changed(); return false; }
    saved->host = host;
    if (!remoteplay::saveProfile(i.ps5EditTarget, *saved)) { i.ps5Status = tr("Kaydedilemedi; klasör izinlerini kontrol et."); emit ps5Changed(); return false; }
    i.ps5Status = tr("Konsolun adresi kaydedildi.");
    ++i.pairRevision;
    ps5Load();
    emit ps5Changed();
    emit lanChanged();
    return true;
#else
    return false;
#endif
}
// The name shown for a registered console ("" = the name the console announces again)
void QmlPlayerBridge::lanRename(const QString& key, const QString& name) {
    if (key.isEmpty() || key.startsWith(QStringLiteral("new:"))) return;
    QVariantMap names = impl_->prefs.value(QStringLiteral("consoleNames")).toMap();
    const QString clean = name.simplified().left(40);
    if (names.value(key).toString() == clean) return;
    if (clean.isEmpty()) names.remove(key); else names.insert(key, clean);
    impl_->prefs.insert(QStringLiteral("consoleNames"), names);
    impl_->savePrefs();
    emit lanChanged();
}
QString QmlPlayerBridge::lanName(const QString& key) const {
    for (const auto& v : lanConsoles())
        if (v.toMap().value("key").toString() == key) return v.toMap().value("name").toString();
    return {};
}
void QmlPlayerBridge::ps5EndEdit() {
    if (impl_->ps5EditTarget.empty()) return;
    impl_->ps5EditTarget.clear();
    emit ps5Changed();
}
void QmlPlayerBridge::lanForget(const QString& key) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (key.startsWith(QStringLiteral("new:"))) return;
    // A deleted console no longer connects by itself at start
    if (impl_->prefString("autoConnect") == key) setPreference(QStringLiteral("autoConnect"), QString());
    lanRename(key, QString());
    ps5SelectProfile(key);
    ps5Forget();
    emit lanChanged();
#else
    Q_UNUSED(key);
#endif
}
void QmlPlayerBridge::ps5Wake() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto saved = impl_->ps5Profile.empty() ? std::nullopt : remoteplay::loadProfile(remoteplay::profileDirectory() / impl_->ps5Profile);
    if (!saved) { impl_->ps5Status = tr("需要先配对才能唤醒。"); emit ps5Changed(); return; }
    saved->host = asciiOnly(impl_->ps5Form.value("host").toString());
    auto work = [request = std::move(*saved)](std::stop_token stop) {
        Impl::Ps5Outcome out; if (stop.stop_requested()) { out.message = L"已取消唤醒"; return out; }
        auto r = remoteplay::wakeLocalPs5(request);
        out.message = r.ok ? L"已发送唤醒请求，等待 PS5 开机后再连接。" : std::format(L"唤醒失败（{}）；确认 PS5 处于待机而不是关机。", r.code);
        return out;
    };
    OVIORA_PS5_LAUNCH(work);
#endif
}
void QmlPlayerBridge::ps5Forget() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (impl_->ps5Busy || impl_->ps5Profile.empty()) return;
    const auto path = remoteplay::profileDirectory() / impl_->ps5Profile;
    const auto saved = remoteplay::loadProfile(path);
    const QString consoleId = saved ? QString::fromStdString(saved->consoleId) : QString();
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    impl_->ps5Status = removed ? tr("配对已删除。现有串流会保留；下次连接需选择其他主机或重新配对。") : tr("删除失败，请检查目录权限。");
    if (removed) impl_->ps5Profile.clear();
    ps5Load();
    // The account forgets the console too, unless another pairing here is for the same one
    bool kept = false;
    for (const auto& p : impl_->ps5ProfileList) kept = kept || p.toMap().value("consoleId").toString() == consoleId;
    if (removed && validConsoleId(consoleId) && !kept && impl_->accountCall)
        impl_->accountCall(QStringLiteral("local_console_remove"), QJsonObject{{"id", consoleId}}, [id = consoleId.toStdString()](const QJsonObject&, const QString& error) {
            oviora::log::info("console-sync", error.isEmpty() ? std::format("removed {}", id) : std::format("remove {} failed: {}", id, error.toStdString()));
        });
#endif
}
// --- Oviora account: PS5s paired on the home network ----------------------------------------------
void QmlPlayerBridge::setAccountCall(AccountCall call) { impl_->accountCall = std::move(call); }
void QmlPlayerBridge::syncAccountConsoles() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (!impl_->accountCall) return;
    impl_->accountCall(QStringLiteral("local_consoles"), {}, [self = QPointer<QmlPlayerBridge>(this)](const QJsonObject& reply, const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) { oviora::log::info("console-sync", std::format("list failed: {}", error.toStdString())); return; }
        self->takeAccountConsoles(reply.value("consoles").toArray());
    });
#endif
}
void QmlPlayerBridge::putAccountConsole(const QString& profileId) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (!impl_->accountCall) return;
    auto saved = remoteplay::loadProfile(remoteplay::profileDirectory() / profileId.toStdWString());
    if (!saved || saved->consoleId.size() != 12) return;
    impl_->accountCall(QStringLiteral("local_console_put"), QJsonObject{{"console", accountConsole(*saved, lanName(profileId))}},
        [id = saved->consoleId](const QJsonObject&, const QString& error) {
            oviora::log::info("console-sync", error.isEmpty() ? std::format("saved {}", id) : std::format("save {} failed: {}", id, error.toStdString()));
        });
#else
    Q_UNUSED(profileId);
#endif
}
void QmlPlayerBridge::takeAccountConsoles(const QJsonArray& consoles) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    try {
        ps5Load();  // the pairings on disk now
        QSet<QString> local, remote;
        for (const auto& p : i.ps5ProfileList) local.insert(p.toMap().value("consoleId").toString());
        unsigned added = 0, skipped = 0;
        for (const auto& v : consoles) {
            const auto c = v.toObject();
            const QString id = c.value("id").toString().toLower();
            if (!validConsoleId(id)) { ++skipped; continue; }
            remote.insert(id);
            if (local.contains(id)) continue;  // a pairing made here is never replaced
            // Paired on another device: the same keys work here, at the console's last address
            remoteplay::NativeConnectRequest r;
            r.host = asciiOnly(c.value("host").toString()); r.consoleId = id.toStdString(); r.video = ps5Video(i.ps5Form);
            const QString account = c.value("account_id").toString();
            const auto accountId = account.isEmpty() ? std::optional(remoteplay::AccountId{}) : remoteplay::accountIdFromBase64(account.toStdString());
            if (accountId) r.credentials.accountId = *accountId;
            const auto path = newProfilePath();
            if (!accountId || !keyFromHex(c.value("regist_key").toString(), r.credentials.registrationKey.data()) ||
                !keyFromHex(c.value("rp_key").toString(), r.credentials.sessionKey.data()) || path.empty() || !remoteplay::saveProfile(path, r)) {
                ++skipped; continue;
            }
            local.insert(id); ++added;
            const QString name = c.value("name").toString().simplified();
            if (!name.isEmpty() && name != QLatin1String("PS5")) lanRename(QString::fromStdWString(path.filename().wstring()), name);
        }
        if (added) ps5Load();
        // Pairings made before the account kept them (or while signed out) go up now
        unsigned uploads = 0;
        for (const auto& v : i.ps5ProfileList) {
            const auto p = v.toMap();
            const QString id = p.value("consoleId").toString();
            if (!validConsoleId(id) || remote.contains(id)) continue;
            remote.insert(id); ++uploads;
            putAccountConsole(p.value("id").toString());
        }
        oviora::log::info("console-sync", std::format("account={} added={} skipped={} uploading={}", consoles.size(), added, skipped, uploads));
    } catch (...) {
        oviora::log::warn("console-sync", "remote play folder unavailable; account consoles not merged");
    }
#else
    Q_UNUSED(consoles);
#endif
}
void QmlPlayerBridge::ps5OpenAccountChecker() {
    // Fixed public URL; no account IDs, pairing credentials or login tokens leave Oviora.
    const bool opened=QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.psntools.com/psn/checker")));
    impl_->ps5Status=opened
        ? tr("在网页获取 PSN Account ID 后，复制到配对栏，再填写主机的 8 位配对码。")
        : tr("无法打开浏览器，请检查默认浏览器设置。");
    emit ps5Changed();
}
void QmlPlayerBridge::ps5SendLoginPin(const QString& pin) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    const auto text = pin.trimmed();
    if (text.isEmpty() || !std::all_of(text.begin(), text.end(), [](QChar c) { return c.isDigit(); })) {
        impl_->ps5Status = tr("登录 PIN 必须为数字。"); emit ps5Changed(); return;
    }
    impl_->engine.remotePlayLoginPin(text.toStdString());
#else
    (void)pin; // RemotePlay disabled: keep the QML-callable symbol but no backend.
#endif
}
void QmlPlayerBridge::ps5Cancel() {
    // Cancelling while waiting for a woken console only stops the wait
    if (!impl_->wakeWaitKey.isEmpty()) { impl_->wakeWaitKey.clear(); updateConnection(); return; }
    if (impl_->ps5Busy && impl_->ps5Worker.joinable()) { impl_->ps5Worker.request_stop(); return; }
    if (impl_->snapshot.remotePlay || impl_->openingSource || !impl_->pendingLiveText.isEmpty()) stopPlayback();
}
void QmlPlayerBridge::ps5Calibrate() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    impl_->ps5Status = impl_->controller.calibrate() ? tr("将手柄平放并保持静止：采集 120 个稳定样本完成校准；10 秒内不稳定则保留原校准。")
                                                     : tr("请先连接串流及带陀螺仪的电脑手柄。仅观看模式不使用电脑手柄。");
    emit ps5Changed();
#endif
}
void QmlPlayerBridge::tickPs5() {
    tickLan();
    tickStreamSession();
    auto& i = *impl_;
    bool changed = false;
    if (i.ps5Busy && i.ps5Done.load()) {
        if (i.ps5Worker.joinable()) i.ps5Worker.join();
        i.ps5Busy = false;
        Impl::Ps5Outcome r; { std::lock_guard lock(i.ps5Mutex); r = std::move(i.ps5Outcome); }
        i.ps5Status = oviora::ui::i18n::text(r.message);
#ifdef OVIORA_ENABLE_REMOTEPLAY
        if (!r.savedPath.empty()) {
            ++i.pairRevision;
            i.ps5Profile = r.savedPath;
            WritePrivateProfileStringW(L"RemotePlay", L"LastProfile", i.ps5Profile.c_str(), ps5Settings().c_str());
            const auto status = i.ps5Status; ps5Load(); i.ps5Status = status;
            putAccountConsole(QString::fromStdWString(r.savedPath));  // kept in the Oviora account too
        }
        if (!r.hosts.empty()) {
            // A found console updates the selected pairing's address only when its
            // id matches (or the legacy profile had no id and the same host).
            const auto path = i.ps5Profile.empty() ? std::filesystem::path{} : remoteplay::profileDirectory() / i.ps5Profile;
            auto selected = path.empty() ? std::nullopt : remoteplay::loadProfile(path);
            bool matched = false;
            if (selected) for (const auto& [host, id] : r.hosts) {
                if ((!selected->consoleId.empty() && selected->consoleId == id) || (selected->consoleId.empty() && selected->host == host)) {
                    selected->host = host; if (id.size() == 12) selected->consoleId = id;
                    if (remoteplay::saveProfile(path, *selected)) { i.ps5Form["host"] = QString::fromStdString(host); matched = true; }
                    break;
                }
            }
            if (!selected && r.hosts.size() == 1) i.ps5Form["host"] = QString::fromStdString(r.hosts.front().first);
            else if (selected && !matched) i.ps5Status = tr("发现主机，但无法确认是已配对的这一台。保留原地址，可核对后手动修改。");
            else if (!selected && r.hosts.size() > 1) i.ps5Status = tr("发现多台 PS5，请填写要配对的主机 IP，避免选错主机。");
        }
#endif
        changed = true;
    }
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (i.ps5Watching && !i.ps5Busy) {
        const auto& s = i.snapshot;
        const bool active = s.remotePlay && (s.running || s.transport == engine::TransportState::Opening || s.transport == engine::TransportState::Stopping);
        QString message = oviora::ui::i18n::text(s.failed ? s.status : s.remoteRecovering ? s.remoteRecoveryMessage
                          : s.remotePlay && s.running ? std::wstring(L"PS5 画面已进入播放；关闭此面板不停止串流。") : active ? s.status : std::wstring(L"PS5 串流已停止，可以重新连接。"));
        if (s.remotePlay && s.running && !s.failed && !s.remoteRecovering) {
            const auto& r = s.remoteStream; const auto& p = r.requestedProfile;
            message = oviora::ui::i18n::text(std::format(L"生效：{}p / {} fps / {} / 请求 {} Mbps · 实收视频 {:.1f} Mbps", p.height, p.fps,
                p.codec == remoteplay::Codec::H264 ? L"H.264" : p.codec == remoteplay::Codec::H265Hdr ? L"H.265 HDR" : L"H.265", p.bitrateKbps / 1000, r.videoMbps));
            const auto c = i.controller.capabilities();
            if (i.ps5Form.value("viewOnly").toBool()) message += tr("\n仅观看：电脑输入已关闭，手柄由 PS5 处理。");
            else if (!c.connected) message += tr("\n未检测到电脑手柄。");
            else message += oviora::ui::i18n::text(std::format(L"\n陀螺仪 {} · 触摸板 {} · 扳机 {} · 触觉 {}{}", c.gyro && c.accel ? L"已启用" : L"不可用",
                c.touch ? L"可用" : L"不可用", c.triggers ? L"已接入" : L"不可用", c.haptics ? L"端点已打开" : L"未打开", c.calibrating ? L" · 校准中" : L""));
        }
        if (message != i.ps5Status) { i.ps5Status = message; changed = true; }
        // Not while the connect is still opening: the engine reports the source only a moment later
        if (!active && !s.remotePlay && !i.openingSource) i.ps5Watching = false;
    }
#endif
    if (changed) emit ps5Changed();
    updateConnection();
}

// Oviora: what the old Oviora's connecting screen showed. stage: "" (nothing), "opening",
// "psn-reach", "psn-punch", "psn-data", "retrying", "pin", "live", "failed". A failure stays until
// the card is closed (clearConnectionError) or a new connection starts.
void QmlPlayerBridge::updateConnection() {
    auto& i = *impl_;
    const auto& s = i.snapshot;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool opening = !i.pendingLiveText.isEmpty() || (s.remotePlay && s.transport == engine::TransportState::Opening);
    QString stage, detail;
#ifdef OVIORA_ENABLE_REMOTEPLAY
    if (opening && !i.connectionSeen) { i.connectionSeen = true; i.connectionStartMs = now; i.connectionError.clear(); }
    if (s.remotePlay && s.failed && i.connectionSeen) {
        i.connectionError = oviora::ui::i18n::text(s.status);
        i.connectionSeen = false;
    }
    if (!i.connectionError.isEmpty()) {
        stage = QStringLiteral("failed"); detail = i.connectionError;
    } else if (!i.wakeWaitKey.isEmpty()) {
        stage = QStringLiteral("waking");
        if (!i.connectionSeen) { i.connectionSeen = true; i.connectionStartMs = now; }
    } else if (s.remotePlay && s.transport == engine::TransportState::Stopping && i.connectionSeen && !s.running) {
        // Cancelled while connecting: the console can take many seconds to let the session go
        // (field report: 16 s of a black window). The connecting screen says it is closing.
        stage = QStringLiteral("stopping");
    } else if (s.remotePlay && s.remoteRecovering) {
        stage = QStringLiteral("retrying"); detail = oviora::ui::i18n::text(s.remoteRecoveryMessage);
    } else if (s.remotePlay && s.remotePlayState == int(remoteplay::SessionState::LoginPinRequired)) {
        stage = QStringLiteral("pin");
    } else if (s.remotePlay && s.running) {
        stage = QStringLiteral("live");
    } else if (opening) {
        switch (remoteplay::connectStage()) {
        case remoteplay::ConnectStage::PsnReach: stage = QStringLiteral("psn-reach"); break;
        case remoteplay::ConnectStage::PsnPunch: stage = QStringLiteral("psn-punch"); break;
        case remoteplay::ConnectStage::PsnData: stage = QStringLiteral("psn-data"); break;
        default: stage = QStringLiteral("opening"); break;
        }
    } else {
        i.connectionSeen = false;
    }
#endif
    QVariantMap m;
    m["stage"] = stage;
    m["detail"] = detail;
    m["psn"] = i.connectionPsn;
    m["console"] = i.connectionName;
    // The stream's codec as asked of the PS5
    m["codec"] = s.remoteStream.requestedProfile.codec == remoteplay::Codec::H264 ? QStringLiteral("h264") : QStringLiteral("h265");
    // The old screen's "this can take a few minutes" note: after 25 s of connecting
    m["slow"] = !stage.isEmpty() && stage != QLatin1String("live") && stage != QLatin1String("failed") && now - i.connectionStartMs > 25000;
#ifdef OVIORA_ENABLE_REMOTEPLAY
    m["cantDisplay"] = stage == QLatin1String("live") && remoteplay::consoleCantDisplay();
    // The console's on-screen keyboard: the PC types into it (keyboardSetText / Accept / Reject)
    const auto kb = remoteplay::consoleKeyboard();
    m["keyboard"] = stage == QLatin1String("live") && kb.open;
    m["keyboardText"] = QString::fromStdString(kb.text);
#endif
    if (m != i.connection) { i.connection = m; emit connectionChanged(); }
}

QVariantMap QmlPlayerBridge::connection() const { return impl_->connection; }
bool QmlPlayerBridge::keyboardSetText(const QString& text) {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    return remoteplay::consoleKeyboardSetText(text.toStdString());
#else
    Q_UNUSED(text); return false;
#endif
}
bool QmlPlayerBridge::keyboardAccept() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    const bool ok = remoteplay::consoleKeyboardAccept(); updateConnection(); return ok;
#else
    return false;
#endif
}
bool QmlPlayerBridge::keyboardReject() {
#ifdef OVIORA_ENABLE_REMOTEPLAY
    const bool ok = remoteplay::consoleKeyboardReject(); updateConnection(); return ok;
#else
    return false;
#endif
}
void QmlPlayerBridge::clearConnectionError() {
    impl_->connectionError.clear();
    updateConnection();
}

// --- ps5 dialog --------------------------------------------------------------
QString QmlPlayerBridge::remotePlayHost() const { return impl_->remotePlayHost; }
void QmlPlayerBridge::setRemotePlayHost(const QString& value) {
    impl_->remotePlayHost = value;
    emit settingsChanged();
}
QString QmlPlayerBridge::remotePlayPin() const { return impl_->remotePlayPin; }
void QmlPlayerBridge::setRemotePlayPin(const QString& value) {
    // Held only until the connect command runs; it is never written to the session
    // file, and the engine keeps PSN credentials encrypted in the user data
    // directory rather than here.
    impl_->remotePlayPin = value;
    emit settingsChanged();
}



// --- opening a live source -----------------------------------------------------
void QmlPlayerBridge::openAfterCinema(const QString& pending, std::function<void()> open) {
    const int gen = ++impl_->liveOpenGen;
    // Switch to the stream page first (from any page, also Ayarlar) and say that it is connecting;
    // open once the page has been laid out, so the video window gets a real rectangle. Oviora: the
    // window no longer springs to the picture's height, so the old 820 ms wait for it is gone.
    const bool onStream = impl_->currentPage == QLatin1String("min");
    if (!onStream) emit navigate(QStringLiteral("min"));
    impl_->pendingLiveText = pending;
    emit snapshotChanged();
    const int delay = onStream || reducedMotion() ? 0 : 120;
    oviora::log::info("ui-live-open", std::format("deferred ms={} gen={}", delay, gen));
    QTimer::singleShot(delay, this, [this, gen, open = std::move(open)] {
        if (gen != impl_->liveOpenGen) { oviora::log::info("ui-live-open", std::format("superseded gen={}", gen)); return; }
        impl_->pendingLiveText.clear();
        oviora::log::info("ui-live-open", std::format("open gen={}", gen));
        open();
        emit snapshotChanged();
    });
}

} // namespace oviora::ui
