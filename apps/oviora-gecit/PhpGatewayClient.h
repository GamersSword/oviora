// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "DataChannelProtocol.h"
#include "GatewayConfig.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace oviora::gecit {

// HTTP client talking to PHP `api.php` over 127.0.0.1 using `X-Oviora-Gecit`.
// Must live on the Qt main thread.
class PhpGatewayClient final : public QObject {
    Q_OBJECT
public:
    explicit PhpGatewayClient(const GatewayConfig& config, QObject* parent = nullptr);
    ~PhpGatewayClient() override;

    void setOnJobs(std::function<void(std::vector<GwJob>)> callback) { onJobs_ = std::move(callback); }

    void startPolling();
    void stopPolling();

    void sendSignal(std::string_view wsid, std::string_view dataJson);
    void sendState(std::string_view wsid, std::string_view state, std::string_view error);
    void sendStats(std::string_view wsid, int kbps, double fps, int rttMs, double loss);

    void sendSignalFor(std::string_view idField, std::string_view idValue, std::string_view dataJson);
    void sendStateFor(std::string_view idField, std::string_view idValue, std::string_view state, std::string_view error);
    void sendIzletStats(std::string_view yid, int kbps, int viewers, int lost = 0, int inKbps = 0);

private:
    void scheduleNextPoll(int delayMs);
    void pollOnce();
    void postAction(const std::string& jsonBody, const char* opTag, int timeoutMs = 4000);

    std::string apiUrl_;
    QByteArray keyHeader_;
    QNetworkAccessManager* net_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QNetworkReply* activePoll_ = nullptr;
    std::function<void(std::vector<GwJob>)> onJobs_;
    std::int64_t lastEventId_ = 0;
    bool polling_ = false;
    int consecutiveErrors_ = 0;
};

} // namespace oviora::gecit
