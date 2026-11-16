// Oviora: see PsnAccountLookup.h. The same client and scopes as the old Oviora's PSNAccountID; the
// token never reaches a log, the screen or a file.
#include "oviora/platform/PsnAccountLookup.h"
#include "oviora/Log.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

namespace {
const QString kClientId = QStringLiteral("ba495a24-818c-472b-b12d-ff231c1b5745");
const QString kClientSecret = QStringLiteral("mvaiZkRsAsI1IBkY");
const QString kRedirect = QStringLiteral("https://remoteplay.dl.playstation.net/remoteplay/redirect");
const QString kScope = QStringLiteral("psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect sessionManager:remotePlaySession.system.update");
const QString kTokenUrl = QStringLiteral("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token");

QByteArray basicAuth()
{
	return "Basic " + (kClientId + QLatin1Char(':') + kClientSecret).toUtf8().toBase64();
}
}

PsnAccountLookup::PsnAccountLookup(QObject *parent) : QObject(parent) {}

QString PsnAccountLookup::loginUrl() const
{
	QUrl url(QStringLiteral("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize"));
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("service_entity"), QStringLiteral("urn:service-entity:psn"));
	q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
	q.addQueryItem(QStringLiteral("client_id"), kClientId);
	q.addQueryItem(QStringLiteral("redirect_uri"), kRedirect);
	q.addQueryItem(QStringLiteral("scope"), kScope);
	q.addQueryItem(QStringLiteral("request_locale"), QStringLiteral("en_US"));
	q.addQueryItem(QStringLiteral("ui"), QStringLiteral("pr"));
	q.addQueryItem(QStringLiteral("service_logo"), QStringLiteral("ps"));
	q.addQueryItem(QStringLiteral("layout_type"), QStringLiteral("popup"));
	q.addQueryItem(QStringLiteral("smcid"), QStringLiteral("remoteplay"));
	// Always the account picker: the point is a different account than the one signed in before
	q.addQueryItem(QStringLiteral("prompt"), QStringLiteral("always"));
	q.addQueryItem(QStringLiteral("PlatformPrivacyWs1"), QStringLiteral("minimal"));
	url.setQuery(q);
	return url.toString(QUrl::FullyEncoded);
}

void PsnAccountLookup::fail(const QString &text)
{
	working = false;
	emit busyChanged();
	emit failed(text);
}

void PsnAccountLookup::finish(const QString &redirectUrl)
{
	if (working)
		return;
	const QString code = QUrlQuery(QUrl(redirectUrl)).queryItemValue(QStringLiteral("code"));
	if (code.isEmpty()) {
		fail(tr("PSN girişi tamamlanmadı."));
		return;
	}
	working = true;
	emit busyChanged();
	if (!nam)
		nam = new QNetworkAccessManager(this);
	QNetworkRequest req{QUrl(kTokenUrl)};
	req.setRawHeader("Authorization", basicAuth());
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
	req.setTransferTimeout(15000);
	QUrlQuery body;
	body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
	body.addQueryItem(QStringLiteral("code"), code);
	body.addQueryItem(QStringLiteral("scope"), kScope);
	body.addQueryItem(QStringLiteral("redirect_uri"), kRedirect);
	QNetworkReply *reply = nam->post(req, body.toString(QUrl::FullyEncoded).toUtf8());
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		const QString token = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("access_token")).toString();
		if (reply->error() != QNetworkReply::NoError || token.isEmpty()) {
			oviora::log::warn("psn-lookup", "token request failed");
			fail(tr("PSN'den hesap bilgisi alınamadı. Tekrar dene."));
			return;
		}
		// The token's own description carries the account's user id (decimal)
		QNetworkRequest info{QUrl(kTokenUrl + QLatin1Char('/') + token)};
		info.setRawHeader("Authorization", basicAuth());
		info.setTransferTimeout(15000);
		QNetworkReply *second = nam->get(info);
		connect(second, &QNetworkReply::finished, this, [this, second]() {
			second->deleteLater();
			const QJsonObject obj = QJsonDocument::fromJson(second->readAll()).object();
			const QString userId = obj.value(QStringLiteral("user_id")).toString();
			bool ok = false;
			const qulonglong id = userId.toULongLong(&ok);
			if (second->error() != QNetworkReply::NoError || !ok) {
				oviora::log::warn("psn-lookup", "account id request failed");
				fail(tr("PSN'den hesap bilgisi alınamadı. Tekrar dene."));
				return;
			}
			// The id as the console knows it: 8 bytes little endian, base64
			QByteArray bytes(8, '\0');
			for (int i = 0; i < 8; ++i)
				bytes[i] = char((id >> (8 * i)) & 0xFF);
			const QString onlineId = obj.value(QStringLiteral("online_id")).toString();
			oviora::log::info("psn-lookup", "account id received");
			working = false;
			emit busyChanged();
			emit found(QString::fromLatin1(bytes.toBase64()), onlineId);
		});
	});
}
