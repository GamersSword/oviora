// SPDX-License-Identifier: GPL-3.0-only
#include "DataChannelProtocol.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace oviora::gecit {
namespace {

std::uint16_t readU16Le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

std::int16_t readI16Le(const std::uint8_t* p) noexcept {
    const std::uint16_t u = readU16Le(p);
    std::int16_t s = 0;
    std::memcpy(&s, &u, sizeof(s));
    return s;
}

void wipeString(std::string& s) noexcept {
    if (!s.empty()) {
        SecureZeroMemory(s.data(), s.size());
        s.clear();
    }
}

} // namespace

bool KumandaSeqTracker::accept(std::uint8_t seq) noexcept {
    if (!initialized_) {
        initialized_ = true;
        lastSeq_ = seq;
        return true;
    }
    const std::uint8_t delta = static_cast<std::uint8_t>(seq - lastSeq_);
    if (delta == 0 || delta >= 128u) {
        return false;
    }
    lastSeq_ = seq;
    return true;
}

KumandaParseResult parseKumandaPacket(std::span<const std::uint8_t> data,
                                      KumandaSeqTracker& seqTracker,
                                      oviora::remoteplay::ControllerState& state) noexcept {
    if (data.empty()) return KumandaParseResult::Invalid;
    const std::uint8_t type = data[0];
    if (type == 1) {
        if (data.size() != 14) return KumandaParseResult::Invalid;
        const std::uint8_t seq = data[1];
        if (!seqTracker.accept(seq)) return KumandaParseResult::StaleSeq;

        state.inputActive = true;
        state.buttons = readU16Le(data.data() + 2);
        state.leftX = readI16Le(data.data() + 4);
        state.leftY = readI16Le(data.data() + 6);
        state.rightX = readI16Le(data.data() + 8);
        state.rightY = readI16Le(data.data() + 10);
        state.l2 = data[12];
        state.r2 = data[13];
        return KumandaParseResult::UpdatedState;
    }
    if (type == 2) {
        if (data.size() < 2) return KumandaParseResult::Invalid;
        const std::uint8_t count = data[1];
        if (count > 2 || data.size() != 2 + static_cast<std::size_t>(count) * 5) {
            return KumandaParseResult::Invalid;
        }
        state.inputActive = true;
        state.touches = {};
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t off = 2 + i * 5;
            const std::uint8_t rawId = data[off];
            const std::uint16_t rawX = readU16Le(data.data() + off + 1);
            const std::uint16_t rawY = readU16Le(data.data() + off + 3);
            state.touches[i].id = static_cast<std::int8_t>(rawId & 0x7Fu);
            state.touches[i].x = std::min<std::uint16_t>(rawX, 1919u);
            state.touches[i].y = std::min<std::uint16_t>(rawY, 941u);
        }
        return KumandaParseResult::UpdatedTouches;
    }
    return KumandaParseResult::Invalid;
}

KontrolIncomingType parseKontrolMessage(std::string_view text) {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return KontrolIncomingType::Unknown;
    const auto it = j.find("t");
    if (it == j.end() || !it->is_string()) return KontrolIncomingType::Unknown;
    const std::string& t = it->get_ref<const std::string&>();
    if (t == "idr") return KontrolIncomingType::Idr;
    if (t == "bye") return KontrolIncomingType::Bye;
    return KontrolIncomingType::Unknown;
}

std::string makeKontrolStateJson(std::string_view state) {
    return nlohmann::json{{"t", "state"}, {"s", state}}.dump();
}

std::string makeKontrolRumbleJson(std::uint8_t left, std::uint8_t right) {
    return nlohmann::json{{"t", "rumble"}, {"l", left}, {"r", right}}.dump();
}

std::string makeKontrolErrorJson(std::string_view message) {
    return nlohmann::json{{"t", "error"}, {"m", message}}.dump();
}

std::string makeKontrolIdrJson() {
    return nlohmann::json{{"t", "idr"}}.dump();
}

GwStartJob::~GwStartJob() { wipeToken(); }

GwStartJob::GwStartJob(GwStartJob&& other) noexcept
    : wsid(std::move(other.wsid)),
      psnDuid(std::move(other.psnDuid)),
      psnAccountIdB64(std::move(other.psnAccountIdB64)),
      psnToken(std::move(other.psnToken)),
      nickname(std::move(other.nickname)),
      width(other.width),
      height(other.height),
      fps(other.fps),
      bitrateKbps(other.bitrateKbps),
      codec(std::move(other.codec)) {
    other.wipeToken();
}

GwStartJob& GwStartJob::operator=(GwStartJob&& other) noexcept {
    if (this == &other) return *this;
    wipeToken();
    wsid = std::move(other.wsid);
    psnDuid = std::move(other.psnDuid);
    psnAccountIdB64 = std::move(other.psnAccountIdB64);
    psnToken = std::move(other.psnToken);
    nickname = std::move(other.nickname);
    width = other.width;
    height = other.height;
    fps = other.fps;
    bitrateKbps = other.bitrateKbps;
    codec = std::move(other.codec);
    other.wipeToken();
    return *this;
}

void GwStartJob::wipeToken() noexcept {
    wipeString(psnToken);
}

bool isValidWsid(std::string_view wsid) noexcept {
    if (wsid.empty() || wsid.size() > 64) return false;
    return std::all_of(wsid.begin(), wsid.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-' || c == '_';
    });
}

std::string shortWsid(std::string_view wsid) {
    return std::string(wsid.substr(0, std::min<std::size_t>(8, wsid.size())));
}

std::optional<GwPollResponse> parseGwPollResponse(std::string_view jsonText, std::string* error) {
    const auto root = nlohmann::json::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "gw_poll yaniti JSON nesnesi degil";
        return std::nullopt;
    }

    GwPollResponse out;
    if (const auto it = root.find("last"); it != root.end() && it->is_number_integer()) {
        out.last = it->get<std::int64_t>();
    }
    const auto jobsIt = root.find("jobs");
    if (jobsIt == root.end()) return out;
    if (!jobsIt->is_array()) {
        if (error) *error = "gw_poll jobs dizi degil";
        return std::nullopt;
    }

    for (const auto& item : *jobsIt) {
        if (!item.is_object()) continue;
        const std::string type = item.value("type", "");

        if (type == "start") {
            const std::string wsid = item.value("wsid", "");
            if (!isValidWsid(wsid)) continue;
            const auto psnIt = item.find("psn");
            if (psnIt == item.end() || !psnIt->is_object()) continue;

            GwJob job;
            job.type = GwJob::Type::Start;
            job.start.wsid = wsid;
            job.start.psnDuid = psnIt->value("duid", "");
            job.start.psnAccountIdB64 = psnIt->value("account_id", "");
            job.start.psnToken = psnIt->value("token", "");
            job.start.nickname = psnIt->value("nickname", "");

            if (const auto vidIt = item.find("video"); vidIt != item.end() && vidIt->is_object()) {
                job.start.width = vidIt->value("width", 1920u);
                job.start.height = vidIt->value("height", 1080u);
                job.start.fps = vidIt->value("fps", 60u);
                job.start.bitrateKbps = vidIt->value("kbps", 15000u);
                job.start.codec = vidIt->value("codec", std::string("h264"));
            }
            out.jobs.push_back(std::move(job));
        } else if (type == "signal") {
            const auto dataIt = item.find("data");
            if (dataIt == item.end() || !dataIt->is_object()) continue;

            SignalTargetKind kind = SignalTargetKind::Wsid;
            std::string targetId = item.value("wsid", "");
            if (!isValidWsid(targetId)) {
                targetId = item.value("yid", "");
                if (isValidWsid(targetId)) {
                    kind = SignalTargetKind::Yid;
                } else {
                    targetId = item.value("iid", "");
                    if (!isValidWsid(targetId)) continue;
                    kind = SignalTargetKind::Iid;
                }
            }

            GwJob job;
            job.type = GwJob::Type::Signal;
            job.signal.targetKind = kind;
            job.signal.targetId = std::move(targetId);
            job.signal.data = *dataIt;
            out.jobs.push_back(std::move(job));
        } else if (type == "stop") {
            const std::string wsid = item.value("wsid", "");
            if (!isValidWsid(wsid)) continue;
            GwJob job;
            job.type = GwJob::Type::Stop;
            job.stop.wsid = wsid;
            job.stop.reason = item.value("reason", "");
            out.jobs.push_back(std::move(job));
        } else if (type == "izlet_start") {
            const std::string yid = item.value("yid", "");
            if (!isValidWsid(yid)) continue;
            GwJob job;
            job.type = GwJob::Type::IzletStart;
            job.izletStart.yid = yid;
            if (const auto vidIt = item.find("video"); vidIt != item.end() && vidIt->is_object()) {
                job.izletStart.codec = vidIt->value("codec", std::string("h264"));
            }
            out.jobs.push_back(std::move(job));
        } else if (type == "izle_join") {
            const std::string yid = item.value("yid", "");
            const std::string iid = item.value("iid", "");
            if (!isValidWsid(yid) || !isValidWsid(iid)) continue;
            GwJob job;
            job.type = GwJob::Type::IzleJoin;
            job.izleJoin.yid = yid;
            job.izleJoin.iid = iid;
            out.jobs.push_back(std::move(job));
        } else if (type == "izle_leave") {
            const std::string iid = item.value("iid", "");
            if (!isValidWsid(iid)) continue;
            GwJob job;
            job.type = GwJob::Type::IzleLeave;
            job.izleLeave.iid = iid;
            job.izleLeave.reason = item.value("reason", "");
            out.jobs.push_back(std::move(job));
        } else if (type == "izlet_stop") {
            const std::string yid = item.value("yid", "");
            if (!isValidWsid(yid)) continue;
            GwJob job;
            job.type = GwJob::Type::IzletStop;
            job.izletStop.yid = yid;
            job.izletStop.reason = item.value("reason", "");
            out.jobs.push_back(std::move(job));
        }
    }
    return out;
}

} // namespace oviora::gecit
