// SPDX-License-Identifier: GPL-3.0-only
#include "DataChannelProtocol.h"
#include "GatewayConfig.h"
#include "H264RelayCache.h"
#include "IzletBroadcastSession.h"
#include "IzletMediaCache.h"
#include "WebRtcGatewaySession.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#define CHECK(expr)                                                               \
    do {                                                                          \
        if (!(expr)) {                                                            \
            std::cerr << "CHECK FAILED at line " << __LINE__ << ": " #expr "\n"; \
            std::exit(1);                                                         \
        }                                                                         \
    } while (false)

using namespace oviora::gecit;
using oviora::remoteplay::ControllerState;

namespace {

std::vector<std::uint8_t> makeType1Packet(std::uint8_t seq,
                                          std::uint16_t buttons,
                                          std::int16_t lx,
                                          std::int16_t ly,
                                          std::int16_t rx,
                                          std::int16_t ry,
                                          std::uint8_t l2,
                                          std::uint8_t r2) {
    std::vector<std::uint8_t> buf(14, 0);
    buf[0] = 1;
    buf[1] = seq;
    buf[2] = static_cast<std::uint8_t>(buttons & 0xFFu);
    buf[3] = static_cast<std::uint8_t>((buttons >> 8) & 0xFFu);
    auto writeI16 = [&](std::size_t off, std::int16_t v) {
        std::uint16_t u = 0;
        std::memcpy(&u, &v, 2);
        buf[off] = static_cast<std::uint8_t>(u & 0xFFu);
        buf[off + 1] = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
    };
    writeI16(4, lx);
    writeI16(6, ly);
    writeI16(8, rx);
    writeI16(10, ry);
    buf[12] = l2;
    buf[13] = r2;
    return buf;
}

void testKumandaBinaryParsing() {
    KumandaSeqTracker tracker;
    ControllerState state;

    // 1. Valid type 1 packet with negative sticks and PS + Cross buttons
    const auto p1 = makeType1Packet(254, ControllerState::Cross | ControllerState::PS,
                                    -32768, 32767, -1234, 5678, 200, 99);
    CHECK(parseKumandaPacket(p1, tracker, state) == KumandaParseResult::UpdatedState);
    CHECK(state.inputActive);
    CHECK(state.buttons == (ControllerState::Cross | ControllerState::PS));
    CHECK(state.leftX == -32768 && state.leftY == 32767);
    CHECK(state.rightX == -1234 && state.rightY == 5678);
    CHECK(state.l2 == 200 && state.r2 == 99);

    // 2. Sequence 8-bit wraparound: 254 -> 255 -> 0 -> 1 accepted
    const auto p2 = makeType1Packet(255, ControllerState::Circle, 0, 0, 0, 0, 0, 0);
    CHECK(parseKumandaPacket(p2, tracker, state) == KumandaParseResult::UpdatedState);
    CHECK(state.buttons == ControllerState::Circle);

    const auto p3 = makeType1Packet(0, ControllerState::Triangle, 10, 20, 30, 40, 1, 2);
    CHECK(parseKumandaPacket(p3, tracker, state) == KumandaParseResult::UpdatedState);
    CHECK(state.buttons == ControllerState::Triangle);

    const auto p4 = makeType1Packet(1, ControllerState::Square, 100, -100, 0, 0, 255, 0);
    CHECK(parseKumandaPacket(p4, tracker, state) == KumandaParseResult::UpdatedState);
    CHECK(state.buttons == ControllerState::Square);

    // 3. Stale / duplicate sequences rejected and state untouched
    const auto dup = makeType1Packet(1, 0, 0, 0, 0, 0, 0, 0);
    CHECK(parseKumandaPacket(dup, tracker, state) == KumandaParseResult::StaleSeq);
    CHECK(state.buttons == ControllerState::Square);

    const auto oldWrap = makeType1Packet(255, 0, 0, 0, 0, 0, 0, 0);
    CHECK(parseKumandaPacket(oldWrap, tracker, state) == KumandaParseResult::StaleSeq);
    CHECK(state.buttons == ControllerState::Square);

    const auto tooFar = makeType1Packet(static_cast<std::uint8_t>(1 + 128), 0, 0, 0, 0, 0, 0, 0);
    CHECK(parseKumandaPacket(tooFar, tracker, state) == KumandaParseResult::StaleSeq);

    // 4. Malformed type 1 lengths
    const std::vector<std::uint8_t> shortT1(13, 1);
    const std::vector<std::uint8_t> longT1(15, 1);
    const std::vector<std::uint8_t> badType = {9, 0, 0};
    CHECK(parseKumandaPacket({}, tracker, state) == KumandaParseResult::Invalid);
    CHECK(parseKumandaPacket(shortT1, tracker, state) == KumandaParseResult::Invalid);
    CHECK(parseKumandaPacket(longT1, tracker, state) == KumandaParseResult::Invalid);
    CHECK(parseKumandaPacket(badType, tracker, state) == KumandaParseResult::Invalid);

    // 5. Type 2 touchpad packets: n=2, n=1, n=0, and malformed
    std::vector<std::uint8_t> t2Two = {
        2, 2,
        3, 0x00, 0x04, 0x20, 0x01, // id=3, x=1024, y=288
        7, 0xFF, 0x0F, 0xFF, 0x07  // id=7, x=4095 (clamped to 1919), y=2047 (clamped to 941)
    };
    CHECK(parseKumandaPacket(t2Two, tracker, state) == KumandaParseResult::UpdatedTouches);
    CHECK(state.buttons == ControllerState::Square); // gamepad state preserved
    CHECK(state.touches[0].id == 3 && state.touches[0].x == 1024 && state.touches[0].y == 288);
    CHECK(state.touches[1].id == 7 && state.touches[1].x == 1919 && state.touches[1].y == 941);

    std::vector<std::uint8_t> t2Release = {2, 0};
    CHECK(parseKumandaPacket(t2Release, tracker, state) == KumandaParseResult::UpdatedTouches);
    CHECK(state.touches[0].id == -1 && state.touches[1].id == -1);

    std::vector<std::uint8_t> t2BadCount = {2, 3, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0};
    std::vector<std::uint8_t> t2Trunc = {2, 1, 0, 1, 2};
    CHECK(parseKumandaPacket(t2BadCount, tracker, state) == KumandaParseResult::Invalid);
    CHECK(parseKumandaPacket(t2Trunc, tracker, state) == KumandaParseResult::Invalid);
}

void testH264RelayCache() {
    H264RelayCache cache;
    const std::vector<std::uint8_t> spsPps = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xE0, 0x1F,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80
    };
    const std::vector<std::uint8_t> pSlice = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x24};
    const std::vector<std::uint8_t> idrSlice = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21};

    // P-frame before config and IDR must be dropped
    CHECK(!cache.processAccessUnit(pSlice, false, false).has_value());
    CHECK(cache.waitingForIdr());

    // IDR without cached SPS/PPS must wait
    CHECK(!cache.processAccessUnit(idrSlice, true, false).has_value());

    // Cache SPS/PPS
    cache.setConfig(spsPps);
    CHECK(cache.hasConfig());

    // IDR gets SPS/PPS prepended
    const auto firstOut = cache.processAccessUnit(idrSlice, true, false);
    CHECK(firstOut.has_value());
    CHECK(!cache.waitingForIdr());
    CHECK(firstOut->size() == spsPps.size() + idrSlice.size());
    CHECK(std::equal(spsPps.begin(), spsPps.end(), firstOut->begin()));
    CHECK(std::equal(idrSlice.begin(), idrSlice.end(), firstOut->begin() + std::ptrdiff_t(spsPps.size())));

    // Following P-frame goes out as-is (without SPS/PPS)
    const auto pOut = cache.processAccessUnit(pSlice, false, false);
    CHECK(pOut.has_value());
    CHECK(*pOut == pSlice);

    // Loss triggers waitingForIdr again and drops P-frame until next IDR
    CHECK(!cache.processAccessUnit(pSlice, false, true).has_value());
    CHECK(cache.waitingForIdr());
    CHECK(!cache.processAccessUnit(pSlice, false, false).has_value());

    const auto nextIdrOut = cache.processAccessUnit(idrSlice, true, false);
    CHECK(nextIdrOut.has_value());
    CHECK(nextIdrOut->size() == spsPps.size() + idrSlice.size());

    // Annex-B file splitter test
    std::vector<std::uint8_t> stream;
    stream.insert(stream.end(), spsPps.begin(), spsPps.end());
    stream.insert(stream.end(), idrSlice.begin(), idrSlice.end());
    stream.insert(stream.end(), pSlice.begin(), pSlice.end());
    const auto split = splitAnnexBFrames(stream);
    CHECK(split.size() == 2);
    CHECK(split[0].isIdr && split[0].configBytes == spsPps && split[0].frameBytes == idrSlice);
    CHECK(!split[1].isIdr && split[1].configBytes.empty() && split[1].frameBytes == pSlice);
}

void testContractJsonAndIni() {
    const char* pollJson = R"json({
        "last": 42,
        "jobs": [
            {
                "type": "start",
                "wsid": "0123456789abcdef0123456789abcdef",
                "psn": {
                    "duid": "0011223344556677",
                    "account_id": "AQAAAAAAAAA=",
                    "token": "secret-rp-token",
                    "nickname": "PS5-Oda"
                },
                "video": {
                    "width": 1920,
                    "height": 1080,
                    "fps": 60,
                    "kbps": 15000,
                    "codec": "h264"
                }
            },
            {
                "type": "signal",
                "wsid": "0123456789abcdef0123456789abcdef",
                "data": {"type": "offer", "sdp": "v=0..."}
            },
            {
                "type": "stop",
                "wsid": "0123456789abcdef0123456789abcdef",
                "reason": "user"
            },
            {
                "type": "start",
                "wsid": "../bad-wsid",
                "psn": {"duid": "x", "account_id": "y", "token": "z"}
            }
        ]
    })json";

    std::string err;
    auto parsed = parseGwPollResponse(pollJson, &err);
    CHECK(parsed.has_value());
    CHECK(parsed->last == 42);
    CHECK(parsed->jobs.size() == 3); // invalid wsid rejected
    CHECK(parsed->jobs[0].type == GwJob::Type::Start);
    CHECK(parsed->jobs[0].start.psnToken == "secret-rp-token");
    CHECK(parsed->jobs[0].start.width == 1920 && parsed->jobs[0].start.fps == 60);
    CHECK(parsed->jobs[1].type == GwJob::Type::Signal);
    CHECK(parsed->jobs[1].signal.targetKind == SignalTargetKind::Wsid);
    CHECK(parsed->jobs[1].signal.targetId == "0123456789abcdef0123456789abcdef");
    CHECK(parsed->jobs[1].signal.data.value("type", "") == "offer");
    CHECK(parsed->jobs[2].type == GwJob::Type::Stop);
    CHECK(parsed->jobs[2].stop.reason == "user");

    CHECK(parseKontrolMessage(R"({"t":"idr"})") == KontrolIncomingType::Idr);
    CHECK(parseKontrolMessage(R"({"t":"bye"})") == KontrolIncomingType::Bye);
    CHECK(parseKontrolMessage(R"({"t":"other"})") == KontrolIncomingType::Unknown);
    CHECK(parseKontrolMessage("not json") == KontrolIncomingType::Unknown);
    CHECK(makeKontrolRumbleJson(12, 240) == R"({"l":12,"r":240,"t":"rumble"})");

    GatewayConfig cfg;
    const std::string ini =
        "api=http://127.0.0.1:9000/api.php\n"
        "anahtar_dosyasi=..\\gizli\\test.key\n"
        "udp=51000-51050\n"
        "stun=stun:stun.example.org:3478\n"
        "en_cok_oturum=8\n";
    CHECK(parseGatewayIniText(ini, std::filesystem::path(L"C:\\srv\\gecit"), cfg, &err));
    CHECK(cfg.apiUrl == "http://127.0.0.1:9000/api.php");
    CHECK(cfg.udpBegin == 51000 && cfg.udpEnd == 51050);
    CHECK(cfg.maxSessions == 8);
    CHECK(cfg.stunServers.size() == 1 && cfg.stunServers[0] == "stun:stun.example.org:3478");

    GatewayConfig badCfg;
    CHECK(!parseGatewayIniText("udp=80-90\n", std::filesystem::path(L"C:\\srv"), badCfg, &err));

    // SDP H.264 / Opus payload type negotiation
    rtc::Description::Video v("0", rtc::Description::Direction::RecvOnly);
    v.addVP8Codec(96);
    v.addH264Codec(102, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f");
    v.addH264Codec(104, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
    v.addH264Codec(106, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c1f");
    const int chosenVideoPt = WebRtcGatewaySession::selectH264PayloadType(v);
    CHECK(chosenVideoPt == 104 || chosenVideoPt == 106);

    rtc::Description::Audio a("1", rtc::Description::Direction::RecvOnly);
    a.addPCMUCodec(0);
    a.addOpusCodec(111);
    CHECK(WebRtcGatewaySession::selectOpusPayloadType(a) == 111);

    rtc::Description::Video v265("0", rtc::Description::Direction::RecvOnly);
    v265.addH264Codec(96);
    v265.addH265Codec(98);
    CHECK(WebRtcGatewaySession::selectH265PayloadType(v265) == 98);
}

void testIzletH265NalAndGopBuffer() {
    // H.265 NAL headers: 2 bytes, type = (b[0] >> 1) & 0x3F, nuh_temporal_id_plus1 = b[1] & 7 != 0
    auto makeH265Nal = [](unsigned type, std::initializer_list<std::uint8_t> body) {
        std::vector<std::uint8_t> out = {
            0x00, 0x00, 0x00, 0x01,
            static_cast<std::uint8_t>((type & 0x3Fu) << 1),
            0x01
        };
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };

    const auto vps = makeH265Nal(32, {0x0C, 0x01});
    const auto sps = makeH265Nal(33, {0x01, 0x02, 0x03});
    const auto pps = makeH265Nal(34, {0xC1, 0x72});
    const auto idrW = makeH265Nal(19, {0xAA, 0xBB});
    const auto idrN = makeH265Nal(20, {0xCC, 0xDD});
    const auto cra = makeH265Nal(21, {0xEE, 0xFF});
    const auto trailR = makeH265Nal(1, {0x11, 0x22});

    // NAL detection unit tests
    {
        std::vector<std::uint8_t> cfg;
        cfg.insert(cfg.end(), vps.begin(), vps.end());
        cfg.insert(cfg.end(), sps.begin(), sps.end());
        cfg.insert(cfg.end(), pps.begin(), pps.end());
        const auto sCfg = inspectIzletAnnexB(cfg, IzletCodec::H265);
        CHECK(sCfg.valid && sCfg.hasConfig && sCfg.hasVps && sCfg.hasSps && sCfg.hasPps);
        CHECK(!sCfg.hasPicture && !sCfg.isIdr);
    }
    for (const auto& irap : {idrW, idrN, cra}) {
        const auto sIrap = inspectIzletAnnexB(irap, IzletCodec::H265);
        CHECK(sIrap.valid && sIrap.hasPicture && sIrap.isIdr && !sIrap.hasConfig);
    }
    {
        const auto sTrail = inspectIzletAnnexB(trailR, IzletCodec::H265);
        CHECK(sTrail.valid && sTrail.hasPicture && !sTrail.isIdr && !sTrail.hasConfig);
    }
    {
        // Invalid temporal_id_plus1 == 0
        const std::vector<std::uint8_t> badH265 = {0x00, 0x00, 0x00, 0x01, 0x26, 0x00, 0x11};
        CHECK(!inspectIzletAnnexB(badH265, IzletCodec::H265).valid);
    }

    // GOP buffer ordering, config prepending, 3 s / 4 MB bounds
    IzletGopBuffer gop(IzletCodec::H265, 3.0, 4u * 1024u * 1024u);
    CHECK(!gop.hasConfig());
    CHECK(!gop.hasValidGop());

    // Push VPS, SPS, PPS separately
    CHECK(!gop.pushAccessUnit(vps, 0.0).has_value());
    CHECK(!gop.hasConfig());
    CHECK(!gop.pushAccessUnit(sps, 0.0).has_value());
    CHECK(!gop.pushAccessUnit(pps, 0.0).has_value());
    CHECK(gop.hasConfig());

    // Push IDR_W_RADL -> prepends VPS+SPS+PPS and starts GOP
    const auto f0 = gop.pushAccessUnit(idrW, 1.00);
    CHECK(f0.has_value() && f0->isIdr);
    CHECK(f0->bytes->size() == vps.size() + sps.size() + pps.size() + idrW.size());
    CHECK(gop.hasValidGop());

    // Push two P-frames in order
    const auto f1 = gop.pushAccessUnit(trailR, 1.016);
    const auto f2 = gop.pushAccessUnit(trailR, 1.033);
    CHECK(f1.has_value() && !f1->isIdr);
    CHECK(f2.has_value() && !f2->isIdr);

    const auto snap = gop.snapshotGop();
    CHECK(snap.valid);
    CHECK(snap.frames.size() == 3);
    CHECK(snap.frames[0].isIdr && snap.frames[0].seq < snap.frames[1].seq && snap.frames[1].seq < snap.frames[2].seq);
    CHECK(snap.frames[0].mediaTimeSec < snap.frames[1].mediaTimeSec);
    CHECK(snap.frames[1].mediaTimeSec < snap.frames[2].mediaTimeSec);

    // Exceeding 3 s invalidates GOP buffer until next keyframe (CRA)
    const auto fLate = gop.pushAccessUnit(trailR, 4.10);
    CHECK(fLate.has_value()); // live frame still forwarded
    CHECK(!gop.hasValidGop());
    CHECK(!gop.snapshotGop().valid);

    // CRA starts a fresh valid GOP
    const auto fCra = gop.pushAccessUnit(cra, 4.20);
    CHECK(fCra.has_value() && fCra->isIdr);
    CHECK(gop.hasValidGop());
    CHECK(gop.snapshotGop().frames.size() == 1);

    // Exceeding byte limit invalidates GOP buffer
    IzletGopBuffer smallCap(IzletCodec::H265, 3.0, 2048);
    CHECK(!smallCap.pushAccessUnit(vps, 0.0).has_value());
    CHECK(!smallCap.pushAccessUnit(sps, 0.0).has_value());
    CHECK(!smallCap.pushAccessUnit(pps, 0.0).has_value());
    CHECK(smallCap.pushAccessUnit(idrN, 0.1).has_value());
    CHECK(smallCap.hasValidGop());
    std::vector<std::uint8_t> bigSlice = makeH265Nal(1, {0x01});
    bigSlice.resize(2500, 0x55);
    CHECK(smallCap.pushAccessUnit(bigSlice, 0.2).has_value());
    CHECK(!smallCap.hasValidGop());
}

void testIzletLimitsAndProtocol() {
    // 20-viewer limit on IzletBroadcastSession
    GatewayConfig cfg;
    cfg.stunServers.clear();
    auto broadcast = std::make_shared<IzletBroadcastSession>(
        "0123456789abcdef0123456789abcdef", IzletCodec::H264, cfg, IzletBroadcastCallbacks{});

    std::string err;
    for (int i = 0; i < 20; ++i) {
        char iid[33]{};
        std::snprintf(iid, sizeof(iid), "111111111111111111111111111111%02x", i);
        CHECK(broadcast->addViewer(iid, &err));
    }
    CHECK(broadcast->viewerCount() == 20);
    CHECK(!broadcast->addViewer("22222222222222222222222222222200", &err));
    CHECK(!err.empty());

    broadcast->removeViewer("11111111111111111111111111111100", "test", false);
    CHECK(broadcast->viewerCount() == 19);
    CHECK(broadcast->addViewer("22222222222222222222222222222200", &err));
    CHECK(broadcast->viewerCount() == 20);
    broadcast->closeAll(false);

    // en_cok_izleyici_mbps config & admission check
    CHECK(cfg.maxViewerMbps == 700);
    CHECK(parseGatewayIniText("en_cok_izleyici_mbps=350\n", std::filesystem::path(L"C:\\srv"), cfg, &err));
    CHECK(cfg.maxViewerMbps == 350);
    CHECK(!parseGatewayIniText("en_cok_izleyici_mbps=0\n", std::filesystem::path(L"C:\\srv"), cfg, &err));
    CHECK(canAcceptViewerBandwidth(680.0, 15.0, 700));
    CHECK(!canAcceptViewerBandwidth(690.0, 15.0, 700));
    CHECK(!canAcceptViewerBandwidth(700.0, 0.0, 700));

    // gw_poll izlet job parsing
    const char* izletPoll = R"json({
        "last": 100,
        "jobs": [
            {"type":"izlet_start","yid":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","video":{"codec":"h265"}},
            {"type":"izle_join","yid":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","iid":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
            {"type":"signal","yid":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","data":{"type":"offer","sdp":"v=0"}},
            {"type":"signal","iid":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","data":{"candidate":"cand","mid":"0"}},
            {"type":"izle_leave","iid":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","reason":"bye"},
            {"type":"izlet_stop","yid":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","reason":"owner"}
        ]
    })json";
    auto parsed = parseGwPollResponse(izletPoll, &err);
    CHECK(parsed.has_value());
    CHECK(parsed->jobs.size() == 6);
    CHECK(parsed->jobs[0].type == GwJob::Type::IzletStart && parsed->jobs[0].izletStart.codec == "h265");
    CHECK(parsed->jobs[1].type == GwJob::Type::IzleJoin && parsed->jobs[1].izleJoin.iid == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    CHECK(parsed->jobs[2].type == GwJob::Type::Signal && parsed->jobs[2].signal.targetKind == SignalTargetKind::Yid);
    CHECK(parsed->jobs[3].type == GwJob::Type::Signal && parsed->jobs[3].signal.targetKind == SignalTargetKind::Iid);
    CHECK(parsed->jobs[4].type == GwJob::Type::IzleLeave);
    CHECK(parsed->jobs[5].type == GwJob::Type::IzletStop);
    CHECK(makeKontrolIdrJson() == R"({"t":"idr"})");
}

void testIzletRtpRoundTrip() {
    auto roundTrip = [](bool isH265, const rtc::binary& annexB) {
        auto rtpCfg = std::make_shared<rtc::RtpPacketizationConfig>(0x12345678u, "test", 96, 90000);
        std::shared_ptr<rtc::RtpPacketizer> pkt = isH265
            ? std::static_pointer_cast<rtc::RtpPacketizer>(
                  std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpCfg, 500))
            : std::static_pointer_cast<rtc::RtpPacketizer>(
                  std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpCfg, 500));

        rtc::message_vector outPackets;
        rtc::message_vector inMsgs;
        inMsgs.push_back(rtc::make_message(rtc::binary(annexB), std::make_shared<rtc::FrameInfo>(90000u)));
        pkt->outgoingChain(inMsgs, [&](rtc::message_ptr m) { outPackets.push_back(std::move(m)); });
        if (outPackets.empty()) outPackets = std::move(inMsgs);
        CHECK(outPackets.size() >= 3); // Fragmented into multiple RTP packets (FU-A / FU)

        std::shared_ptr<rtc::MediaHandler> depkt = isH265
            ? std::static_pointer_cast<rtc::MediaHandler>(
                  std::make_shared<rtc::H265RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence))
            : std::static_pointer_cast<rtc::MediaHandler>(
                  std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));

        rtc::binary reassembled;
        for (auto& m : outPackets) {
            rtc::message_vector step{m};
            depkt->incomingChain(step, [&](rtc::message_ptr rm) {
                if (rm && rm->frameInfo) reassembled = *rm;
            });
            for (const auto& r : step) {
                if (r && r->frameInfo) {
                    reassembled = *r;
                }
            }
        }
        CHECK(!reassembled.empty());
        const auto span = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(reassembled.data()), reassembled.size());
        const auto summary = inspectIzletAnnexB(span, isH265 ? IzletCodec::H265 : IzletCodec::H264);
        CHECK(summary.valid && summary.hasConfig && summary.hasPicture && summary.isIdr);
    };

    // Large H.264 SPS + PPS + IDR (> 1500 bytes to force FU-A)
    rtc::binary h264Frame = {
        rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x01), rtc::byte(0x67), rtc::byte(0x42), rtc::byte(0xE0), rtc::byte(0x1F),
        rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x01), rtc::byte(0x68), rtc::byte(0xCE), rtc::byte(0x3C), rtc::byte(0x80),
        rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x01), rtc::byte(0x65)
    };
    h264Frame.resize(h264Frame.size() + 1800, rtc::byte(0xAB));
    roundTrip(false, h264Frame);

    // Large H.265 VPS + SPS + PPS + IDR_W_RADL (> 1500 bytes to force FU)
    rtc::binary h265Frame = {
        rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x01), rtc::byte(0x40), rtc::byte(0x01), rtc::byte(0x0C), rtc::byte(0x01),
        rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x01), rtc::byte(0x42), rtc::byte(0x01), rtc::byte(0x01), rtc::byte(0x02),
        rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x01), rtc::byte(0x44), rtc::byte(0x01), rtc::byte(0xC1), rtc::byte(0x72),
        rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x00), rtc::byte(0x01), rtc::byte(0x26), rtc::byte(0x01), rtc::byte(0xAA), rtc::byte(0xBB)
    };
    h265Frame.resize(h265Frame.size() + 1800, rtc::byte(0xCD));
    roundTrip(true, h265Frame);
}

} // namespace

int main() {
    testKumandaBinaryParsing();
    testH264RelayCache();
    testContractJsonAndIni();
    testIzletH265NalAndGopBuffer();
    testIzletLimitsAndProtocol();
    testIzletRtpRoundTrip();
    std::cout << "oviora-gecit unit tests PASSED\n";
    return 0;
}
