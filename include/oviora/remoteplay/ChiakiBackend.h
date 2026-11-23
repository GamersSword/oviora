// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SessionInbox.h"
#include "WatchRoute.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>

namespace oviora::remoteplay {
// No serialization/ostream support: never put registration/session keys into
// command lines, logs, fixtures or source control. DPAPI storage is NOT in patch 01.
struct PairingCredentials {
    PairingCredentials() = default;
    PairingCredentials(const PairingCredentials&) = delete;
    PairingCredentials& operator=(const PairingCredentials&) = delete;
    PairingCredentials(PairingCredentials&&) noexcept;
    PairingCredentials& operator=(PairingCredentials&&) noexcept;
    AccountId accountId{};
    std::array<char,16> registrationKey{};
    std::array<std::uint8_t,16> sessionKey{};
    ~PairingCredentials();
};
struct BackendResult {
    bool ok=false;
    int code=0;
    // One of our fixed operation names, never an unfiltered server string.
    std::string operation;
};
// Oviora: the Oviora server makes the PSN requests that need a console owner's token
// (a friend's PS5); same contract as chiaki's ChiakiHolepunchRelay. Called from
// chiaki threads, each call may block on the server; returned buffers use malloc.
class PsnRelay {
public:
    virtual ~PsnRelay()=default;
    virtual int http(const char* op,const char* sessionId,const char* body,std::size_t bodyLen,char** resp,std::size_t* respLen)=0;
    virtual bool open(const char* pushctx)=0;
    virtual int poll(std::int64_t after,char*** events,std::size_t* count,std::int64_t* last)=0;
    virtual void close()=0;
};
// Oviora: a PS5 reached over the internet through PSN (hole punching) instead of the LAN.
struct PsnRoute {
    std::string duid;   // Console id, hex.
    std::string token;  // PSN access token; empty when relay is set.
    AccountId accountId{};
    std::shared_ptr<PsnRelay> relay;
};
struct NativeConnectRequest {
    std::string host;
    std::string consoleId; // Stable discovery host_id / registration MAC, normalized hex.
    VideoProfile video;
    bool viewOnly=false;
    // Yayın → Uyarlanabilir kalite: the console is offered the next lower resolution too and may
    // switch to it mid-stream on a weak line (the picture then changes size).
    bool adaptiveResolution=false;
    PairingCredentials credentials;
    std::shared_ptr<const PsnRoute> psn; // Set: connect over PSN, host and credentials unused.
    std::shared_ptr<WatchRoute> watch;   // Set: watch a friend's İzlet broadcast over WebRTC (no PS5 session).
};
struct NativeSnapshot {
    bool started=false,connected=false;
    std::uint64_t warnings=0,errors=0,videoCallbacks=0,audioCallbacks=0;
    int lastQuitReason=0,lastApiError=0;
    uint64_t callbackRejected=0,packetReceived=0,packetLost=0;
    uint64_t transportErrors=0,assemblyErrors=0;
    bool automaticRetryAllowed=true;
    bool startupRetryAllowed=false; // Only transient RP_IN_USE, never an auth rejection.
    int64_t serverTargetBitrate=-1; // Raw upstream field; diagnostic opt-in only.
    uint64_t qualityReports=0;
    // Oviora: the stream statistics the old Oviora showed (Yayın bilgileri): the congestion
    // control's packet loss share (0..1, its own short window), the measured video bitrate
    // and the console's own round trip estimate (-1 until it has reported one).
    double packetLoss=0,measuredMbps=0,consoleRttMs=-1;
    // Oviora: the console's reason when it refused the session request (0 = none reported):
    // 0x80108b09 the pairing is not accepted, 0x80108b02 the pairing's PSN account is not signed in,
    // 0x80108b12 Remote Play is off on the PS5 for that PSN account (found 2026-11-15: a second account)
    std::uint32_t applicationReason=0;
};
BackendResult initializeChiaki();
// Oviora: where the (single) connection is, for the connecting screen, and whether the console
// says the screen it shows cannot be streamed (protected content). Read from any thread.
enum class ConnectStage : int { Idle=0, PsnReach=1, PsnPunch=2, Opening=3, PsnData=4, Connected=5 };
ConnectStage connectStage();
bool consoleCantDisplay();
// Oviora: the console's on-screen keyboard. While it is open the PC types into it: setText sends the
// whole text, accept/reject close it. revision grows with every change the console reports.
struct ConsoleKeyboard { bool open=false; std::string text; std::uint64_t revision=0; };
ConsoleKeyboard consoleKeyboard();
bool consoleKeyboardSetText(const std::string& utf8);
bool consoleKeyboardAccept();
bool consoleKeyboardReject();
struct MediaRelayCallbacks {
    // Called on Chiaki's video thread with raw Annex-B bytes (valid during the callback only).
    std::function<void(SampleKind kind,std::span<const std::uint8_t> bytes,
        std::uint32_t width,std::uint32_t height,bool idr,bool lostOrRecovered)> onVideo;
    // Called on Chiaki's audio thread with raw Opus packet bytes (empty span on PLC).
    std::function<void(std::span<const std::uint8_t> opusPacket,
        std::uint32_t channels,std::uint32_t rate,std::uint32_t frameSize)> onAudio;
};
class ChiakiBackend {
public:
    ChiakiBackend();
    ~ChiakiBackend();
    ChiakiBackend(const ChiakiBackend&)=delete;
    ChiakiBackend& operator=(const ChiakiBackend&)=delete;
    // All methods, including snapshot(), must be called from one session-owner thread,
    // NEVER from a Chiaki callback. This object is not a renderer or a decoder.
    // discardMedia (PS5 手柄控制): video samples are counted and accepted without
    // a copy, audio goes to a no-op sink, so Chiaki keeps its reference-frame
    // bookkeeping and never reports a dropped sample to the console as a
    // corrupt frame. The DualSense channel is forced on; feedback is unchanged.
    // relay: headless transport mode (no decoder, no mic encoder; raw H.264/Opus
    // packets are forwarded directly to the callbacks).
    BackendResult start(const NativeConnectRequest&,SessionInbox::Token,bool discardMedia=false,
        MediaRelayCallbacks relay={});
    // Attaches or detaches a media tee listener while normal desktop decode/audio
    // playback continues. Empty callbacks disable the tee (near-zero cost when off).
    // Thread-safe; never invoke from inside a Chiaki callback.
    void setMediaTee(MediaRelayCallbacks tee);
    BackendResult stop(); // Does not call PS5 goto_bed; must precede inbox.finishStop().
    BackendResult requestIdr();
    // Oviora: puts the PS5 into rest mode (the ctrl channel sends it; the console then ends
    // the session). The caller stops the session a moment later so the message leaves first.
    BackendResult gotoBed();
    // Oviora: the microphone to the PS5. setMicrophone tells the console it is on or off (the
    // first "on" connects it); submitMicrophone encodes one 10 ms block (480 frames, 48 kHz,
    // stereo, 16-bit) with the lib's Opus encoder and sends it while the microphone is on.
    BackendResult setMicrophone(bool on);
    BackendResult submitMicrophone(const std::int16_t* pcm,std::size_t frames);
    BackendResult submitController(const ControllerState&);
    BackendResult submitLoginPin(std::string_view);
    NativeSnapshot snapshot() const;
    ControllerFeedback takeFeedback();
    std::string watchEndReason() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::string lastWatchEndReason_;
};
struct PairResult {
    BackendResult result;
    PairingCredentials credentials;
    std::array<std::uint8_t,6> mac{};
    std::string nickname;
    bool canceled=false,timedOut=false;
};
// Synchronous OWNERSHIP operation; run on a dedicated session/setup worker, not HWND.
// Cancellation stops the upstream registration and joins before returning.
PairResult pairLocalPs5(std::string host,const AccountId&,std::string_view eightDigitPin,
    std::stop_token stop={},std::chrono::milliseconds timeout=std::chrono::seconds(30));
} // namespace oviora::remoteplay
