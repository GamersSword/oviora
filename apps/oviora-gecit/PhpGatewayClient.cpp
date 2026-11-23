// SPDX-License-Identifier: GPL-3.0-only
#include "PhpGatewayClient.h"

#include "oviora/Log.h"
#include "oviora/platform/SecretWipe.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <format>

namespace oviora::gecit {

PhpGatewayClient::PhpGatewayClient(const GatewayConfig& config, QObject* parent)
    : QObject(parent),
      apiUrl_(config.apiUrl),
      keyHeader_(QByteArray::fromStdString(config.gatewayKey)),
      net_(new QNetworkAccessManager(this)),
      pollTimer_(new QTimer(this)) {
    pollTimer_->setSingleShot(true);
    connect(pollTimer_, &QTimer::timeout, this, &PhpGatewayClient::pollOnce);
}

PhpGatewayClient::~PhpGatewayClient() {
    stopPolling();
    SecretWipe::wipe(keyHeader_);
}

void PhpGatewayClient::startPolling() {
    if (polling_) return;
    polling_ = true;
    consecutiveErrors_ = 0;
    scheduleNextPoll(0);
}

void PhpGatewayClient::stopPolling() {
    polling_ = false;
    if (pollTimer_) pollTimer_->stop();
    if (activePoll_) {
        activePoll_->abort();
        activePoll_->deleteLater();
        activePoll_ = nullptr;
    }
}

void PhpGatewayClient::scheduleNextPoll(int delayMs) {
    if (!polling_) return;
    pollTimer_->start(std::max(0, delayMs));
}

void PhpGatewayClient::pollOnce() {
    if (!polling_ || activePoll_) return;

    QNetworkRequest req{QUrl(QString::fromStdString(apiUrl_))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setRawHeader("X-Oviora-Gecit", keyHeader_);
    req.setTransferTimeout(5000);

    const std::string body = nlohmann::json{{"a", "gw_poll"}, {"after", lastEventId_}}.dump();
    QNetworkReply* reply = net_->post(req, QByteArray::fromStdString(body));
    activePoll_ = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (activePoll_ == reply) activePoll_ = nullptr;
        const auto netErr = reply->error();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray raw = reply->readAll();
        reply->deleteLater();

        if (!polling_) {
            SecretWipe::wipe(raw);
            return;
        }

        if (netErr != QNetworkReply::NoError || httpStatus >= 400) {
            SecretWipe::wipe(raw);
            ++consecutiveErrors_;
            if (consecutiveErrors_ == 1 || consecutiveErrors_ % 10 == 0) {
                oviora::log::warn("gecit-php",
                                  std::format("gw_poll failed http={} err={} count={}",
                                              httpStatus, int(netErr), consecutiveErrors_));
            }
            const int backoff = std::min(2000, 300 * consecutiveErrors_);
            scheduleNextPoll(backoff);
            return;
        }

        consecutiveErrors_ = 0;
        std::string parseErr;
        auto parsed = parseGwPollResponse(std::string_view(raw.constData(), std::size_t(raw.size())), &parseErr);
        // Wipe response buffer right after parsing because `start` jobs contain PSN tokens.
        SecretWipe::wipe(raw);

        if (!parsed) {
            oviora::log::warn("gecit-php", std::format("gw_poll parse error: {}", parseErr));
            scheduleNextPoll(500);
            return;
        }

        if (parsed->last > lastEventId_) {
            lastEventId_ = parsed->last;
        }
        if (!parsed->jobs.empty() && onJobs_) {
            onJobs_(std::move(parsed->jobs));
        }
        scheduleNextPoll(0);
    });
}

void PhpGatewayClient::postAction(const std::string& jsonBody, const char* opTag, int timeoutMs) {
    QNetworkRequest req{QUrl(QString::fromStdString(apiUrl_))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setRawHeader("X-Oviora-Gecit", keyHeader_);
    req.setTransferTimeout(timeoutMs);

    QNetworkReply* reply = net_->post(req, QByteArray::fromStdString(jsonBody));
    connect(reply, &QNetworkReply::finished, this, [reply, tag = std::string(opTag)]() {
        const auto err = reply->error();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || http >= 400) {
            oviora::log::warn("gecit-php", std::format("{} failed http={} err={}", tag, http, int(err)));
        }
    });
}

void PhpGatewayClient::sendSignal(std::string_view wsid, std::string_view dataJson) {
    sendSignalFor("wsid", wsid, dataJson);
}

void PhpGatewayClient::sendState(std::string_view wsid, std::string_view state, std::string_view error) {
    sendStateFor("wsid", wsid, state, error);
}

void PhpGatewayClient::sendStats(std::string_view wsid, int kbps, double fps, int rttMs, double loss) {
    const nlohmann::json body = {
        {"a", "gw_stats"},
        {"wsid", wsid},
        {"kbps", kbps},
        {"fps", fps},
        {"rtt_ms", rttMs},
        {"loss", loss}
    };
    postAction(body.dump(), "gw_stats");
}

void PhpGatewayClient::sendSignalFor(std::string_view idField, std::string_view idValue, std::string_view dataJson) {
    auto parsedData = nlohmann::json::parse(dataJson, nullptr, false);
    if (parsedData.is_discarded()) return;
    nlohmann::json body = {
        {"a", "gw_signal"},
        {"data", std::move(parsedData)}
    };
    body[std::string(idField)] = idValue;
    postAction(body.dump(), "gw_signal");
}

void PhpGatewayClient::sendStateFor(std::string_view idField, std::string_view idValue, std::string_view state, std::string_view error) {
    nlohmann::json body = {
        {"a", "gw_state"},
        {"state", state},
        {"error", error}
    };
    body[std::string(idField)] = idValue;
    postAction(body.dump(), "gw_state");
}

void PhpGatewayClient::sendIzletStats(std::string_view yid, int kbps, int viewers, int lost, int inKbps) {
    const nlohmann::json body = {
        {"a", "gw_stats"},
        {"yid", yid},
        {"kbps", kbps},
        {"viewers", viewers},
        {"loss", lost},
        {"in_kbps", inKbps}
    };
    postAction(body.dump(), "gw_stats");
}

} // namespace oviora::gecit
