// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oviora/remoteplay/Types.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oviora::gecit {

// Tracks 8-bit modular sequence numbers on the unreliable `kumanda` channel.
class KumandaSeqTracker {
public:
    bool accept(std::uint8_t seq) noexcept;
    void reset() noexcept {
        initialized_ = false;
        lastSeq_ = 0;
    }

private:
    bool initialized_ = false;
    std::uint8_t lastSeq_ = 0;
};

enum class KumandaParseResult {
    UpdatedState,
    UpdatedTouches,
    StaleSeq,
    Invalid
};

// Parses a binary `kumanda` DataChannel packet (contract section 4):
//  - type 1 (14 bytes, LE): u8 1 | u8 seq | u16 buttons | i16 lx | i16 ly | i16 rx | i16 ry | u8 l2 | u8 r2
//  - type 2 (2 + 5*n bytes, LE): u8 2 | u8 n | n * (u8 id | u16 x | u16 y), n <= 2
KumandaParseResult parseKumandaPacket(std::span<const std::uint8_t> data,
                                      KumandaSeqTracker& seqTracker,
                                      oviora::remoteplay::ControllerState& state) noexcept;

enum class KontrolIncomingType {
    Unknown,
    Idr,
    Bye
};

KontrolIncomingType parseKontrolMessage(std::string_view text);
std::string makeKontrolStateJson(std::string_view state);
std::string makeKontrolRumbleJson(std::uint8_t left, std::uint8_t right);
std::string makeKontrolErrorJson(std::string_view message);
std::string makeKontrolIdrJson();

struct GwStartJob {
    std::string wsid;
    std::string psnDuid;
    std::string psnAccountIdB64;
    std::string psnToken;
    std::string nickname;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t fps = 60;
    std::uint32_t bitrateKbps = 15000;
    std::string codec = "h264";

    GwStartJob() = default;
    ~GwStartJob();
    GwStartJob(const GwStartJob&) = delete;
    GwStartJob& operator=(const GwStartJob&) = delete;
    GwStartJob(GwStartJob&& other) noexcept;
    GwStartJob& operator=(GwStartJob&& other) noexcept;

    void wipeToken() noexcept;
};

enum class SignalTargetKind { Wsid, Yid, Iid };

struct GwSignalJob {
    SignalTargetKind targetKind = SignalTargetKind::Wsid;
    std::string targetId;
    nlohmann::json data;
};

struct GwStopJob {
    std::string wsid;
    std::string reason;
};

struct GwIzletStartJob {
    std::string yid;
    std::string codec = "h264";
};

struct GwIzleJoinJob {
    std::string yid;
    std::string iid;
};

struct GwIzleLeaveJob {
    std::string iid;
    std::string reason;
};

struct GwIzletStopJob {
    std::string yid;
    std::string reason;
};

struct GwJob {
    enum class Type { Start, Signal, Stop, IzletStart, IzleJoin, IzleLeave, IzletStop };
    Type type = Type::Stop;
    GwStartJob start;
    GwSignalJob signal;
    GwStopJob stop;
    GwIzletStartJob izletStart;
    GwIzleJoinJob izleJoin;
    GwIzleLeaveJob izleLeave;
    GwIzletStopJob izletStop;
};

struct GwPollResponse {
    std::int64_t last = 0;
    std::vector<GwJob> jobs;
};

bool isValidWsid(std::string_view wsid) noexcept;
std::string shortWsid(std::string_view wsid);

// Parses `{jobs:[...], last:...}` from `gw_poll`.
std::optional<GwPollResponse> parseGwPollResponse(std::string_view jsonText, std::string* error = nullptr);

} // namespace oviora::gecit
