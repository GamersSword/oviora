// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oviora/remoteplay/ChiakiBackend.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace oviora::platform {

// WebRTC sendonly publisher for "İzlet" (desktop PC -> oviora-gecit SFU).
// Media callbacks from ChiakiBackend only copy into bounded queues; all
// libdatachannel packetization and network sends happen on a dedicated thread.
class IzletPublisher final : public QObject {
    Q_OBJECT
public:
    explicit IzletPublisher(QObject* parent = nullptr);
    ~IzletPublisher() override;

    IzletPublisher(const IzletPublisher&) = delete;
    IzletPublisher& operator=(const IzletPublisher&) = delete;

    static bool isAvailable() noexcept;

    bool active() const noexcept;
    QString codec() const;

    // Starts a sendonly PeerConnection for `codec` ("h264" or "h265") with the
    // given `iceServers` array (`[{urls:[...], username?, credential?}]`).
    // `sendSignal` is invoked on this object's thread with offer/candidate JSON.
    bool start(const QString& codec,
               const QJsonArray& iceServers,
               std::function<void(const QJsonObject&)> sendSignal);

    // Applies an incoming remote signal (`{"type":"answer","sdp":...}` or
    // `{"candidate":...,"mid":...}`). Safe to call from the Qt thread.
    void onRemoteSignal(const QJsonObject& signal);

    void stop();

    // Thread-safe media ingestion hooks (called on Chiaki video/audio threads).
    void pushVideo(oviora::remoteplay::SampleKind kind,
                   std::span<const std::uint8_t> annexB,
                   bool isIdr,
                   bool lostOrRecovered);
    void pushAudio(std::span<const std::uint8_t> opusPacket,
                   std::uint32_t frameSamples);

    // Builds lifetime-safe tee callbacks suitable for ChiakiBackend::setMediaTee
    // or EngineController::setRemotePlayMediaTee.
    oviora::remoteplay::MediaRelayCallbacks makeTeeCallbacks();

signals:
    void stateChanged(const QString& state);
    void idrRequested();

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

} // namespace oviora::platform
