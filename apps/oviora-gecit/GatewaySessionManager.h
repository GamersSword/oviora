// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "DataChannelProtocol.h"
#include "GatewayConfig.h"
#include "H264RelayCache.h"
#include "IzletBroadcastSession.h"
#include "PhpGatewayClient.h"
#include "WebRtcGatewaySession.h"

#include <QObject>
#include <QTimer>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace oviora::gecit {

// Coordinates active WebRTC <-> PS5 (or mock H.264 file) sessions on the Qt main thread,
// while each session runs its ChiakiBackend or mock media pump on its own owner thread.
class GatewaySessionManager final : public QObject {
    Q_OBJECT
public:
    GatewaySessionManager(const GatewayConfig& config,
                          PhpGatewayClient& phpClient,
                          std::filesystem::path mockH264Path = {},
                          QObject* parent = nullptr);
    ~GatewaySessionManager() override;

    void handleJobs(std::vector<GwJob> jobs);
    void shutdownAll();
    std::size_t activeCount() const;

private:
    struct ActiveSession;

    void startSession(GwStartJob job);
    void signalSession(const GwSignalJob& job);
    void stopSession(const std::string& wsid, const std::string& reason, bool notifyPhp);
    void startIzletBroadcast(const GwIzletStartJob& job);
    void joinIzletViewer(const GwIzleJoinJob& job);
    void leaveIzletViewer(const std::string& iid, const std::string& reason, bool notifyPhp);
    void stopIzletBroadcast(const std::string& yid, const std::string& reason, bool notifyPhp);
    void publishStats();
    void runRealPs5Owner(const std::shared_ptr<ActiveSession>& session, GwStartJob job);
    void runMockSourceOwner(const std::shared_ptr<ActiveSession>& session, GwStartJob job);

    const GatewayConfig& config_;
    PhpGatewayClient& php_;
    std::filesystem::path mockH264Path_;
    std::vector<AnnexBTestFrame> mockFrames_;
    QTimer* statsTimer_ = nullptr;

    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<ActiveSession>> sessions_;
    std::unordered_map<std::string, std::shared_ptr<IzletBroadcastSession>> izletBroadcasts_;
    std::unordered_map<std::string, std::string> viewerToBroadcast_;
    std::chrono::steady_clock::time_point lastIzletStatsAt_ = std::chrono::steady_clock::now();
    bool shuttingDown_ = false;
};

} // namespace oviora::gecit
