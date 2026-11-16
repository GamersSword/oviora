#pragma once
#include "RemotePlaySource.h"
#include "oviora/sink/CaptureAudioSession.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace oviora::source {
// Network/decoder owner keeps consuming reference pictures while the GPU is
// busy. Only already-decoded presentation opportunities use the latest slot.
class RemotePlaySessionSource final : public IFrameSource {
public:
    ~RemotePlaySessionSource() override { close(); }
    bool connect(RemotePlayConnectDesc desc);
    // True while the owner loop, including its reconnect retries, still runs.
    bool alive() const { return alive_.load(); }
    bool failed() const { std::lock_guard lock(mutex_); return failed_; }
    bool open(const SourceOpenDesc&) override { return false; }
    const SourceInfo& info() const override { return info_; }
    SourceReadStatus read(pipeline::FramePacket&, const AVFrame**) override;
    bool seek(const pipeline::Rational&) override { return false; }
    void close() noexcept override;
    void controller(remoteplay::ControllerState state);
    void loginPin(std::string pin);
    // Oviora: PS5 rest mode (sent once on the owner thread, the caller stops the session a
    // moment later), the microphone's wanted state (applied once the console is connected) and
    // its 10 ms blocks (480 stereo frames, 48 kHz, 16 bit; at most 20 waiting, older ones go).
    void gotoBed();
    void microphone(bool on);
    void microphonePcm(std::vector<std::int16_t> block);
    // The link as the old Oviora showed it: packet loss averaged over the last ~2 s (0..1),
    // the measured video bitrate, the console's round trip (-1 unknown) and whether the
    // microphone is on at the console.
    struct Link {bool ready=false;double packetLoss=0,measuredMbps=0,rttMs=-1;bool microphoneOn=false;};
    Link link() const {std::lock_guard lock(mutex_);return link_;}
    remoteplay::ControllerFeedback takeFeedback();
    remoteplay::SessionInbox::Snapshot sessionSnapshot() const;
    uint64_t skipped() const;
    struct Rates {uint64_t received=0,decoded=0,ingressDropped=0;double receivedFps=0,decodedFps=0;bool ready=false;};
    Rates rates() const;
    struct RecoveryStatus {bool active=false;unsigned attempts=0;std::wstring message;};
    RecoveryStatus recoveryStatus()const {std::lock_guard lock(mutex_);return recovery_;}
    sink::CaptureAudioState audioState() const { return audio_.snapshot(); }
    void setAudioGain(float value) { audio_.setGain(value); }
    void setAudioSync(unsigned mode, int offset) { audio_.setSync(mode, offset); }
    void videoPresented(double pts, int64_t host) { audio_.videoPresented(pts, host); }
    void videoReset(bool resetAudio=true) { audio_.videoReset(resetAudio); }
private:
    friend struct RemotePlaySessionSourceTestAccess;
    void publishDecoded(const AVFrame*,pipeline::FramePacket,const SourceInfo&);
    void run(std::stop_token, RemotePlayConnectDesc);
    remoteplay::ControllerState takeControllerLocked(remoteplay::HostTime now);
    struct Frame { std::shared_ptr<AVFrame> frame; pipeline::FramePacket packet; SourceInfo info; };
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::jthread owner_;
    std::atomic<bool> alive_{false};
    bool initialized_=false, started_=false, failed_=false;
    std::optional<Frame> latest_;
    std::shared_ptr<AVFrame> view_;
    SourceInfo info_, publishedInfo_;
    remoteplay::SessionInbox::Snapshot snapshot_;
    std::shared_ptr<const remoteplay::SessionInbox> telemetryInbox_;
    remoteplay::ControllerState controller_;
    std::deque<std::pair<remoteplay::HostTime,remoteplay::ControllerState>> pendingControllers_;
    remoteplay::HostTime controllerStamp_=0;
    std::string pin_;
    bool bedRequested_=false;
    bool micWanted_=false;
    std::deque<std::vector<std::int16_t>> micBlocks_;
    Link link_;
    uint64_t skipped_=0;
    Rates rates_;
    RecoveryStatus recovery_;
    uint64_t publishedSequence_=0;
    remoteplay::ControllerFeedback feedback_;
    sink::CaptureAudioSession audio_;
};
}
