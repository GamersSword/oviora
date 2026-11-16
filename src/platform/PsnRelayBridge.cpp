// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "oviora/platform/PsnRelayBridge.h"
#include "oviora/platform/OvioraServer.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cstdlib>
#include <cstring>

PsnRelayBridge::PsnRelayBridge(OvioraServer *server, const QString &lease_sid)
	: srv(server), sid(lease_sid)
{
}

static char *dup_bytes(const QByteArray &b, std::size_t *len)
{
	char *out = static_cast<char *>(malloc(size_t(b.size()) + 1));
	if(!out)
		return nullptr;
	memcpy(out, b.constData(), size_t(b.size()));
	out[b.size()] = '\0';
	if(len)
		*len = size_t(b.size());
	return out;
}

int PsnRelayBridge::http(const char *op, const char *session_id, const char *body, std::size_t body_len, char **resp, std::size_t *resp_len)
{
	*resp = nullptr;
	*resp_len = 0;
	if(!srv)
		return -1;
	QJsonObject req { { "sid", sid }, { "op", QString::fromLatin1(op) } };
	if(session_id && *session_id)
		req["session_id"] = QString::fromLatin1(session_id);
	if(body && body_len)
		req["body"] = QString::fromUtf8(body, qsizetype(body_len));
	QString error;
	const QJsonObject r = srv->callBlocking(QStringLiteral("psn_relay_http"), req, &error, 20000);
	if(!error.isEmpty() || !r.contains("status"))
		return -1;
	*resp = dup_bytes(r.value("body").toString().toUtf8(), resp_len);
	return r.value("status").toInt();
}

bool PsnRelayBridge::open(const char *pushctx)
{
	if(!srv)
		return false;
	QString error;
	const QJsonObject r = srv->callBlocking(QStringLiteral("psn_relay_open"),
		{ { "sid", sid }, { "pushctx", QString::fromLatin1(pushctx) } }, &error, 20000);
	return error.isEmpty() && r.value("ok").toBool();
}

int PsnRelayBridge::poll(std::int64_t after, char ***events, std::size_t *count, std::int64_t *last)
{
	*events = nullptr;
	*count = 0;
	if(!srv)
		return -1;
	QString error;
	const QJsonObject r = srv->callBlocking(QStringLiteral("psn_relay_poll"),
		{ { "sid", sid }, { "after", double(after) } }, &error, 10000);
	if(!error.isEmpty())
		return 0;	// a missed poll is retried; the library gives up on its own deadline
	const QJsonArray list = r.value("events").toArray();
	if(!list.isEmpty())
	{
		*events = static_cast<char **>(calloc(size_t(list.size()), sizeof(char *)));
		if(*events)
		{
			for(const QJsonValue &v : list)
			{
				const QJsonObject e = v.toObject();
				(*events)[(*count)++] = dup_bytes(e.value("data").toString().toUtf8(), nullptr);
				const std::int64_t id = std::int64_t(e.value("id").toDouble());
				if(id > *last)
					*last = id;
			}
		}
	}
	const double server_last = r.value("last").toDouble();
	if(std::int64_t(server_last) > *last)
		*last = std::int64_t(server_last);
	const QString state = r.value("state").toString();
	if(state == QLatin1String("open"))
		return 1;
	if(state == QLatin1String("failed") || state == QLatin1String("closed"))
		return -1;
	return 0;
}

void PsnRelayBridge::close()
{
	if(closed || !srv)
		return;
	closed = true;
	QString error;
	srv->callBlocking(QStringLiteral("psn_relay_close"), { { "sid", sid } }, &error, 5000);
}
