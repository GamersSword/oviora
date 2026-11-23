// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace oviora::gecit {

struct GatewayConfig {
    std::string apiUrl = "http://127.0.0.1:8080/api.php";
    std::filesystem::path keyFilePath;
    std::uint16_t udpBegin = 50000;
    std::uint16_t udpEnd = 50100;
    std::vector<std::string> stunServers = {"stun:stun.l.google.com:19302"};
    std::size_t maxSessions = 4;
    std::size_t maxViewerMbps = 700;
    std::string gatewayKey;

    GatewayConfig() = default;
    ~GatewayConfig();
    GatewayConfig(const GatewayConfig&) = delete;
    GatewayConfig& operator=(const GatewayConfig&) = delete;
    GatewayConfig(GatewayConfig&&) noexcept;
    GatewayConfig& operator=(GatewayConfig&&) noexcept;

    void wipeKey() noexcept;
};

// Loads gecit.ini (creating defaults if missing when allowMissingIni is true)
// and reads the gateway shared key from anahtar_dosyasi.
bool loadGatewayConfig(const std::filesystem::path& iniPath,
                       const std::filesystem::path& exeDir,
                       GatewayConfig& out,
                       std::string* error = nullptr,
                       bool requireKeyFile = true);

// Pure parser for INI text (also unit-testable without disk I/O).
bool parseGatewayIniText(std::string_view text,
                         const std::filesystem::path& exeDir,
                         GatewayConfig& out,
                         std::string* error = nullptr);

} // namespace oviora::gecit
