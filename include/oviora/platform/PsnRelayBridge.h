// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "oviora/remoteplay/ChiakiBackend.h"

#include <QPointer>
#include <QElapsedTimer>
#include <QString>

#include "oviora/platform/OvioraServer.h"

/**
 * Oviora: the PSN requests of a friend's console leased with "relay" go through the Oviora
 * server's psn_relay_* actions; this PC never gets the owner's token. Called from chiaki
 * threads; each call waits for the server (bounded), never on the GUI thread.
 */
class PsnRelayBridge final : public oviora::remoteplay::PsnRelay
{
public:
	PsnRelayBridge(OvioraServer *server, const QString &lease_sid);

	int http(const char *op, const char *session_id, const char *body, std::size_t body_len, char **resp, std::size_t *resp_len) override;
	bool open(const char *pushctx) override;
	int poll(std::int64_t after, char ***events, std::size_t *count, std::int64_t *last) override;
	void close() override;

private:
	QPointer<OvioraServer> srv;
	QString sid;
	bool closed = false;
	// Timing logs: since the relay opened, and the last state the server reported
	QElapsedTimer opened_at;
	QString last_state;
};
