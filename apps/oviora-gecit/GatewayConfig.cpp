// SPDX-License-Identifier: GPL-3.0-only
#include "GatewayConfig.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>

namespace oviora::gecit {
namespace {

void secureZeroString(std::string& s) noexcept {
    if (!s.empty()) {
        SecureZeroMemory(s.data(), s.size());
        s.clear();
    }
}

std::string_view trimView(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n'))
        sv.remove_suffix(1);
    return sv;
}

bool parseU16(std::string_view sv, std::uint16_t& val) noexcept {
    sv = trimView(sv);
    if (sv.empty()) return false;
    unsigned int tmp = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), tmp);
    if (ec != std::errc{} || ptr != sv.data() + sv.size() || tmp > 65535u) return false;
    val = static_cast<std::uint16_t>(tmp);
    return true;
}

bool parseSize(std::string_view sv, std::size_t& val) noexcept {
    sv = trimView(sv);
    if (sv.empty()) return false;
    std::size_t tmp = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), tmp);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) return false;
    val = tmp;
    return true;
}

std::filesystem::path resolvePathRelativeTo(const std::filesystem::path& baseDir, std::string_view utf8Path) {
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(), int(utf8Path.size()), nullptr, 0);
    std::wstring wide(std::size_t(std::max(0, wideLen)), L'\0');
    if (wideLen > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8Path.data(), int(utf8Path.size()), wide.data(), wideLen);
    }
    std::filesystem::path p(wide);
    if (p.is_relative()) return (baseDir / p).lexically_normal();
    return p.lexically_normal();
}

} // namespace

GatewayConfig::~GatewayConfig() { wipeKey(); }

GatewayConfig::GatewayConfig(GatewayConfig&& other) noexcept
    : apiUrl(std::move(other.apiUrl)),
      keyFilePath(std::move(other.keyFilePath)),
      udpBegin(other.udpBegin),
      udpEnd(other.udpEnd),
      stunServers(std::move(other.stunServers)),
      maxSessions(other.maxSessions),
      maxViewerMbps(other.maxViewerMbps),
      gatewayKey(std::move(other.gatewayKey)) {
    other.wipeKey();
}

GatewayConfig& GatewayConfig::operator=(GatewayConfig&& other) noexcept {
    if (this == &other) return *this;
    wipeKey();
    apiUrl = std::move(other.apiUrl);
    keyFilePath = std::move(other.keyFilePath);
    udpBegin = other.udpBegin;
    udpEnd = other.udpEnd;
    stunServers = std::move(other.stunServers);
    maxSessions = other.maxSessions;
    maxViewerMbps = other.maxViewerMbps;
    gatewayKey = std::move(other.gatewayKey);
    other.wipeKey();
    return *this;
}

void GatewayConfig::wipeKey() noexcept {
    secureZeroString(gatewayKey);
}

bool parseGatewayIniText(std::string_view text,
                         const std::filesystem::path& exeDir,
                         GatewayConfig& out,
                         std::string* error) {
    out.keyFilePath = (exeDir / L"..\\gizli\\gecit.key").lexically_normal();

    // Strip UTF-8 BOM if present.
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    std::size_t pos = 0;
    std::size_t lineNo = 0;
    while (pos <= text.size()) {
        const auto next = text.find('\n', pos);
        const std::string_view rawLine = next == std::string_view::npos
            ? text.substr(pos)
            : text.substr(pos, next - pos);
        pos = next == std::string_view::npos ? text.size() + 1 : next + 1;
        ++lineNo;

        const std::string_view line = trimView(rawLine);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') continue;

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (error) *error = "gecit.ini gecersiz satir #" + std::to_string(lineNo);
            return false;
        }
        const std::string_view key = trimView(line.substr(0, eq));
        const std::string_view val = trimView(line.substr(eq + 1));

        if (key == "api") {
            if (val.empty()) {
                if (error) *error = "gecit.ini api bos olamaz";
                return false;
            }
            out.apiUrl = std::string(val);
        } else if (key == "anahtar_dosyasi") {
            if (val.empty()) {
                if (error) *error = "gecit.ini anahtar_dosyasi bos olamaz";
                return false;
            }
            out.keyFilePath = resolvePathRelativeTo(exeDir, val);
        } else if (key == "udp") {
            const auto dash = val.find('-');
            std::uint16_t b = 0, e = 0;
            if (dash == std::string_view::npos || !parseU16(val.substr(0, dash), b) ||
                !parseU16(val.substr(dash + 1), e) || b < 1024 || b > e) {
                if (error) *error = "gecit.ini udp araligi gecersiz (ornek: 50000-50100)";
                return false;
            }
            out.udpBegin = b;
            out.udpEnd = e;
        } else if (key == "stun") {
            out.stunServers.clear();
            std::size_t s = 0;
            while (s < val.size()) {
                const auto comma = val.find(',', s);
                const std::string_view item = trimView(
                    comma == std::string_view::npos ? val.substr(s) : val.substr(s, comma - s));
                if (!item.empty()) out.stunServers.emplace_back(item);
                if (comma == std::string_view::npos) break;
                s = comma + 1;
            }
        } else if (key == "en_cok_oturum") {
            std::size_t n = 0;
            if (!parseSize(val, n) || n == 0 || n > 64) {
                if (error) *error = "gecit.ini en_cok_oturum 1..64 olmali";
                return false;
            }
            out.maxSessions = n;
        } else if (key == "en_cok_izleyici_mbps") {
            std::size_t mbps = 0;
            if (!parseSize(val, mbps) || mbps == 0 || mbps > 100000) {
                if (error) *error = "gecit.ini en_cok_izleyici_mbps 1..100000 olmali";
                return false;
            }
            out.maxViewerMbps = mbps;
        }
    }
    return true;
}

bool loadGatewayConfig(const std::filesystem::path& iniPath,
                       const std::filesystem::path& exeDir,
                       GatewayConfig& out,
                       std::string* error,
                       bool requireKeyFile) {
    out.wipeKey();
    out.apiUrl = "http://127.0.0.1:8080/api.php";
    out.keyFilePath = (exeDir / L"..\\gizli\\gecit.key").lexically_normal();
    out.udpBegin = 50000;
    out.udpEnd = 50100;
    out.stunServers = {"stun:stun.l.google.com:19302"};
    out.maxSessions = 4;
    out.maxViewerMbps = 700;

    std::error_code ec;
    if (std::filesystem::exists(iniPath, ec)) {
        std::ifstream f(iniPath, std::ios::binary);
        if (!f) {
            if (error) *error = "gecit.ini acilamadi";
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!parseGatewayIniText(ss.str(), exeDir, out, error)) {
            return false;
        }
    }

    if (requireKeyFile) {
        std::ifstream kf(out.keyFilePath, std::ios::binary);
        if (!kf) {
            if (error) *error = "anahtar dosyasi okunamadi";
            return false;
        }
        std::ostringstream kss;
        kss << kf.rdbuf();
        std::string raw = kss.str();
        const std::string_view trimmed = trimView(raw);
        if (trimmed.empty()) {
            secureZeroString(raw);
            if (error) *error = "anahtar dosyasi bos";
            return false;
        }
        out.gatewayKey.assign(trimmed.data(), trimmed.size());
        secureZeroString(raw);
    }
    return true;
}

} // namespace oviora::gecit
