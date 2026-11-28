// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "oviora/platform/OvioraServer.h"
#include "oviora/platform/SecretStore.h"
#include "oviora/platform/SecretWipe.h"
#include <QBuffer>
#include <cstring>
#include "oviora/platform/DiscordPresence.h"
#include "oviora/RuntimePaths.h"
#include <QSemaphore>
#include <memory>
#include "qrcodegen.hpp"
#include <QRegularExpression>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrent>
#include <QGuiApplication>
#include <QIcon>
#include <QRegularExpression>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QWindow>


#include <openssl/evp.h>
#include <QtZlib/zlib.h>
#include <openssl/rand.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wlanapi.h>
#endif
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(yakamozServer, "oviora.server")

static const char *GROUP = "yakamoz_server";
static const char *CODE_PREFIX = "YAKAMOZ-SUNUCU-";
static const int POLL_MS = 3000;
static const int ACCOUNT_MS = 10000;
// A release on GitHub reaches an app left open within this long (one GitHub API call each; the
// unauthenticated limit is 60 an hour). It downloads in the background; installing waits for the game
static const int UPDATE_CHECK_MS = 10 * 60 * 1000;
static const int PS5_TARGET = 1000100;
static const char *OLD_SUFFIX = ".eski";
// The program's file; "Yakamoz.exe" was its old name and is only a starter for old copies now
static const char *APP_EXE = "Oviora.exe";
static const char *OLD_APP_EXE = "Yakamoz.exe";

// The new Oviora's own file in its data folder: the old Oviora (same organization and app name)
// keeps its settings in the registry or its portable folder and must never be touched
static QString platformSettingsPath()
{
	return QString::fromStdWString((oviora::runtime::localDataDirectory() / L"platform.ini").wstring());
}
#define APP_SETTINGS(name) QSettings name(platformSettingsPath(), QSettings::IniFormat)

OvioraServer::OvioraServer(Hooks hooks, QObject *parent)
	: QObject(parent), hooks(std::move(hooks)), net(new QNetworkAccessManager(this))
{
	load();
	cleanupOldFiles();
	test_psn_links = QCoreApplication::arguments().contains(QStringLiteral("--test-psn-links"));
	test_github_update = QCoreApplication::arguments().contains(QStringLiteral("--test-github-update"));
	if(test_github_update)
		QTimer::singleShot(0, this, [this]() { githubCheckUpdate(); });
	test_groups = QCoreApplication::arguments().contains(QStringLiteral("--test-groups"));
	{
		const QStringList args = QCoreApplication::arguments();
		const qsizetype at = args.indexOf(QStringLiteral("--test-consoles"));
		if(at >= 0 && at + 1 < args.size())
			test_consoles = qBound(1, args.at(at + 1).toInt(), 200);
	}
	// Shortcuts, the app list entry and the old exe: a moment after start, the old exe's starter has ended by then
	QTimer::singleShot(4000, this, &OvioraServer::takeOverOldName);
	poll_timer.setInterval(POLL_MS);
	connect(&poll_timer, &QTimer::timeout, this, [this]() {
		post("poll", { { "sid", session_id } }, [this](const QJsonObject &r, const QString &error) {
			if(session_state != "playing" || !error.isEmpty())
				return;
			if(!r.value("kick").toBool())
			{
				takeSessionInfo(r);
				return;
			}
			qCInfo(yakamozServer) << "The session was ended:" << r.value("reason").toString();
			QString host = session_host;
			endSession();
			if(this->hooks.stop_session)
				this->hooks.stop_session();
			// The server says why (time is up, sharing turned off, a newer session, ...); older servers did not
			const QString why = r.value("text").toString().trimmed();
			emit message(tr("Oturum sonlandırıldı"), !why.isEmpty() ? why : host == user_name
				? tr("Oturum yönetim sitesinden kapatıldı.") : tr("%1 oturumu sonlandırdı.").arg(host));
		});
	});
	// Friends, requests and who plays where change on other PCs: look again now and then
	account_timer.setInterval(ACCOUNT_MS);
	// While this PC streams, the account and groups are asked every third time (30 s); notifications (a friend's
	// ask, a message) still every 10 s
	connect(&account_timer, &QTimer::timeout, this, [this]() {
		const bool streaming = this->hooks.session_active && this->hooks.session_active();
		if(streaming && (++account_ticks % 3) != 0)
		{
			if(configured() && !token.isEmpty())
				fetchEvents();
			return;
		}
		refresh();
	});
	update_timer.setInterval(UPDATE_CHECK_MS);
	connect(&update_timer, &QTimer::timeout, this, &OvioraServer::checkUpdate);
	if(configured() && !token.isEmpty())
		loggedIn();

#ifdef OVIORA_TEST_HOOKS
	// Testing without clicking: register (or log in), connect to a console, install updates at once.
	// Oviora: only in test builds (-DOVIORA_TEST_HOOKS): a login from the environment and updates that
	// install themselves have no place in a release.
	QString test_login = qEnvironmentVariable("YAKAMOZ_SERVER_TEST_LOGIN");
	if(!test_login.isEmpty() && test_login.contains(':'))
		QTimer::singleShot(2000, this, [this, test_login]() {
			QString name = test_login.section(':', 0, 0), pass = test_login.section(':', 1);
			post("register", { { "user", name }, { "pass", pass }, { "token", QString() } }, [this, name, pass](const QJsonObject &r, const QString &error) {
				if(!error.isEmpty())
					login(name, pass);
				else
					takeLogin(r);
			});
		});
	if(qEnvironmentVariableIntValue("YAKAMOZ_SERVER_TEST_APPLY") == 1)
		connect(this, &OvioraServer::updateReady, this, &OvioraServer::applyUpdate);
	QString test_connect = qEnvironmentVariable("YAKAMOZ_SERVER_TEST_CONNECT");  // host:duid
	if(test_connect.contains(':'))
		QTimer::singleShot(12000, this, [this, test_connect]() { connectTo(test_connect.section(':', 0, 0), test_connect.section(':', 1)); });
#endif
	connect(this, &OvioraServer::updateChanged, this, [this]() {
		if(update_state != "downloading")
			qCInfo(yakamozServer) << "update:" << update_state << update_text;
	});
	connect(this, &OvioraServer::message, this, [](const QString &title, const QString &text) { qCInfo(yakamozServer) << "message:" << title << text; });
}

// First start after the old Oviora installed this one as an update: its login, the server, the
// installed release and the notification choices come from the old program's settings (portable
// "ayarlar" folder next to the exe, else the registry). Only read: the old files are never written.
static void takeOverOldSettings()
{
	APP_SETTINGS(now);
	if(now.contains(QString(GROUP) + "/installed_version") || now.contains(QString(GROUP) + "/token"))
		return;
	const QString app = QCoreApplication::applicationDirPath();
	QList<QSettings *> sources;
	for(const QString name : { QStringLiteral("Oviora"), QStringLiteral("Yakamoz") })
	{
		const QString ini = app + "/ayarlar/" + name + "/" + name + ".ini";
		if(QFileInfo::exists(ini))
			sources << new QSettings(ini, QSettings::IniFormat);
	}
	sources << new QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("Oviora"), QStringLiteral("Oviora"));
	for(QSettings *old : sources)
	{
		old->beginGroup(GROUP);
		const QStringList keys = old->childKeys();
		if(!keys.isEmpty() && (old->contains("token") || old->contains("installed_version")))
		{
			for(const QString &key : keys)
				if(key == "srv" || key == "token" || key == "user" || key == "installed_version" || key == "events_last" || key.startsWith("notify_"))
					now.setValue(QString(GROUP) + "/" + key, old->value(key));
			qCInfo(yakamozServer) << "Took over the old Oviora's login and release from" << old->fileName();
			old->endGroup();
			break;
		}
		old->endGroup();
	}
	qDeleteAll(sources);
}

void OvioraServer::load()
{
	takeOverOldSettings();
	APP_SETTINGS(s);
	s.beginGroup(GROUP);
	server_url = s.value("srv").toString();
	// Oviora: kept encrypted for this Windows user; a plain one from an older version is rewritten below
	const QString stored_token = s.value("token").toString();
	token = SecretStore::unprotect(stored_token);
	if(SecretStore::needsUpgrade(stored_token))
		s.setValue("token", SecretStore::protect(token));
	user_name = s.value("user").toString();
	account_status = token.isEmpty() ? "none" : "active";
	s.endGroup();
	// Only a package made by the server has sunucu.txt next to the program; without it this is a
	// plain Yakamoz and nothing of the platform shows
	QFile f(appDir() + "/sunucu.txt");
	if(!f.open(QIODevice::ReadOnly) || !readConnectionCode(QString::fromUtf8(f.readAll()).trimmed()))
		server_url.clear();
}

void OvioraServer::save()
{
	APP_SETTINGS(s);
	s.beginGroup(GROUP);
	s.setValue("srv", server_url);
	s.setValue("token", SecretStore::protect(token));
	s.setValue("user", user_name);
	s.endGroup();
}

QString OvioraServer::installedVersion() const
{
#ifndef OVIORA_PLATFORM_UPDATES
	// Development build: the server only serves the release it offers; until the new Oviora is that
	// release (stage 6) this build reports the offered one. Learned from the first update check.
	if(!dev_server_version.isEmpty())
		return dev_server_version;
#endif
	APP_SETTINGS(s);
	return s.value(QString(GROUP) + "/installed_version").toString();
}

bool OvioraServer::readConnectionCode(const QString &code)
{
	if(!code.startsWith(CODE_PREFIX))
		return false;
	QJsonObject o = QJsonDocument::fromJson(QByteArray::fromBase64(code.mid(int(strlen(CODE_PREFIX))).toLatin1(),
		QByteArray::Base64UrlEncoding)).object();
	// Oviora: only https; the login token and the PSN login travel to this address
	if(o.value("v").toInt() != 3 || !o.value("srv").toString().startsWith("https://"))
		return false;
	// A login for another server is worth nothing here
	if(o.value("srv").toString() != server_url)
	{
		token.clear();
		account_status = "none";
		user_name.clear();
	}
	server_url = o.value("srv").toString();
	cert_fp = o.value("fp").toString().toLower();
	publisher_pub = o.value("pub").toString();
	save();
	return true;
}

// ---------------------------------------------------------------- requests

void OvioraServer::pin(QNetworkReply *reply)
{
	// No domain, so a self-signed certificate: trust exactly the one pinned by the connection code
	QString fp = cert_fp;
	auto pinned = [reply, fp]() {
		return !fp.isEmpty() && QString::fromLatin1(reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256).toHex()) == fp;
	};
	connect(reply, &QNetworkReply::sslErrors, reply, [reply, pinned](const QList<QSslError> &errors) {
		if(pinned())
			reply->ignoreSslErrors(errors);
	});
	connect(reply, &QNetworkReply::encrypted, reply, [reply, pinned, fp]() {
		if(!fp.isEmpty() && !pinned())
			reply->abort();
	});
}

QNetworkRequest OvioraServer::apiRequest() const
{
	QNetworkRequest req{QUrl(server_url)};
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	req.setTransferTimeout(30000);
	return req;
}

QByteArray OvioraServer::requestBody(const QString &action, QJsonObject body) const
{
	body["a"] = action;
	// The server only lets the current Yakamoz do anything but update
	body["client"] = "win:" + installedVersion();
	if(!token.isEmpty() && !body.contains("token"))
		body["token"] = token;
	return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QNetworkReply *OvioraServer::post(const QString &action, QJsonObject body, Callback done)
{
	QNetworkReply *reply = net->post(apiRequest(), requestBody(action, std::move(body)));
	answer(reply, std::move(done));
	return reply;
}

QNetworkReply *OvioraServer::postSecret(const QString &action, QJsonObject body, const char *key, QByteArray &secret, Callback done)
{
	// The secret is spliced into the JSON text by hand (it is only letters, digits, '-', '_' and '.', so it
	// needs no escaping): no QJsonObject, document or temporary string ever holds it. The request reads
	// this one buffer in place (QBuffer over it), and the buffer is overwritten when the request is over.
	const QByteArray json = requestBody(action, std::move(body));
	auto payload = std::make_shared<QByteArray>();
	payload->reserve(json.size() + qsizetype(strlen(key)) + secret.size() + 8);
	payload->append(json.constData(), json.size() - 1);  // without the closing '}'
	payload->append(",\"");
	payload->append(key);
	payload->append("\":\"");
	payload->append(secret.constData(), secret.size());
	payload->append("\"}");
	SecretWipe::wipe(secret);
	auto *device = new QBuffer(payload.get());
	device->open(QIODevice::ReadOnly);
	QNetworkReply *reply = net->post(apiRequest(), device);
	// Wiped when the answer is there, and in any case when the reply goes (then the buffer over it goes too)
	connect(reply, &QNetworkReply::finished, reply, [payload]() { SecretWipe::wipe(*payload); });
	connect(reply, &QObject::destroyed, [payload, device]() {
		SecretWipe::wipe(*payload);
		delete device;
	});
	answer(reply, std::move(done));
	return reply;
}

void OvioraServer::answer(QNetworkReply *reply, Callback done)
{
	pin(reply);
	connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
		reply->deleteLater();
		QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
		if(json.value("relogin").toBool() && !token.isEmpty())
			loggedOut(json.value("error").toString());
		if(json.value("update_required").toBool())
			requireUpdate();
		if(json.contains("error"))
		{
			json["http_status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			done(json, json.value("error").toString());
		}
		else if(reply->error() != QNetworkReply::NoError)
			done(json, tr("Sunucuya ulaşılamadı (%1)").arg(reply->errorString()));
		else
			done(json, QString());
	});
}

QJsonObject OvioraServer::callBlocking(const QString &action, const QJsonObject &body, QString *error, int timeout_ms)
{
	// The answer is shared with the request's callback, which may still come after we gave up waiting
	struct Wait
	{
		QSemaphore done;
		QJsonObject reply;
		QString error;
	};
	auto wait = std::make_shared<Wait>();
	QPointer<OvioraServer> self(this);
	QMetaObject::invokeMethod(this, [self, action, body, wait]() {
		if(!self)
		{
			wait->error = QStringLiteral("no server");
			wait->done.release();
			return;
		}
		QNetworkReply *reply = self->post(action, body, [wait](const QJsonObject &r, const QString &e) {
			wait->reply = r;
			wait->error = e;
			wait->done.release();
		});
		reply->setProperty("oviora_relay", true);
	}, Qt::QueuedConnection);
	if(!wait->done.tryAcquire(1, timeout_ms))
	{
		if(error)
			*error = QStringLiteral("timeout");
		return QJsonObject();
	}
	if(error)
		*error = wait->error;
	return wait->reply;
}

// ---------------------------------------------------------------- running game

void OvioraServer::reportConsoleApp(const QString &duid, const QString &app_name, const QString &title_id, const QString &state,
	const QString &console_name)
{
	if(token.isEmpty() || duid.isEmpty())
		return;
	lan_title_ids.insert(duid.toLower(), title_id);
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	AppReport &last = app_reports[duid.toLower()];
	if(now < last.refused_until)
		return;
	const QString sig = app_name + QChar('|') + title_id + QChar('|') + state;
	if(sig == last.sig && now - last.at < 28000)
		return;
	last.sig = sig;
	last.at = now;
	QJsonObject body;
	body["duid"] = duid;
	body["app_name"] = app_name.left(128);
	body["title_id"] = title_id;
	body["state"] = state;
	if(!console_name.isEmpty())
		body["name"] = console_name.left(64);
	const QString key = duid.toLower();
	post("console_app", body, [this, key](const QJsonObject &, const QString &error) {
		if(error.isEmpty())
			return;
		// Not my console (404) or the server is out of reach: try again later, quietly
		AppReport &r = app_reports[key];
		r.sig.clear();
		r.refused_until = QDateTime::currentMSecsSinceEpoch() + 10 * 60 * 1000;
	});
}

QString OvioraServer::runningTitleId(const QString &duid) const
{
	if(duid.isEmpty())
		return QString();
	const QString lan = lan_title_ids.value(duid.toLower());
	if(!lan.isEmpty())
		return lan;
	for(const QString list : { QStringLiteral("available"), QStringLiteral("consoles") })
		for(const QJsonValue &v : account.value(list).toArray())
		{
			const QJsonObject c = v.toObject();
			if(c.value("duid").toString().compare(duid, Qt::CaseInsensitive) == 0)
			{
				const QString t = c.value("app").toObject().value("title_id").toString();
				if(!t.isEmpty())
					return t;
			}
		}
	return QString();
}

// ---------------------------------------------------------------- account

void OvioraServer::loggedIn()
{
	++auth_gen;
	account_status = "active";
	emit changed();
	refresh();
	account_timer.start();
	QTimer::singleShot(8000, this, &OvioraServer::checkUpdate);
	update_timer.start();
}

void OvioraServer::loggedOut(const QString &why)
{
	++auth_gen;
	token.clear();
	account_status = "none";
	account = QJsonObject();
	invalidateAccountCaches();
	setBusy(false);
	invite_info.clear();
	invite_qr.clear();
	history_list.clear();
	history_more = false;
	history_next_before = 0;
	// A page on its way is dropped by its auth_gen check: the flag must not wait for it
	history_loading = false;
	emit inviteChanged();
	emit historyChanged();
	notification_list.clear();
	unread = 0;
	emit notificationsChanged();
	group_list = QJsonArray();
	groups_loaded = false;
	groups_busy = false;
	groups_unsupported = false;
	events_busy = false;
	cache_groups = false;
	emit groupsChanged();
	account_timer.stop();
	update_timer.stop();
	save();
	emit changed();
	emit psnLinksChanged();
	emit consolesChanged();
	emit availableChanged();
	emit friendsChanged();
	emit requestsChanged();
	emit accountChanged();
	if(!why.isEmpty())
		emit message(tr("Oturum kapandı"), why);
}

void OvioraServer::takeLogin(const QJsonObject &r)
{
	token = r.value("token").toString();
	user_name = r.value("user").toString();
	save();
	loggedIn();
}

void OvioraServer::registerUser(const QString &name, const QString &password, const QString &invite)
{
	QJsonObject body { { "user", name.trimmed() }, { "pass", password }, { "token", QString() } };
	const QString code = inviteCode(invite);
	if(!code.isEmpty())
		body["invite"] = code;
	post("register", body, [this](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty())
		{
			emit message(tr("Kayıt olunamadı"), error);
			return;
		}
		takeLogin(r);
		tellInviteResult(r.value("invite").toObject());
	});
}

void OvioraServer::login(const QString &name, const QString &password, const QString &invite)
{
	QJsonObject body { { "user", name.trimmed() }, { "pass", password }, { "token", QString() } };
	const QString code = inviteCode(invite);
	if(!code.isEmpty())
		body["invite"] = code;
	post("login", body, [this](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty())
		{
			emit message(tr("Giriş yapılamadı"), error);
			return;
		}
		takeLogin(r);
		tellInviteResult(r.value("invite").toObject());
	});
}

// ---------------------------------------------------------------- invite

QString OvioraServer::inviteCode(const QString &code_or_link)
{
	const QString text = code_or_link.trimmed();
	if(text.isEmpty())
		return QString();
	// A full link: .../davet/<KOD> or davet.php?k=<KOD>
	static const QRegularExpression link(QStringLiteral("davet(?:[/=]|\\.php\\?k=)([A-Za-z0-9-]{4,16})"));
	const QRegularExpressionMatch m = link.match(text);
	QString code = m.hasMatch() ? m.captured(1) : text;
	code = code.toUpper().remove(QLatin1Char('-')).remove(QLatin1Char(' '));
	static const QRegularExpression valid(QStringLiteral("^[A-Z0-9]{4,16}$"));
	return valid.match(code).hasMatch() ? code : QString();
}

/** The invite link as a QR code: an SVG data URL for an Image (dark modules on white, with a quiet zone) */
static QString qrDataUrl(const QString &text)
{
	if(text.isEmpty())
		return QString();
	try
	{
		const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
		const int border = 4;
		const int size = qr.getSize() + border * 2;
		QString path;
		for(int y = 0; y < qr.getSize(); y++)
			for(int x = 0; x < qr.getSize(); x++)
				if(qr.getModule(x, y))
					path += QStringLiteral("M%1,%2h1v1h-1z").arg(x + border).arg(y + border);
		const QString svg = QStringLiteral(
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %1 %1\" width=\"%2\" height=\"%2\" shape-rendering=\"crispEdges\">"
			"<rect width=\"%1\" height=\"%1\" fill=\"#ffffff\"/><path d=\"%3\" fill=\"#0e0d12\"/></svg>")
			.arg(size).arg(size * 8).arg(path);
		return QStringLiteral("data:image/svg+xml;base64,") + QString::fromLatin1(svg.toUtf8().toBase64());
	}
	catch(const std::exception &)
	{
		return QString();
	}
}

void OvioraServer::takeInvite(const QJsonObject &r)
{
	invite_info = r.toVariantMap();
	// The domain link is the one to share; the IP one only when there is no domain
	const QString url = r.value("url").toString().isEmpty() ? r.value("url_ip").toString() : r.value("url").toString();
	invite_qr = qrDataUrl(url);
	emit inviteChanged();
}

void OvioraServer::loadInvite(bool regenerate)
{
	if(token.isEmpty())
		return;
	QJsonObject body;
	if(regenerate)
		body["regenerate"] = true;
	const quint64 gen = auth_gen;
	post("invite", body, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(tr("Davet bağlantısı alınamadı"), error);
			return;
		}
		takeInvite(r);
	});
}

void OvioraServer::setInviteApprove(bool approve)
{
	if(token.isEmpty())
		return;
	const quint64 gen = auth_gen;
	post("invite", { { "approve", approve } }, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(tr("Davet ayarı değiştirilemedi"), error);
			emit inviteChanged();	// the switch goes back to what is stored
			return;
		}
		takeInvite(r);
	});
}

void OvioraServer::acceptInvite(const QString &code_or_link)
{
	const QString code = inviteCode(code_or_link);
	if(code.isEmpty())
	{
		emit message(tr("Davet kodu"), tr("Bu bir davet kodu ya da bağlantısı değil."));
		return;
	}
	const quint64 gen = auth_gen;
	post("invite_accept", { { "code", code } }, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(tr("Davet kullanılamadı"), error);
			return;
		}
		takeAccount(r);
		tellInviteResult(r.value("invite").toObject());
	});
}

void OvioraServer::checkInvite(const QString &code_or_link)
{
	const QString code = inviteCode(code_or_link);
	if(code.isEmpty())
	{
		emit inviteChecked(QString(), QString(), QString());
		return;
	}
	post("invite_check", { { "code", code }, { "token", QString() } }, [this, code](const QJsonObject &r, const QString &error) {
		emit inviteChecked(code, r.value("from").toString(), error);
	});
}

void OvioraServer::tellInviteResult(const QJsonObject &invite)
{
	if(invite.isEmpty())
		return;
	const QString from = invite.value("from").toString();
	const QString status = invite.value("status").toString();
	if(status == "friends")
		emit toast(tr("Artık arkadaşsınız"), tr("%1 ile arkadaş oldunuz.").arg(from));
	else if(status == "requested")
		emit toast(tr("Arkadaşlık isteği gönderildi"), tr("%1 onaylayınca arkadaş olacaksınız.").arg(from));
	else if(status == "already")
		emit toast(tr("Zaten arkadaşsınız"), tr("%1 ile zaten arkadaştınız.").arg(from));
	else if(status == "self")
		emit message(tr("Davet kodu"), tr("Bu senin kendi davet kodun."));
	else if(status == "invalid")
		emit message(tr("Davet kodu"), tr("Davet kodu geçersiz ya da eskimiş; hesabın yine de hazır."));
}

// ---------------------------------------------------------------- history

void OvioraServer::loadHistory(bool more)
{
	if(token.isEmpty() || history_loading || (more && !history_more))
		return;
	history_loading = true;
	emit historyChanged();
	QJsonObject body { { "limit", 30 } };
	if(more && history_next_before > 0)
		body["before"] = history_next_before;
	const quint64 gen = auth_gen;
	post("history", body, [this, gen, more](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		history_loading = false;
		if(!error.isEmpty())
		{
			emit historyChanged();
			emit message(tr("Oturum geçmişi alınamadı"), error);
			return;
		}
		if(!more)
			history_list.clear();
		history_list.append(r.value("sessions").toArray().toVariantList());
		history_more = r.value("more").toBool();
		history_next_before = (qint64)r.value("next_before").toDouble();
		emit historyChanged();
	});
}

void OvioraServer::logout()
{
	if(!token.isEmpty())
		post("logout", {}, [](const QJsonObject &, const QString &) {});
	loggedOut(QString());
}

void OvioraServer::deleteAccount(const QString &password)
{
	if(!configured() || token.isEmpty())
		return;
	QJsonObject body { { "pass", password } };
	post("account_delete", body, [this](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty())
		{
			emit accountDeleteFailed(error);
			return;
		}
		if(!r.value("deleted").toBool())
		{
			emit accountDeleteFailed(tr("Hesap silinemedi."));
			return;
		}
		// Account is gone on the server; clear local session without sending a logout request.
		user_name.clear();
		loggedOut(QString());
		emit accountDeleted();
	});
}

QString OvioraServer::legalUrl(const QString &page) const
{
	const QUrl base(server_url.startsWith("https://") ? server_url : QStringLiteral("https://oviora.tr/"));
	return base.resolved(QUrl(page)).toString();
}

void OvioraServer::setBusy(bool on)
{
	if(busy == on)
		return;
	busy = on;
	emit busyChanged();
}

void OvioraServer::invalidateAccountCaches()
{
	cache_psn_links = false;
	cache_psn_accounts = false;
	cache_consoles = false;
	cache_available = false;
	cache_friends = false;
	cache_incoming = false;
	cache_outgoing = false;
}

void OvioraServer::takeAccount(const QJsonObject &r)
{
	if(!r.contains("user"))
		return;
	if(r.value("update_required").toBool())
		requireUpdate();
	else if(update_required)
	{
		update_required = false;
		emit updateChanged();
	}
	const QJsonObject prev = account;
	const bool was_loaded = prev.contains("user");
	if(prev != r)
	{
		account = r;
		const bool psn_links_diff = !was_loaded
			|| prev.value("psn_links") != r.value("psn_links")
			|| prev.value("psn_links_max") != r.value("psn_links_max")
			|| prev.value("psn_accounts") != r.value("psn_accounts")
			|| prev.value("psn").toBool() != r.value("psn").toBool()
			|| prev.value("psn_account").toString() != r.value("psn_account").toString()
			|| (!r.contains("psn_links") && prev.value("consoles") != r.value("consoles"));
		const bool consoles_diff = !was_loaded || prev.value("consoles") != r.value("consoles");
		const bool available_diff = !was_loaded || prev.value("available") != r.value("available");
		const bool friends_diff = !was_loaded || prev.value("friends") != r.value("friends");
		const bool requests_diff = !was_loaded
			|| prev.value("incoming") != r.value("incoming")
			|| prev.value("outgoing") != r.value("outgoing");
		if(psn_links_diff) { cache_psn_links = false; cache_psn_accounts = false; emit psnLinksChanged(); }
		if(consoles_diff) { cache_consoles = false; emit consolesChanged(); }
		if(available_diff) { cache_available = false; emit availableChanged(); }
		if(friends_diff) { cache_friends = false; emit friendsChanged(); }
		if(requests_diff) { cache_incoming = false; cache_outgoing = false; emit requestsChanged(); }
		emit accountChanged();
	}
	if(r.contains("warning"))
		emit message(tr("PSN"), r.value("warning").toString());
}

void OvioraServer::requireUpdate()
{
	if(!update_required)
	{
		update_required = true;
		emit updateChanged();
	}
	if(update_state != "checking" && update_state != "downloading" && update_state != "ready")
		checkUpdate();
	else if(update_state == "ready")
		emit updateReady();
}

void OvioraServer::accountCall(const QString &action, QJsonObject body, const QString &fail_title)
{
	const quint64 gen = auth_gen;
	post(action, body, [this, gen, fail_title](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(fail_title, error);
			setBusy(false);
			return;
		}
		setBusy(false);
		takeAccount(r);
	});
}

void OvioraServer::refresh()
{
	if(!configured() || token.isEmpty())
		return;
	const quint64 gen = auth_gen;
	post("me", {}, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen == auth_gen && error.isEmpty())
			takeAccount(r);
	});
	fetchGroups();
	fetchEvents();
}

void OvioraServer::requestPsnLoginUrl()
{
	post("psn_url", {}, [this](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty())
			emit message(tr("PSN hesabı bağlanamadı"), error);
		else
			emit psnLoginUrl(r.value("url").toString());
	});
}

QVariantList OvioraServer::psnAccounts() const
{
	if(!loaded())
		return account.value("psn_accounts").toVariant().toList();
	if(!cache_psn_accounts)
	{
		cached_psn_accounts = account.value("psn_accounts").toVariant().toList();
		cache_psn_accounts = true;
	}
	return cached_psn_accounts;
}

QVariantList OvioraServer::psnLinks() const
{
	if(test_psn_links)
		return {
			QVariantMap{ { "id", -1 }, { "n", 1 }, { "name", "PSN 1" }, { "online_id", "" }, { "account_id", "" },
				{ "primary", true }, { "linked", 1795103023 }, { "consoles", qMax(1, test_consoles - 1) }, { "wide", "ok" } },
			QVariantMap{ { "id", -2 }, { "n", 2 }, { "name", "kardesim_ps" }, { "online_id", "kardesim_ps" }, { "account_id", "" },
				{ "primary", false }, { "linked", 1795104399 }, { "consoles", test_consoles > 1 ? 2 : 1 }, { "wide", "expired" } },
			QVariantMap{ { "id", -3 }, { "n", 3 }, { "name", "kuzen_ps" }, { "online_id", "kuzen_ps" }, { "account_id", "" },
				{ "primary", false }, { "linked", 1795105120 }, { "consoles", 1 }, { "wide", "" } },
		};
	if(!loaded())
		return {};
	if(!cache_psn_links)
	{
		if(account.contains("psn_links"))
			cached_psn_links = account.value("psn_links").toVariant().toList();
		else if(!psnLinked())
			cached_psn_links.clear();
		else
			// A server from before several PSN accounts: its one link, as an entry of the list
			cached_psn_links = { QVariantMap{ { "id", 0 }, { "n", 1 }, { "name", "PSN" }, { "online_id", "" }, { "account_id", psnAccountId() },
				{ "primary", true }, { "linked", 0 }, { "consoles", account.value("consoles").toArray().size() } } };
		cache_psn_links = true;
	}
	return cached_psn_links;
}

int OvioraServer::psnLinksMax() const
{
	if(test_psn_links)
		return 5;
	return account.contains("psn_links_max") ? account.value("psn_links_max").toInt(1) : 1;
}

QVariantList OvioraServer::consoles() const
{
	if(!loaded())
		return withTestPsn(account.value("consoles").toVariant().toList(), false);
	if(!cache_consoles)
	{
		cached_consoles = withTestPsn(account.value("consoles").toVariant().toList(), false);
		cache_consoles = true;
	}
	return cached_consoles;
}

QVariantList OvioraServer::available() const
{
	if(!loaded())
		return withTestPsn(account.value("available").toVariant().toList(), true);
	if(!cache_available)
	{
		cached_available = withTestPsn(account.value("available").toVariant().toList(), true);
		cache_available = true;
	}
	return cached_available;
}

QStringList OvioraServer::incoming() const
{
	if(!loaded())
		return account.value("incoming").toVariant().toStringList();
	if(!cache_incoming)
	{
		cached_incoming = account.value("incoming").toVariant().toStringList();
		cache_incoming = true;
	}
	return cached_incoming;
}

QStringList OvioraServer::outgoing() const
{
	if(!loaded())
		return account.value("outgoing").toVariant().toStringList();
	if(!cache_outgoing)
	{
		cached_outgoing = account.value("outgoing").toVariant().toStringList();
		cache_outgoing = true;
	}
	return cached_outgoing;
}

// --test-psn-links: own PS5s shown as on the made-up accounts (the second one gets the last PS5);
// signed out, made-up PS5s so the home screen's cards can be seen: the first PS5 is on all three
// made-up PSN accounts (three rows with one `ps5` key, one card). Only on screen: connecting to
// them stops at once, nothing is sent to the server.
QVariantList OvioraServer::withTestPsn(QVariantList list, bool own_only) const
{
	if(!test_psn_links)
		return list;
	if(!loaded())
	{
		// Shared with made-up friends (--test-groups) so the friend rows' summaries differ: mehmet the first
		// two, ayse the first one, can every fifth of a long list (--test-consoles)
		auto row = [](const QString &duid, const QString &ps5, const QString &name, const QStringList &shared, int link) {
			static const char *const psn_names[] = { "PSN 1", "kardesim_ps", "kuzen_ps" };
			return QVariantMap{ { "duid", duid }, { "ps5", ps5 }, { "name", name }, { "host", "" }, { "mine", true },
				{ "busy", "" }, { "shared", shared }, { "sid", "" }, { "psn_link", -link }, { "psn_name", psn_names[link - 1] } };
		};
		for(int i = 1; i <= test_consoles; ++i)
		{
			QStringList shared;
			if(i <= 2)
				shared << "mehmet";
			if(i == 1)
				shared << "ayse";
			if(i % 5 == 0)
				shared << "can";
			const QString name = i == 1 ? QStringLiteral("Salon PS5") : i == 2 ? QStringLiteral("Yatak odası PS5") : QStringLiteral("PS5 Yan-%1").arg(i - 2);
			const QString ps5 = QStringLiteral("testps5-%1").arg(i);
			list << row(QStringLiteral("test-%1").arg(i), ps5, name, shared, i == test_consoles && i > 1 ? 2 : 1);
			// The first PS5 on the other two made-up PSN accounts too
			if(i == 1)
			{
				list << row(QStringLiteral("test-1b"), ps5, name, { "mehmet" }, 2);
				list << row(QStringLiteral("test-1c"), ps5, QStringLiteral("Salon PS5 (kuzen)"), {}, 3);
			}
		}
		// --test-groups: rows friends share with me; mehmet's PS5 on two of his PSN accounts (one card, busy:
		// he plays on it, so the queue shows), ayse's on one
		if(own_only && test_groups)
		{
			auto theirs = [](const QString &host, const QString &duid, const QString &ps5, const QString &name, const QString &busy) {
				return QVariantMap{ { "duid", duid }, { "ps5", ps5 }, { "name", name }, { "host", host }, { "mine", false },
					{ "busy", busy }, { "queued", false }, { "queue_pos", 0 }, { "queue_len", 0 } };
			};
			list << theirs("mehmet", "test-f1", "testps5-f1", "Oturma odası", "mehmet");
			list << theirs("ayse", "test-f3", "testps5-f3", "Ayşe PS5", "");
			list << theirs("mehmet", "test-f2", "testps5-f1", "Oturma odası 2", "mehmet");
		}
		return list;
	}
	int own = 0;
	for(const QVariant &v : list)
		own += !own_only || v.toMap().value("mine").toBool();
	int n = 0;
	for(QVariant &v : list)
	{
		QVariantMap c = v.toMap();
		if(own_only && !c.value("mine").toBool())
			continue;
		const bool second = ++n == own && own > 1;
		c["psn_link"] = second ? -2 : -1;
		c["psn_name"] = second ? "kardesim_ps" : "PSN 1";
		v = c;
	}
	return list;
}

void OvioraServer::linkPsn(const QString &redirect)
{
	setBusy(true);
	// The links before, to find the new one when the server does not name it (psn_link_id)
	QList<int> before;
	for(const QVariant &v : psnLinks())
		before << v.toMap().value("id").toInt();
	// Signed in with a PSN account that is linked already: only its login was renewed
	const quint64 gen = auth_gen;
	post("psn_link", { { "redirect", redirect } }, [this, gen, before](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		setBusy(false);
		if(!error.isEmpty())
		{
			emit message(tr("PSN hesabı bağlanamadı"), error);
			emit psnLinkFinished(false, 0);
			return;
		}
		takeAccount(r);
		const bool again = r.value("psn_link_again").toBool();
		if(again)
			emit message(tr("PSN hesabı"), tr("Bu PSN hesabı zaten bağlıydı; girişi yenilendi."));
		// Which account this sign-in was: the server's psn_link_id (also for a renewed one), else the one
		// id that was not there before (a renewed account cannot be told apart that way: 0)
		int link = r.value("psn_link_id").toInt();
		if(link <= 0 && !again)
		{
			QList<int> added;
			for(const QVariant &v : psnLinks())
			{
				const int id = v.toMap().value("id").toInt();
				if(id > 0 && !before.contains(id))
					added << id;
			}
			if(added.size() == 1)
				link = added.first();
		}
		emit psnLinkFinished(true, link > 0 ? link : 0);
	});
}

void OvioraServer::unlinkPsn(int link)
{
	if(test_psn_links && link < 0)
		return;
	QJsonObject body;
	if(link > 0)
		body["psn_link"] = link;
	accountCall("psn_unlink", body, tr("PSN hesabı kaldırılamadı"));
}

void OvioraServer::wideLinkSoftFailed()
{
	emit toast(tr("PSN hesabı"), tr("PSN bağlandı; oyun bilgisi açılamadı, Ayarlar > Hesap'tan tekrar deneyebilirsin."));
}

void OvioraServer::wideLinkPsn(int link, QByteArray &npsso, bool afterLink)
{
	// --test-psn-links' made-up accounts (and an older server's single link, id 0) never reach the server;
	// the value is checked again before it goes anywhere: only what Sony's page can hold
	bool usable = link > 0 && signedIn() && npsso.size() >= 16 && npsso.size() <= 512;
	for(qsizetype i = 0; usable && i < npsso.size(); ++i)
	{
		const char c = npsso.at(i);
		usable = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
	}
	if(!usable)
	{
		SecretWipe::wipe(npsso);
		if(afterLink)
			wideLinkSoftFailed();
		else if(link > 0)
			emit message(tr("Oyun bilgisi açılamadı"), tr("Sony girişi okunamadı. Yeniden dene."));
		return;
	}
	setBusy(true);
	const quint64 gen = auth_gen;
	postSecret("psn_wide_link", { { "psn_link", link } }, "npsso", npsso, [this, gen, afterLink](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		setBusy(false);
		if(!error.isEmpty())
		{
			if(afterLink)
				wideLinkSoftFailed();
			else
				emit message(tr("Oyun bilgisi açılamadı"), error);
			return;
		}
		takeAccount(r);
		emit toast(tr("Oyun bilgisi"), tr("Oyun bilgisi açıldı"));
	});
	SecretWipe::wipe(npsso);  // postSecret wiped it already; never left behind whatever happens above
}

void OvioraServer::wideUnlinkPsn(int link)
{
	if(link <= 0)
		return;
	setBusy(true);
	accountCall("psn_wide_unlink", { { "psn_link", link } }, tr("Oyun bilgisi kapatılamadı"));
}

void OvioraServer::savePsnAccount(const QString &id, const QString &name)
{
	const QString trimmedId = id.trimmed();
	if(!signedIn() || trimmedId.isEmpty())
		return;
	const quint64 gen = auth_gen;
	post("psn_account_save", { { "id", trimmedId }, { "name", name.trimmed().left(64) } }, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen || !error.isEmpty())
			return;
		if(r.contains("user"))
			takeAccount(r);
		else
			refresh();
	});
}

void OvioraServer::removePsnAccount(const QString &id)
{
	const QString trimmedId = id.trimmed();
	if(!signedIn() || trimmedId.isEmpty())
		return;
	const quint64 gen = auth_gen;
	post("psn_account_remove", { { "id", trimmedId } }, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(tr("PSN hesabı silinemedi"), error);
			return;
		}
		if(r.contains("user"))
			takeAccount(r);
		else
			refresh();
	});
}

void OvioraServer::refreshConsoles(int link)
{
	if(test_psn_links && link < 0)
		return;
	setBusy(true);
	QJsonObject body;
	if(link > 0)
		body["psn_link"] = link;
	accountCall("consoles_refresh", body, tr("Konsollar yenilenemedi"));
}

void OvioraServer::renameConsole(const QString &duid, const QString &name)
{
	// --test-psn-links' made-up PS5s are only on screen
	if(duid.isEmpty() || (test_psn_links && duid.startsWith(QLatin1String("test-"))))
		return;
	setBusy(true);
	accountCall("console_rename", { { "duid", duid }, { "name", name.trimmed().left(40) } }, tr("Konsolun adı değiştirilemedi"));
}

void OvioraServer::addFriend(const QString &name)
{
	if(!name.trimmed().isEmpty())
		accountCall("friend_add", { { "name", name.trimmed() } }, tr("Arkadaş eklenemedi"));
}

void OvioraServer::acceptFriend(const QString &name)
{
	accountCall("friend_accept", { { "name", name } }, tr("İstek kabul edilemedi"));
}

void OvioraServer::removeFriend(const QString &name)
{
	accountCall("friend_remove", { { "name", name } }, tr("Arkadaşlık kaldırılamadı"));
}

// ---------------------------------------------------------------- presence and friend groups

// The presence object of a friend or group member; an older server has only the online bool
static QVariantMap presenceOf(const QVariantMap &entry)
{
	QVariantMap p = entry.value("presence").toMap();
	if(p.value("state").toString().isEmpty())
		p = QVariantMap{ { "state", entry.value("online").toBool() ? "online" : "offline" }, { "game", "" }, { "host", "" },
			{ "own", false }, { "since", 0 } };
	return p;
}

// --test-groups: a presence that started / was last seen `ago` seconds before now (-1: since 0)
static QVariantMap testPresence(const char *state, const char *game, const char *host, bool own, qint64 ago)
{
	return { { "state", state }, { "game", game }, { "host", host }, { "own", own },
		{ "since", ago < 0 ? 0 : QDateTime::currentSecsSinceEpoch() - ago } };
}

QVariantList OvioraServer::friends() const
{
	if(test_groups && !loaded())
		return {
			QVariantMap{ { "name", "ayse" }, { "online", true }, { "shares", QStringList{ "Ayşe PS5" } },
				{ "presence", testPresence("playing", "Astro Bot", "mehmet", false, 25 * 60) } },
			QVariantMap{ { "name", "mehmet" }, { "online", true }, { "shares", QStringList{ "Oturma odası", "Oturma odası 2" } },
				{ "presence", testPresence("playing", "GTA VI", "", true, 70 * 60) } },
			QVariantMap{ { "name", "can" }, { "online", true }, { "shares", QStringList() },
				{ "presence", testPresence("online", "", "", false, -1) } },
			QVariantMap{ { "name", "zeynep" }, { "online", false }, { "shares", QStringList() },
				{ "presence", testPresence("offline", "", "", false, 2 * 3600 + 300) } },
			QVariantMap{ { "name", "burak" }, { "online", false }, { "shares", QStringList() },
				{ "presence", testPresence("offline", "", "", false, 3 * 86400) } },
		};
	if(!cache_friends)
	{
		QVariantList list = account.value("friends").toVariant().toList();
		for(QVariant &v : list)
		{
			QVariantMap f = v.toMap();
			f["presence"] = presenceOf(f);
			v = f;
		}
		cached_friends = std::move(list);
		cache_friends = true;
	}
	return cached_friends;
}

QVariantList OvioraServer::groups() const
{
	QVariantList list;
	const bool test_mode = test_groups && !loaded();
	if(!test_mode && cache_groups)
		return cached_groups;
	if(test_mode)
	{
		const QString me = user_name.isEmpty() ? QStringLiteral("deneme") : user_name;
		auto member = [](const QString &name, bool owner, bool is_me, bool is_friend, const QVariantMap &presence) {
			return QVariantMap{ { "name", name }, { "role", owner ? "owner" : "member" }, { "added", 1795103023 }, { "me", is_me },
				{ "friend", is_friend }, { "presence", presence } };
		};
		const QVariantMap mine = testPresence("online", "", "", false, -1);
		list << QVariantMap{ { "id", -1 }, { "name", "Hafta sonu" }, { "owner", me }, { "mine", true }, { "created", 1795103023 },
			{ "members", QVariantList{
				member(me, true, true, false, mine),
				member("ayse", false, false, true, testPresence("playing", "Astro Bot", "mehmet", false, 25 * 60)),
				member("mehmet", false, false, true, testPresence("playing", "GTA VI", "", true, 70 * 60)),
				member("can", false, false, true, testPresence("online", "", "", false, -1)),
				member("umut", false, false, false, testPresence("offline", "", "", false, -1)) } } };
		list << QVariantMap{ { "id", -2 }, { "name", "Gece ekibi" }, { "owner", "mehmet" }, { "mine", false }, { "created", 1795104399 },
			{ "members", QVariantList{
				member("mehmet", true, false, true, testPresence("playing", "GTA VI", "", true, 70 * 60)),
				member(me, false, true, false, mine),
				member("zeynep", false, false, true, testPresence("offline", "", "", false, 2 * 3600 + 300)),
				member("selin", false, false, false, testPresence("online", "", "", false, -1)) } } };
	}
	else
		list = group_list.toVariantList();
	// Members' presence always complete; how many of the others play now, for the list
	for(QVariant &v : list)
	{
		QVariantMap g = v.toMap();
		QVariantList members = g.value("members").toList();
		int playing = 0;
		for(QVariant &m : members)
		{
			QVariantMap member = m.toMap();
			member["presence"] = presenceOf(member);
			if(!member.value("me").toBool() && member.value("presence").toMap().value("state").toString() == "playing")
				playing++;
			m = member;
		}
		g["members"] = members;
		g["playing"] = playing;
		v = g;
	}
	if(!test_mode)
	{
		cached_groups = std::move(list);
		cache_groups = true;
		return cached_groups;
	}
	return list;
}

QVariantMap OvioraServer::group(int id) const
{
	for(const QVariant &v : groups())
		if(v.toMap().value("id").toInt() == id)
			return v.toMap();
	return {};
}

QString OvioraServer::presenceText(const QVariantMap &presence, bool hideNames) const
{
	const QString state = presence.value("state").toString();
	const qint64 since = presence.value("since").toLongLong();
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	if(state == "playing")
	{
		const QString game = presence.value("game").toString().simplified();
		const QString host = presence.value("host").toString().simplified();
		QString line;
		if(presence.value("own").toBool())
			line = game.isEmpty() ? tr("Kendi PS5'inde oynuyor") : tr("Kendi PS5'inde oynuyor: %1").arg(game);
		else
		{
			line = game.isEmpty() ? tr("Oynuyor") : tr("Oynuyor: %1").arg(game);
			if(!host.isEmpty())
			{
				QString where;
				if(hideNames)
					where = tr("Bir arkadaşının PS5'inde");
				else
				{
					// Turkish (the source text): the suffix follows the name's vowels; other languages as translated
					const QString pattern = tr("%1'in PS5'inde");
					where = pattern == QStringLiteral("%1'in PS5'inde")
						? DiscordPresence::turkishGenitive(host) + QStringLiteral(" PS5'inde") : pattern.arg(host);
				}
				line += QStringLiteral(" · ") + where;
			}
		}
		// How long already: "1 sa 20 dk", "45 dk" (the home's session lengths)
		if(since > 0 && now > since)
		{
			const qint64 m = qMax<qint64>(1, (now - since) / 60);
			const qint64 h = m / 60;
			line += QStringLiteral(" · ") + (h > 0 ? (m % 60 ? tr("%1 sa %2 dk").arg(h).arg(m % 60) : tr("%1 sa").arg(h)) : tr("%1 dk").arg(m));
		}
		return line;
	}
	if(state == "online")
		return tr("Çevrimiçi");
	if(since <= 0 || now < since)
		return tr("Çevrimdışı");
	// Last seen: "az önce", "12 dk önce", "2 sa önce", "3 gün önce"
	const qint64 ago = now - since;
	QString when;
	if(ago < 60)
		when = tr("az önce");
	else if(ago < 3600)
		when = tr("%1 dk önce").arg(ago / 60);
	else if(ago < 86400)
		when = tr("%1 sa önce").arg(ago / 3600);
	else
		when = tr("%1 gün önce").arg(ago / 86400);
	return tr("Çevrimdışı") + QStringLiteral(" · ") + when;
}

void OvioraServer::fetchGroups()
{
	if(groups_busy || groups_unsupported || token.isEmpty())
		return;
	groups_busy = true;
	const quint64 gen = auth_gen;
	post("groups", {}, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		groups_busy = false;
		if(!error.isEmpty())
		{
			// A server from before friend groups: stop asking until the next sign-in
			const int status = r.value("http_status").toInt();
			if((status == 400 || status == 404) && !r.value("relogin").toBool())
				groups_unsupported = true;
			return;
		}
		takeGroups(r);
	});
}

void OvioraServer::refreshGroups(std::function<void()> then)
{
	if(!signedIn() || groups_unsupported)
	{
		if(then)
			then();
		return;
	}
	const quint64 gen = auth_gen;
	post("groups", {}, [this, gen, then](const QJsonObject &r, const QString &error) {
		if(gen == auth_gen && error.isEmpty())
			takeGroups(r);
		if(then)
			then();
	});
}

void OvioraServer::takeGroups(const QJsonObject &r)
{
	if(!r.contains("groups"))
		return;
	const QJsonArray next_groups = r.value("groups").toArray();
	const int next_max = r.value("groups_max").toInt(20);
	const int next_members_max = r.value("group_members_max").toInt(20);
	// Every groups view that came counts (MessageCenter waits for one), changed or not
	emit groupsTaken();
	if(groups_loaded && group_list == next_groups && groups_max == next_max && group_members_max == next_members_max)
		return;
	group_list = next_groups;
	groups_max = next_max;
	group_members_max = next_members_max;
	groups_loaded = true;
	cache_groups = false;
	emit groupsChanged();
}

void OvioraServer::groupCall(const QString &action, QJsonObject body, const QString &fail_title)
{
	// The made-up groups of --test-groups (ids below 1) never reach the server
	if(!signedIn() || (body.contains("id") && body.value("id").toInt() <= 0))
		return;
	const quint64 gen = auth_gen;
	post(action, body, [this, gen, action, fail_title](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(fail_title, error);
			fetchGroups();
			return;
		}
		takeGroups(r);
		if(action == "group_create")
			emit groupCreated(r.value("group").toInt());
	});
}

void OvioraServer::groupCreate(const QString &name, const QStringList &members)
{
	QJsonArray list;
	for(const QString &m : members)
		list.append(m);
	if(!name.simplified().isEmpty())
		groupCall("group_create", { { "name", name.simplified() }, { "members", list } }, tr("Grup kurulamadı"));
}

void OvioraServer::groupRename(int id, const QString &name)
{
	if(!name.simplified().isEmpty())
		groupCall("group_rename", { { "id", id }, { "name", name.simplified() } }, tr("Grubun adı değiştirilemedi"));
}

void OvioraServer::groupAdd(int id, const QString &name)
{
	groupCall("group_add", { { "id", id }, { "name", name } }, tr("Gruba eklenemedi"));
}

void OvioraServer::groupRemove(int id, const QString &name)
{
	groupCall("group_remove", { { "id", id }, { "name", name } },
		name.compare(user_name, Qt::CaseInsensitive) == 0 ? tr("Gruptan ayrılınamadı") : tr("Gruptan çıkarılamadı"));
}

void OvioraServer::groupDelete(int id)
{
	groupCall("group_delete", { { "id", id } }, tr("Grup silinemedi"));
}

void OvioraServer::setShare(const QString &duid, const QString &friend_name, bool on)
{
	// --test-psn-links' made-up PS5s are only on screen
	if(test_psn_links && duid.startsWith(QLatin1String("test-")))
		return;
	const quint64 gen = auth_gen;
	post("share", { { "duid", duid }, { "friend", friend_name }, { "on", on } }, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(tr("Paylaşım değiştirilemedi"), error);
			emit consolesChanged();	// the switch in shareConsoles goes back to the server's state
			return;
		}
		takeAccount(r);
	});
}

void OvioraServer::kick(const QString &sid)
{
	accountCall("kick", { { "sid", sid } }, tr("Çıkarılamadı"));
}

// ---------------------------------------------------------------- connection test

#ifdef _WIN32
// The Wi-Fi this PC is connected with: band (from the channel), signal (%), link rate; empty when it is on a cable
static QVariantMap wifiLink()
{
	QVariantMap out;
	HANDLE client = nullptr;
	DWORD version = 0;
	if(WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS)
		return out;
	PWLAN_INTERFACE_INFO_LIST list = nullptr;
	if(WlanEnumInterfaces(client, nullptr, &list) == ERROR_SUCCESS && list)
	{
		for(DWORD i = 0; i < list->dwNumberOfItems && out.isEmpty(); ++i)
		{
			const WLAN_INTERFACE_INFO &info = list->InterfaceInfo[i];
			if(info.isState != wlan_interface_state_connected)
				continue;
			DWORD size = 0;
			PWLAN_CONNECTION_ATTRIBUTES conn = nullptr;
			if(WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size,
					reinterpret_cast<PVOID *>(&conn), nullptr) != ERROR_SUCCESS || !conn)
				continue;
			out["signal"] = int(conn->wlanAssociationAttributes.wlanSignalQuality);
			out["linkMbps"] = int(conn->wlanAssociationAttributes.ulRxRate / 1000);
			WlanFreeMemory(conn);
			ULONG *channel = nullptr;
			if(WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_channel_number, nullptr, &size,
					reinterpret_cast<PVOID *>(&channel), nullptr) == ERROR_SUCCESS && channel)
			{
				// 1-14: 2.4 GHz; 36-177: 5 GHz (6 GHz channels overlap these numbers and read as 5 GHz here)
				out["band"] = *channel <= 14 ? QStringLiteral("2,4 GHz") : QStringLiteral("5 GHz");
				WlanFreeMemory(channel);
			}
		}
		WlanFreeMemory(list);
	}
	WlanCloseHandle(client, nullptr);
	return out;
}
#endif

void OvioraServer::runNetworkTest()
{
	if(network_test.value("state").toString() == "running" || !configured())
		return;
	network_test = { { "state", "running" } };
#ifdef _WIN32
	const QVariantMap wifi = wifiLink();
	network_test["connection"] = wifi.isEmpty() ? "cable" : "wifi";
	for(auto it = wifi.begin(); it != wifi.end(); ++it)
		network_test[it.key()] = it.value();
#else
	network_test["connection"] = "unknown";
#endif
	emit networkTestChanged();
	network_rtts.clear();
	network_lost = 0;
	networkPing(8);
}

void OvioraServer::networkPing(int left)
{
	if(left <= 0)
		return finishNetworkTest();
	auto *timer = new QElapsedTimer;
	timer->start();
	// Any answer of the server is a round trip (the action itself does not exist: it answers at once)
	QNetworkReply *reply = post("net_test", {}, [this, timer, left](const QJsonObject &r, const QString &error) {
		const double ms = timer->nsecsElapsed() / 1e6;
		delete timer;
		// An error answer of the server is a round trip too; no answer within 3 s is lost
		if(r.value("http_status").toInt() > 0 || error.isEmpty())
			network_rtts << ms;
		else
			++network_lost;
		QTimer::singleShot(150, this, [this, left]() { networkPing(left - 1); });
	});
	if(reply)
		QTimer::singleShot(3000, reply, [reply]() { if(reply->isRunning()) reply->abort(); });
}

void OvioraServer::finishNetworkTest()
{
	QVariantMap t = network_test;
	QStringList advice;
	double rtt = 0, jitter = 0;
	if(!network_rtts.isEmpty())
	{
		QList<double> sorted = network_rtts;
		std::sort(sorted.begin(), sorted.end());
		rtt = sorted[sorted.size() / 2];
		for(int i = 1; i < network_rtts.size(); ++i)
			jitter += std::abs(network_rtts[i] - network_rtts[i - 1]);
		if(network_rtts.size() > 1)
			jitter /= network_rtts.size() - 1;
	}
	t["rttMs"] = qRound(rtt);
	t["jitterMs"] = qRound(jitter);
	t["lost"] = network_lost;
	const bool wifi = t.value("connection").toString() == "wifi";
	if(wifi && t.value("band").toString().startsWith("2,4"))
		advice << tr("2,4 GHz Wi-Fi'ye bağlısın; varsa modemin 5 GHz ağına geç, akış belirgin şekilde düzelir.");
	if(wifi && t.contains("signal") && t.value("signal").toInt() < 55)
		advice << tr("Wi-Fi sinyali zayıf (%%1): modeme yaklaş ya da kablo kullan.").arg(t.value("signal").toInt());
	if(network_rtts.isEmpty())
		advice << tr("Oviora sunucusuna ulaşılamadı: internet bağlantın kopmuş ya da sunucu bir süreliğine kapalı olabilir.");
	else
	{
		if(jitter > 15)
			advice << tr("Bağlantı dalgalı (ortalama ±%1 ms): arka planda indirme, güncelleme ya da yayın varsa durdur.").arg(qRound(jitter));
		if(rtt > 120)
			advice << tr("İnternet gecikmesi yüksek (%1 ms); internet üzerinden oynarken 720p ve daha düşük bit hızı daha akıcı olur.").arg(qRound(rtt));
		if(network_lost > 0)
			advice << tr("%1 istek cevapsız kaldı; bağlantıda kopmalar olabilir.").arg(network_lost);
	}
	t["verdict"] = advice.isEmpty() ? tr("Bağlantın oyun için uygun görünüyor.") : tr("Akışı etkileyebilecek şeyler var:");
	t["advice"] = advice;
	t["state"] = "done";
	network_test = t;
	qCInfo(yakamozServer) << "network test" << t.value("connection").toString() << t.value("band").toString() << "signal" << t.value("signal").toInt()
		<< "rtt" << t.value("rttMs").toInt() << "jitter" << t.value("jitterMs").toInt() << "lost" << network_lost;
	emit networkTestChanged();
}

// ---------------------------------------------------------------- Sharing 2.0

void OvioraServer::setShareRules(const QString &duid, const QString &friend_name, int limitMin, bool askFirst, const QVariantList &titles)
{
	if(test_psn_links && duid.startsWith(QLatin1String("test-")))
		return;
	QJsonArray list;
	for(const QVariant &t : titles)
	{
		const QVariantMap m = t.toMap();
		list.append(QJsonObject { { "id", m.value("id").toString() }, { "name", m.value("name").toString() } });
	}
	setBusy(true);
	accountCall("share_rules", { { "duid", duid }, { "friend", friend_name }, { "limit_min", limitMin }, { "ask_first", askFirst }, { "titles", list } },
		tr("Kurallar kaydedilemedi"));
}

void OvioraServer::loadOwnerTitles()
{
	const quint64 gen = auth_gen;
	post("owner_titles", {}, [this, gen](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen || !error.isEmpty())
			return;
		owner_titles = r.value("titles").toArray().toVariantList();
		emit ownerTitlesChanged();
	});
}

void OvioraServer::answerAsk(int index, bool ok, int minutes)
{
	if(index < 0 || index >= pending_asks.size())
		return;
	const QVariantMap a = pending_asks.takeAt(index).toMap();
	emit pendingAsksChanged();
	const bool lease = a.value("kind").toString() == "lease";
	post(lease ? "lease_answer" : "extend_answer",
		lease ? QJsonObject { { "guest", a.value("guest").toString() }, { "duid", a.value("duid").toString() }, { "ok", ok } }
			  : QJsonObject { { "session", a.value("sid").toString() }, { "ok", ok }, { "minutes", minutes } },
		[this](const QJsonObject &, const QString &error) {
			if(!error.isEmpty())
				emit message(tr("Cevap gönderilemedi"), error);
		});
}

void OvioraServer::askMoreTime()
{
	if(session_state != "playing")
		return;
	post("extend_ask", { { "sid", session_id } }, [this](const QJsonObject &, const QString &error) {
		emit message(error.isEmpty() ? tr("Ek süre istendi") : tr("Ek süre istenemedi"),
			error.isEmpty() ? tr("%1 kabul ederse süren uzayacak.").arg(session_host) : error);
	});
}

void OvioraServer::cancelWaiting()
{
	if(session_state == "waiting")
	{
		session_duid.clear();
		setSession("idle");
	}
}

void OvioraServer::takeSessionInfo(const QJsonObject &r)
{
	const int remaining = r.contains("remaining_s") ? r.value("remaining_s").toInt() : -1;
	QStringList games;
	for(const QJsonValue &v : r.value("allowed_games").toArray())
		games << v.toString();
	const QVariantMap warning = r.value("game_warning").toObject().toVariantMap();
	// A few minutes before the end, once each: 10, 5 and 1 minute left
	for(const int mark : { 600, 300, 60 })
		if(remaining > 0 && remaining <= mark && (session_warned == 0 || session_warned > mark))
		{
			session_warned = mark;
			emit message(tr("Süre azalıyor"), tr("%1 dakikan kaldı. Ctrl+O > Ek süre iste ile %2'den süre isteyebilirsin.").arg(mark / 60).arg(session_host));
			break;
		}
	if(remaining != session_remaining || games != session_allowed_games || warning != session_game_warning)
	{
		session_remaining = remaining;
		session_allowed_games = games;
		session_game_warning = warning;
		emit sessionInfoChanged();
	}
}

void OvioraServer::clearSessionInfo()
{
	session_warned = 0;
	if(session_remaining == -1 && session_allowed_games.isEmpty() && session_game_warning.isEmpty())
		return;
	session_remaining = -1;
	session_allowed_games.clear();
	session_game_warning.clear();
	emit sessionInfoChanged();
}

// ---------------------------------------------------------------- sessions

void OvioraServer::setSession(const QString &state)
{
	session_state = state;
	emit changed();
}

void OvioraServer::connectTo(const QString &host, const QString &duid)
{
	if(session_state != "idle" || account_status != "active")
		return;
	// --test-psn-links' made-up PS5s are only on screen
	if(test_psn_links && duid.startsWith(QLatin1String("test-")))
		return;
	session_host = host;
	session_duid = duid;
	setSession("asking");
	lease(false);
}

void OvioraServer::lease(bool retry)
{
	session_id = QString::fromLatin1(QCryptographicHash::hash(QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + QByteArray::number(QRandomGenerator::global()->generate64()), QCryptographicHash::Sha1).toHex().left(16));
	// "relay": this app can let the server make the PSN requests (S2): the owner's token never comes here.
	// The server picks the PSN account the PS5 is on (several can be linked): account_id and token are that one's.
	post("lease", { { "host", session_host }, { "duid", session_duid }, { "sid", session_id }, { "relay", 1 } }, [this, retry](const QJsonObject &r, const QString &error) {
		// The owner says yes each time (Sharing 2.0): wait for it, asking again every 5 s (an answer event also
		// asks at once) for at most 10 minutes; Vazgeç (cancelWaiting) stops it
		if(!error.isEmpty() && r.value("ask_wait").toBool())
		{
			if(session_state != "waiting")
			{
				waiting_since = QDateTime::currentMSecsSinceEpoch();
				setSession("waiting");
				emit message(tr("Onay istendi"), error);
			}
			if(QDateTime::currentMSecsSinceEpoch() - waiting_since > 10 * 60 * 1000)
			{
				setSession("idle");
				emit message(tr("Bağlanılamadı"), tr("%1 isteğine cevap vermedi.").arg(session_host));
				return;
			}
			QTimer::singleShot(5000, this, [this]() {
				if(session_state == "waiting")
					lease(false);
			});
			return;
		}
		if(session_state == "waiting" && !error.isEmpty())
			setSession("asking");
		// 409: the PS5 moved to another of the owner's PSN accounts during the lease; once more
		if(!error.isEmpty() && !retry && r.value("http_status").toInt() == 409)
		{
			lease(true);
			return;
		}
		if(!error.isEmpty() || r.value("mode").toString() != "psn")
		{
			setSession("idle");
			emit message(tr("Bağlanılamadı"), error.isEmpty() ? tr("Sunucu beklenmedik bir cevap verdi.") : error);
			refresh();
			return;
		}
		clearSessionInfo();
		setSession("playing");
		poll_timer.start();
		refresh();
		if(hooks.connect_psn)
			hooks.connect_psn(r.value("token").toString(), r.value("account_id").toString(), r.value("duid").toString(),
				PS5_TARGET, r.value("nickname").toString(), r.value("relay").toBool() ? session_id : QString());
	});
}

void OvioraServer::endSession()
{
	poll_timer.stop();
	clearSessionInfo();
	if(session_state == "playing")
		post("ended", { { "sid", session_id } }, [this](const QJsonObject &, const QString &) { refresh(); });
	session_duid.clear();
	setSession("idle");
}

void OvioraServer::sessionEnded()
{
	if(session_state != "playing")
		return;
	// Connecting over PSN may end a first attempt and start another one right away
	QTimer::singleShot(5000, this, [this]() {
		if(hooks.session_active && hooks.session_active())
			return;
		endSession();
		if(update_state == "ready")
			emit updateReady();  // asked again now that the game is over
	});
}

// ---------------------------------------------------------------- updates

QString OvioraServer::appDir() const
{
	return QCoreApplication::applicationDirPath();
}

QString OvioraServer::stageDir() const
{
	return appDir() + "/_guncelleme";
}

QString OvioraServer::hashFile(const QString &path)
{
	// Hashing ~400 MB at every start would be slow; a file whose size and time did not change keeps its hash
	QFileInfo fi(path);
	if(!fi.exists())
		return QString();
	APP_SETTINGS(s);
	QString key = "yakamoz_hashes/" + QString::fromLatin1(QCryptographicHash::hash(fi.absoluteFilePath().toLower().toUtf8(), QCryptographicHash::Md5).toHex());
	QString stamp = QString::number(fi.size()) + "|" + QString::number(fi.lastModified().toMSecsSinceEpoch());
	QStringList cached = s.value(key).toString().split('#');
	if(cached.size() == 2 && cached[0] == stamp)
		return cached[1];
	QFile f(path);
	if(!f.open(QIODevice::ReadOnly))
		return QString();
	QCryptographicHash h(QCryptographicHash::Sha256);
	h.addData(&f);
	QString hash = QString::fromLatin1(h.result().toHex());
	s.setValue(key, stamp + "#" + hash);
	return hash;
}

// The releases of Oviora on GitHub (GamersSword/oviora) carry only what people download: the zip and the APK.
// The zip holds Oviora/surum-dosyalari.json (version, notes, each file's path, sha256 and size) and its
// signature Oviora/surum-dosyalari.sig, made with this key (its secret half never leaves the publisher's PC).
// Every entry of the zip is deflated on its own: the app reads the zip's central directory and then only
// the entries it lacks, each with an HTTP Range request.
static const char GITHUB_RELEASES[] = "https://api.github.com/repos/GamersSword/oviora/releases/latest";
static const char GITHUB_PUBLISHER[] = "gIPS8/xYzh2GyK9ylmrHXf2oA+sHSCJeLGM1So4LhAM=";
static const char ZIP_LIST[] = "Oviora/surum-dosyalari.json";
static const char ZIP_LIST_SIG[] = "Oviora/surum-dosyalari.sig";
static const char ZIP_ROOT[] = "Oviora/";

static QString githubReleasesUrl()
{
	// Tests point this at a local copy of the API
	const QString test = qEnvironmentVariable("OVIORA_TEST_GITHUB_API");
	return test.isEmpty() ? QString::fromLatin1(GITHUB_RELEASES) : test;
}

namespace {
quint32 le32(const QByteArray &b, qsizetype at) { return quint8(b[at]) | quint8(b[at + 1]) << 8 | quint8(b[at + 2]) << 16 | quint32(quint8(b[at + 3])) << 24; }
quint16 le16(const QByteArray &b, qsizetype at) { return quint16(quint8(b[at]) | quint8(b[at + 1]) << 8); }

/** The central directory of a zip from its tail: [offset, size], or {-1, 0} */
QPair<qint64, qint64> zipDirectory(const QByteArray &tail)
{
	for(qsizetype i = tail.size() - 22; i >= 0; i--)
		if(le32(tail, i) == 0x06054b50u)
			return { qint64(le32(tail, i + 16)), qint64(le32(tail, i + 12)) };
	return { -1, 0 };
}

/** Raw deflate (a zip entry) to exactly size bytes; empty on any error */
QByteArray inflateRaw(const QByteArray &in, qint64 size)
{
	QByteArray out(size, Qt::Uninitialized);
	z_stream s {};
	if(inflateInit2(&s, -MAX_WBITS) != Z_OK)
		return {};
	s.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
	s.avail_in = uInt(in.size());
	s.next_out = reinterpret_cast<Bytef *>(out.data());
	s.avail_out = uInt(out.size());
	const int r = inflate(&s, Z_FINISH);
	const qint64 got = qint64(s.total_out);
	inflateEnd(&s);
	return r == Z_STREAM_END && got == size ? out : QByteArray();
}
}

QHash<QString, OvioraServer::ZipEntry> OvioraServer::zipEntries(const QByteArray &dir)
{
	QHash<QString, ZipEntry> out;
	for(qsizetype i = 0; i + 46 <= dir.size() && le32(dir, i) == 0x02014b50u;)
	{
		const int nlen = le16(dir, i + 28), elen = le16(dir, i + 30), clen = le16(dir, i + 32);
		if(i + 46 + nlen > dir.size())
			break;
		ZipEntry e;
		e.method = le16(dir, i + 10);
		e.csize = le32(dir, i + 20);
		e.size = le32(dir, i + 24);
		e.offset = le32(dir, i + 42);
		out.insert(QString::fromUtf8(dir.mid(i + 46, nlen)), e);
		i += 46 + nlen + elen + clen;
	}
	return out;
}

QByteArray OvioraServer::zipEntryData(const QByteArray &chunk, const ZipEntry &e)
{
	// The chunk starts at the entry's local header: its name and extra field come before the data
	if(chunk.size() < 30 || le32(chunk, 0) != 0x04034b50u)
		return {};
	const qsizetype at = 30 + le16(chunk, 26) + le16(chunk, 28);
	if(at + e.csize > chunk.size())
		return {};
	const QByteArray data = chunk.mid(at, e.csize);
	if(e.method == 0)
		return data.size() == e.size ? data : QByteArray();
	return e.method == 8 ? inflateRaw(data, e.size) : QByteArray();
}

QNetworkReply *OvioraServer::zipRange(qint64 from, qint64 to)
{
	QNetworkRequest req{QUrl(gh_zip_url)};
	req.setRawHeader("User-Agent", "Oviora");
	req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(from) + '-' + QByteArray::number(qMin(to, gh_zip_size - 1)));
	req.setTransferTimeout(60000);
	return net->get(req);
}

bool OvioraServer::verifySignature(const QByteArray &manifest, const QByteArray &signature) const
{
	return verifySignatureWith(publisher_pub, manifest, signature);
}

bool OvioraServer::verifySignatureWith(const QString &publicKey, const QByteArray &manifest, const QByteArray &signature)
{
	QByteArray pub = QByteArray::fromBase64(publicKey.toLatin1());
	if(pub.size() != 32 || signature.size() != 64)
		return false;
	EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(pub.constData()), 32);
	if(!key)
		return false;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
		EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(signature.constData()), 64,
			reinterpret_cast<const unsigned char *>(manifest.constData()), size_t(manifest.size())) == 1;
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(key);
	return ok;
}

void OvioraServer::checkUpdate()
{
	if(!configured() || token.isEmpty() || account_status != "active" || update_state == "downloading" || update_state == "ready")
		return;
	// Testing a build that is not on the site yet: it must not replace itself with the site's one
	if(qEnvironmentVariableIsSet("YAKAMOZ_TEST_GUNCELLEME_YOK") || test_github_update)
		return;
	update_state = "checking";
	update_text = tr("Güncelleme denetleniyor…");
	emit updateChanged();
	githubCheckUpdate();
}

void OvioraServer::githubCheckUpdate()
{
	QNetworkRequest req{QUrl(githubReleasesUrl())};
	req.setRawHeader("User-Agent", "Oviora");
	req.setRawHeader("Accept", "application/vnd.github+json");
	req.setTransferTimeout(30000);
	QNetworkReply *reply = net->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
		gh_zip_url.clear();
		gh_zip_size = 0;
		for(const QJsonValue &v : release.value("assets").toArray())
		{
			const QJsonObject a = v.toObject();
			const QString name = a.value("name").toString();
			if(name.startsWith(QLatin1String("Oviora-")) && name.endsWith(QLatin1String(".zip")))
			{
				gh_zip_url = a.value("browser_download_url").toString();
				gh_zip_size = qint64(a.value("size").toDouble());
			}
		}
		if(reply->error() != QNetworkReply::NoError || gh_zip_url.isEmpty() || gh_zip_size < 22)
		{
			qCWarning(yakamozServer) << "GitHub release not available:" << reply->errorString() << "- asking the server";
			return serverCheckUpdate();
		}
		// The zip's tail (its end record), then its central directory
		QNetworkReply *tail = zipRange(qMax<qint64>(0, gh_zip_size - 66000), gh_zip_size - 1);
		connect(tail, &QNetworkReply::finished, this, [this, tail]() {
			tail->deleteLater();
			const auto [dirAt, dirSize] = zipDirectory(tail->readAll());
			if(tail->error() != QNetworkReply::NoError || dirAt < 0 || dirSize <= 0)
			{
				qCWarning(yakamozServer) << "GitHub zip end not read:" << tail->errorString() << "- asking the server";
				return serverCheckUpdate();
			}
			QNetworkReply *dir = zipRange(dirAt, dirAt + dirSize - 1);
			connect(dir, &QNetworkReply::finished, this, [this, dir]() {
				dir->deleteLater();
				gh_entries = zipEntries(dir->readAll());
				const ZipEntry list = gh_entries.value(QLatin1String(ZIP_LIST)), sig = gh_entries.value(QLatin1String(ZIP_LIST_SIG));
				if(dir->error() != QNetworkReply::NoError || list.csize <= 0 || sig.csize <= 0)
				{
					qCWarning(yakamozServer) << "GitHub zip without a signed list:" << dir->errorString() << "- asking the server";
					return serverCheckUpdate();
				}
				QNetworkReply *a = zipRange(list.offset, list.offset + 30 + 1024 + list.csize);
				QNetworkReply *b = zipRange(sig.offset, sig.offset + 30 + 1024 + sig.csize);
				auto pending = std::make_shared<int>(2);
				auto done = [this, a, b, list, sig, pending]() {
					if(--*pending)
						return;
					a->deleteLater();
					b->deleteLater();
					const QByteArray manifest = zipEntryData(a->readAll(), list);
					const QByteArray signature = QByteArray::fromBase64(zipEntryData(b->readAll(), sig).trimmed());
					if(manifest.isEmpty())
					{
						qCWarning(yakamozServer) << "GitHub release list not read - asking the server";
						return serverCheckUpdate();
					}
					if(!verifySignatureWith(QString::fromLatin1(GITHUB_PUBLISHER), manifest, signature))
					{
						// Not signed by the publisher: whoever changed the release, nothing of it is installed
						update_state = "error";
						update_text = tr("Güncellemenin imzası doğrulanamadı, kurulmadı.");
						emit updateChanged();
						qCWarning(yakamozServer) << "GitHub update signature rejected";
						return;
					}
					const QJsonObject m = QJsonDocument::fromJson(manifest).object();
					qCInfo(yakamozServer) << "GitHub release" << m.value("version").toString();
					takeManifest(manifest, m.value("version").toString(), m.value("notes").toString(), gh_zip_url);
				};
				connect(a, &QNetworkReply::finished, this, done);
				connect(b, &QNetworkReply::finished, this, done);
			});
		});
	});
}

void OvioraServer::serverCheckUpdate()
{
	post("update", { { "version", installedVersion() } }, [this](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty() || r.value("none").toBool())
		{
			update_state = error.isEmpty() ? "none" : "error";
			update_text = error.isEmpty() ? tr("Henüz yayınlanmış bir güncelleme yok.") : error;
			emit updateChanged();
			return;
		}
		QByteArray manifest = r.value("manifest").toString().toUtf8();
		if(!verifySignature(manifest, QByteArray::fromBase64(r.value("sig").toString().toLatin1())))
		{
			// Signed with another key than the one in the connection code: not from the owner
			update_state = "error";
			update_text = tr("Güncellemenin imzası doğrulanamadı, kurulmadı.");
			emit updateChanged();
			qCWarning(yakamozServer) << "Update signature rejected";
			return;
		}
		takeManifest(manifest, r.value("version").toString(), r.value("notes").toString(), QString());
	});
}

void OvioraServer::takeManifest(const QByteArray &manifest, const QString &version, const QString &notes, const QString &packUrl)
{
	update_version = version;
	update_notes = notes;
#ifndef OVIORA_PLATFORM_UPDATES
	// Development build: never download or install the published release
	if(!test_github_update)
	{
		dev_server_version = update_version;
		update_state = "none";
		update_text = tr("Sunucudaki sürüm: %1 (bu derlemede güncelleme kapalı).").arg(update_version);
		if(update_required)
		{
			update_required = false;
			refresh();
		}
		emit updateChanged();
		return;
	}
#endif
	gh_pack_url = packUrl;
	QList<UpdateFile> files;
	for(const QJsonValue &v : QJsonDocument::fromJson(manifest).object().value("files").toArray())
	{
		QJsonObject f = v.toObject();
		QString path = f.value("path").toString();
		// Never outside the program folder
		if(path.isEmpty() || path.contains("..") || path.contains('\\') || path.contains(':') || QDir::isAbsolutePath(path))
			continue;
		UpdateFile u { path, f.value("sha256").toString(), qint64(f.value("size").toDouble()) };
		// From the GitHub zip: where the file's entry is in it
		if(!packUrl.isEmpty())
		{
			const auto it = gh_entries.constFind(QLatin1String(ZIP_ROOT) + path);
			if(it == gh_entries.constEnd() || it->size != u.size)
				continue;	// not in the zip (or not the listed size): never fetched from the wrong place
			u.offset = it->offset;
			u.csize = it->csize;
			u.method = it->method;
		}
		files.append(u);
	}
	// Which files differ from what is installed (hashing runs off the UI thread)
	auto watcher = new QFutureWatcher<QList<UpdateFile>>(this);
	connect(watcher, &QFutureWatcher<QList<UpdateFile>>::finished, this, [this, watcher, files]() {
		watcher->deleteLater();
		update_files = files;
		update_queue = watcher->result();
		qCInfo(yakamozServer) << "Update" << update_version << "files" << files.size() << "to download" << update_queue.size();
		update_bytes_total = 0;
		update_bytes_done = 0;
		for(const UpdateFile &f : update_queue)
			update_bytes_total += f.size;
		if(update_queue.isEmpty())
		{
			bool staged = QDir(stageDir()).exists() && !QDir(stageDir()).isEmpty();
			if(staged)
			{
				update_state = "ready";
				update_text = tr("Sürüm %1 hazır, yeniden başlatınca kurulur.").arg(update_version);
				emit updateChanged();
				emit updateReady();
				return;
			}
			APP_SETTINGS(s);
			s.setValue(QString(GROUP) + "/installed_version", update_version);
			update_state = "none";
			update_text = tr("Oviora güncel (sürüm %1).").arg(update_version);
			emit updateChanged();
			// The files already were the current ones: the server knows it from the next request
			if(update_required)
				refresh();
			return;
		}
		update_state = "downloading";
		emit updateChanged();
		downloadNext();
	});
	QString app = appDir(), stage = stageDir();
	watcher->setFuture(QtConcurrent::run([this, files, app, stage]() {
		QList<UpdateFile> todo;
		for(const UpdateFile &f : files)
		{
			// The starter for copies with the old program; this one needs nothing of it
			if(f.path.compare(OLD_APP_EXE, Qt::CaseInsensitive) == 0 && QFileInfo::exists(app + "/" + APP_EXE))
				continue;
			// Optional components come only to those who installed them (Ayarlar -> Goruntu)
			const QString component = componentOf(f.path);
			if(!component.isEmpty() && !componentInstalled(component))
				continue;
			if(hashFile(app + "/" + f.path) == f.sha256)
				continue;
			if(hashFile(stage + "/" + f.path) == f.sha256)
				continue;  // downloaded earlier, waiting for the restart
			todo.append(f);
		}
		return todo;
	}));
}

void OvioraServer::downloadNext()
{
	if(update_queue.isEmpty())
	{
		update_state = "ready";
		update_progress = 1;
		update_text = tr("Sürüm %1 indirildi. Yeniden başlatınca kurulur.").arg(update_version);
		emit updateChanged();
		emit updateReady();
		return;
	}
	UpdateFile f = update_queue.first();
	QString target = stageDir() + "/" + f.path;
	QDir().mkpath(QFileInfo(target).absolutePath());
	delete download_file;
	download_file = new QFile(target + ".part");
	if(!download_file->open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		update_state = "error";
		update_text = tr("Güncelleme dosyası yazılamadı: %1").arg(f.path);
		emit updateChanged();
		return;
	}
	if(f.offset >= 0 && f.csize > 0 && !gh_pack_url.isEmpty())
	{
		// The file's entry of the GitHub zip (its local header and data), unpacked and its hash checked
		download = zipRange(f.offset, f.offset + 30 + 1024 + f.csize);
		auto chunk = std::make_shared<QByteArray>();
		const qint64 doneBefore = update_bytes_done;
		const qint64 want = 30 + 1024 + f.csize + 1;
		connect(download, &QNetworkReply::readyRead, this, [this, chunk, f, doneBefore, want]() {
			chunk->append(download->readAll());
			if(chunk->size() > want)
				return download->abort();  // a server ignoring Range sends the whole zip
			update_bytes_done = doneBefore + f.size * qMin<qint64>(chunk->size(), f.csize) / std::max<qint64>(1, f.csize);
			update_progress = update_bytes_total ? double(update_bytes_done) / double(update_bytes_total) : 0;
			update_text = tr("Güncelleme indiriliyor: %1 / %2 MB").arg(update_bytes_done / 1048576).arg(update_bytes_total / 1048576);
			emit updateChanged();
		});
		connect(download, &QNetworkReply::finished, this, [this, chunk, f, target, doneBefore]() {
			download->deleteLater();
			chunk->append(download->readAll());
			const int status = download->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			ZipEntry e;
			e.offset = f.offset;
			e.csize = f.csize;
			e.size = f.size;
			e.method = f.method;
			QByteArray data = download->error() == QNetworkReply::NoError && status == 206 ? zipEntryData(*chunk, e) : QByteArray();
			chunk->clear();
			if(data.size() != f.size || QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) != f.sha256 ||
				download_file->write(data) != data.size() || !download_file->flush())
			{
				qCWarning(yakamozServer) << "Update entry failed:" << f.path << "http" << status << download->errorString();
				download_file->close();
				download_file->remove();
				update_bytes_done = doneBefore;
				update_state = "error";
				update_text = tr("Güncelleme indirilemedi (%1), sonra tekrar denenecek.").arg(f.path);
				emit updateChanged();
				return;
			}
			download_file->close();
			update_bytes_done = doneBefore + f.size;
			QFile::remove(target);
			download_file->rename(target);
			update_queue.removeFirst();
			downloadNext();
		});
		return;
	}
	QUrl url(server_url);
	QUrlQuery q;
	q.addQueryItem("a", "file");
	q.addQueryItem("h", f.sha256);
	url.setQuery(q);
	QNetworkRequest req(url);
	req.setRawHeader("X-Yakamoz-Token", token.toUtf8());
	req.setTransferTimeout(60000);
	download = net->get(req);
	pin(download);
	connect(download, &QNetworkReply::readyRead, this, [this]() {
		QByteArray chunk = download->readAll();
		download_file->write(chunk);
		update_bytes_done += chunk.size();
		update_progress = update_bytes_total ? double(update_bytes_done) / double(update_bytes_total) : 0;
		update_text = tr("Güncelleme indiriliyor: %1 / %2 MB").arg(update_bytes_done / 1048576).arg(update_bytes_total / 1048576);
		emit updateChanged();
	});
	connect(download, &QNetworkReply::finished, this, [this, f, target]() {
		download->deleteLater();
		download_file->write(download->readAll());
		download_file->close();
		bool ok = download->error() == QNetworkReply::NoError;
		QCryptographicHash h(QCryptographicHash::Sha256);
		if(ok && download_file->open(QIODevice::ReadOnly))
		{
			h.addData(download_file);
			download_file->close();
		}
		if(!ok || QString::fromLatin1(h.result().toHex()) != f.sha256)
		{
			download_file->remove();
			update_state = "error";
			update_text = tr("Güncelleme indirilemedi (%1), sonra tekrar denenecek.").arg(f.path);
			emit updateChanged();
			return;
		}
		QFile::remove(target);
		download_file->rename(target);
		update_queue.removeFirst();
		downloadNext();
	});
}

void OvioraServer::applyUpdate()
{
#ifndef OVIORA_PLATFORM_UPDATES
	return;
#endif
	if(update_state != "ready")
		return;
	if(hooks.session_active && hooks.session_active())
	{
		emit message(tr("Güncelleme"), tr("Oyun bitince kurulacak."));
		return;
	}
	// Running files cannot be overwritten on Windows but can be renamed: the old ones move aside
	// and are deleted at the next start
	QString app = appDir(), stage = stageDir();
	QDirIterator it(stage, QDir::Files, QDirIterator::Subdirectories);
	int moved = 0;
	while(it.hasNext())
	{
		QString staged = it.next();
		QString rel = QDir(stage).relativeFilePath(staged);
		if(rel.endsWith(".part"))
			continue;
		QString target = app + "/" + rel;
		QDir().mkpath(QFileInfo(target).absolutePath());
		if(QFileInfo::exists(target))
		{
			QFile::remove(target + OLD_SUFFIX);
			if(!QFile::rename(target, target + OLD_SUFFIX))
			{
				qCWarning(yakamozServer) << "Could not move aside" << target;
				continue;
			}
		}
		if(QFile::rename(staged, target))
			moved++;
	}
	QDir(stage).removeRecursively();
	APP_SETTINGS(s);
	s.setValue(QString(GROUP) + "/installed_version", update_version);
	qCInfo(yakamozServer) << "Update" << update_version << "installed," << moved << "files, restarting";
	// An update may bring the program under its new name: that one is started then
	QString restart = QCoreApplication::applicationFilePath();
	if(QFileInfo::exists(app + "/" + APP_EXE))
		restart = app + "/" + APP_EXE;
	QProcess::startDetached(restart, QStringList(), app);
	QCoreApplication::quit();
}

void OvioraServer::cleanupOldFiles()
{
	QDirIterator it(appDir(), { QString("*") + OLD_SUFFIX }, QDir::Files, QDirIterator::Subdirectories);
	while(it.hasNext())
		QFile::remove(it.next());
}

void OvioraServer::takeOverOldName()
{
#if defined(Q_OS_WIN) && defined(OVIORA_PLATFORM_UPDATES)
	// Only the shipped program does this, never a build run from elsewhere
	const QString app = QDir::cleanPath(appDir());
	if(QFileInfo(QCoreApplication::applicationFilePath()).fileName().compare(APP_EXE, Qt::CaseInsensitive) != 0)
		return;
	const QString exe = QDir::toNativeSeparators(app + "/" + APP_EXE);
	const QString native_app = QDir::toNativeSeparators(app);
	auto in_app = [&native_app](const QString &path) {
		QString p = QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(path)));
		return p.startsWith(native_app + "\\", Qt::CaseInsensitive);
	};

	// 1. Shortcuts to the old program: desktop and start menu ones become "Oviora", pinned ones are
	// pointed at Oviora.exe where they are (a pin is its file)
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // already done by Qt on this thread; harmless
	struct Place { QString dir; bool rename; };
	QList<Place> places;
	for(int folder : { CSIDL_DESKTOPDIRECTORY, CSIDL_PROGRAMS, CSIDL_COMMON_DESKTOPDIRECTORY })
	{
		wchar_t base[MAX_PATH];
		if(SUCCEEDED(SHGetFolderPathW(nullptr, folder, nullptr, 0, base)))
			places.append({ QString::fromWCharArray(base), folder != CSIDL_COMMON_DESKTOPDIRECTORY });
	}
	wchar_t roaming[MAX_PATH];
	if(SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roaming)))
		places.append({ QString::fromWCharArray(roaming) + "\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar", false });
	for(const Place &place : places)
	{
		QDir dir(place.dir);
		for(const QString &name : dir.entryList({ "*.lnk" }, QDir::Files))
		{
			const QString lnk = QDir::toNativeSeparators(dir.filePath(name));
			IShellLinkW *link = nullptr;
			if(FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void **>(&link))))
				continue;
			IPersistFile *file = nullptr;
			if(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file))))
			{
				wchar_t target[MAX_PATH] = {};
				if(SUCCEEDED(file->Load(reinterpret_cast<const wchar_t *>(lnk.utf16()), STGM_READ))
					&& SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, 0)))
				{
					QString t = QString::fromWCharArray(target);
					if(in_app(t) && QFileInfo(t).fileName().compare(OLD_APP_EXE, Qt::CaseInsensitive) == 0)
					{
						link->SetPath(reinterpret_cast<const wchar_t *>(exe.utf16()));
						link->SetWorkingDirectory(reinterpret_cast<const wchar_t *>(native_app.utf16()));
						link->SetDescription(L"Oviora");
						link->SetIconLocation(reinterpret_cast<const wchar_t *>(exe.utf16()), 0);
						QString save_to = lnk;
						bool rename = place.rename && name.compare("Yakamoz.lnk", Qt::CaseInsensitive) == 0;
						if(rename)
							save_to = QDir::toNativeSeparators(dir.filePath("Oviora.lnk"));
						if(SUCCEEDED(file->Save(reinterpret_cast<const wchar_t *>(save_to.utf16()), TRUE)))
						{
							qCInfo(yakamozServer) << "Shortcut now opens Oviora:" << save_to;
							if(rename)
								QFile::remove(lnk);
						}
					}
				}
				file->Release();
			}
			link->Release();
		}
	}
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

	// 2. The entry in Windows' app list: "Oviora", published by Arfep, opening this exe
	const wchar_t *uninstall = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
	for(const wchar_t *old_name : { L"Yakamoz", L"Oviora" })
	{
		std::wstring old_key = std::wstring(uninstall) + old_name;
		HKEY old_handle;
		if(RegOpenKeyExW(HKEY_CURRENT_USER, old_key.c_str(), 0, KEY_READ, &old_handle) != ERROR_SUCCESS)
			continue;
		wchar_t location[MAX_PATH * 2] = {};
		DWORD size = sizeof(location) - sizeof(wchar_t);
		bool ours = RegQueryValueExW(old_handle, L"InstallLocation", nullptr, nullptr, reinterpret_cast<BYTE *>(location), &size) == ERROR_SUCCESS
			&& QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(location))).compare(app, Qt::CaseInsensitive) == 0;
		if(!ours)
		{
			RegCloseKey(old_handle);
			continue;
		}
		HKEY handle;
		std::wstring new_key = std::wstring(uninstall) + L"Oviora";
		bool same = wcscmp(old_name, L"Oviora") == 0;
		if(RegCreateKeyExW(HKEY_CURRENT_USER, new_key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &handle, nullptr) == ERROR_SUCCESS)
		{
			bool copied = same || RegCopyTreeW(old_handle, nullptr, handle) == ERROR_SUCCESS;
			auto set = [handle](const wchar_t *name, const QString &value) {
				RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.utf16()), DWORD((value.size() + 1) * sizeof(wchar_t)));
			};
			set(L"DisplayName", "Oviora");
			set(L"Publisher", "Arfep");
			set(L"DisplayIcon", exe);
			if(!installedVersion().isEmpty())
				set(L"DisplayVersion", installedVersion());
			RegCloseKey(handle);
			RegCloseKey(old_handle);
			if(copied && !same)
			{
				RegDeleteTreeW(HKEY_CURRENT_USER, old_key.c_str());
				qCInfo(yakamozServer) << "App list entry renamed to Oviora";
			}
		}
		else
			RegCloseKey(old_handle);
	}

	// 3. The old program file, once nothing runs it (it cannot be deleted while it runs: next start then)
	const QString old_exe = app + "/" + OLD_APP_EXE;
	if(QFileInfo::exists(old_exe) && QFileInfo::exists(exe))
	{
		if(QFile::remove(old_exe))
			qCInfo(yakamozServer) << "Removed the old" << OLD_APP_EXE;
		else
			qCInfo(yakamozServer) << "The old" << OLD_APP_EXE << "is still in use, removed at a later start";
	}
#endif
}

// ---------------------------------------------------------------- events and notifications

static QString eventField(const QJsonObject &e, const char *key)
{
	if(e.contains(key))
		return e.value(key).toVariant().toString();
	return e.value("data").toObject().value(key).toVariant().toString();
}

bool OvioraServer::notifyEnabled(const QString &type) const
{
	APP_SETTINGS(s);
	return s.value(QString(GROUP) + "/notify_" + type, true).toBool();
}

void OvioraServer::setNotifyEnabled(const QString &type, bool on)
{
	APP_SETTINGS(s);
	s.setValue(QString(GROUP) + "/notify_" + type, on);
}

void OvioraServer::markNotificationsRead()
{
	if(!unread)
		return;
	unread = 0;
	emit notificationsChanged();
}

void OvioraServer::fetchEvents()
{
	if(events_busy || events_unsupported || token.isEmpty())
		return;
	// First look of this run: the last week for the list; only what came after the last run is announced
	const bool first = events_last < 0;
	qint64 announce_after = events_last;
	if(first)
	{
		APP_SETTINGS(s);
		announce_after = s.value(QString(GROUP) + "/events_last", -1).toLongLong();
	}
	const qint64 since = first ? QDateTime::currentMSecsSinceEpoch() - 7LL * 24 * 3600 * 1000 : events_last;
	events_busy = true;
	const quint64 gen = auth_gen;
	post("events", { { "since", double(since) } }, [this, gen, announce_after](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		events_busy = false;
		if(!error.isEmpty())
		{
			// An older server without this action: stop asking until the next start
			if(r.contains("error") && !r.contains("events") && !r.value("relogin").toBool())
				events_unsupported = true;
			return;
		}
		const QJsonArray events = r.value("events").toArray();
		for(const QJsonValue &v : events)
		{
			const QJsonObject e = v.toObject();
			// Nothing seen before on this PC: listed, not announced
			takeEvent(e, announce_after >= 0 && qint64(e.value("id").toDouble()) > announce_after);
		}
		// The settings file is written on the UI thread: only when the newest event changed
		if(r.contains("last") && qint64(r.value("last").toDouble()) != events_last)
		{
			events_last = qint64(r.value("last").toDouble());
			APP_SETTINGS(s);
			s.setValue(QString(GROUP) + "/events_last", events_last);
		}
	});
}

void OvioraServer::takeEvent(const QJsonObject &e, bool announce)
{
	const QString type = e.value("type").toString();
	QString title, text;
	if(type == "friend_request")
	{
		title = tr("Arkadaşlık isteği");
		text = tr("%1 seni arkadaş olarak eklemek istiyor.").arg(eventField(e, "from"));
	}
	else if(type == "friend_accepted")
	{
		title = tr("Arkadaşlık kabul edildi");
		text = tr("%1 isteğini kabul etti.").arg(eventField(e, "by"));
	}
	else if(type == "share_granted")
	{
		title = tr("Yeni PS5 paylaşımı");
		text = tr("%1, %2 adlı PS5'ini seninle paylaştı.").arg(eventField(e, "owner"), eventField(e, "console"));
	}
	else if(type == "session_started")
	{
		title = tr("PS5'inde oturum başladı");
		text = tr("%1, %2 adlı PS5'inde oynamaya başladı.").arg(eventField(e, "guest"), eventField(e, "console"));
	}
	else if(type == "console_free")
	{
		title = tr("PS5 boşaldı");
		text = tr("%1 (%2) artık boşta, bağlanabilirsin.").arg(eventField(e, "console"), eventField(e, "owner"));
	}
	else if(type == "group_added")
	{
		title = tr("Yeni grup");
		text = tr("%1 seni %2 grubuna ekledi.").arg(eventField(e, "by"), eventField(e, "group_name"));
		// The new group in the list (and its members' keys for messages) at once
		fetchGroups();
	}
	else if(type == "lease_ask" || type == "extend_ask")
	{
		const bool lease = type == "lease_ask";
		title = lease ? tr("Bağlanma isteği") : tr("Ek süre isteği");
		text = lease ? tr("%1, %2 adlı PS5'ine bağlanmak istiyor.").arg(eventField(e, "guest"), eventField(e, "console"))
					 : tr("%1, %2 adlı PS5'inde ek süre istiyor.").arg(eventField(e, "guest"), eventField(e, "console"));
		// Asks of the last 10 minutes wait for an answer in the window (older ones the server forgot)
		qint64 t = qint64(e.value("t").toDouble());
		if(t > 0 && t < 100000000000LL)
			t *= 1000;
		if(t <= 0 || QDateTime::currentMSecsSinceEpoch() - t < 10 * 60 * 1000)
		{
			const QString key = type + "/" + eventField(e, "guest") + "/" + eventField(e, lease ? "duid" : "sid");
			bool known = false;
			for(const QVariant &v : pending_asks)
				known = known || v.toMap().value("key").toString() == key;
			if(!known)
			{
				pending_asks.append(QVariantMap { { "key", key }, { "kind", lease ? "lease" : "extend" }, { "guest", eventField(e, "guest") },
					{ "console", eventField(e, "console") }, { "duid", eventField(e, "duid") }, { "sid", eventField(e, "sid") } });
				emit pendingAsksChanged();
			}
		}
	}
	else if(type == "lease_answer")
	{
		const bool ok = eventField(e, "ok") == "true" || eventField(e, "ok") == "1";
		title = ok ? tr("İzin verildi") : tr("İzin verilmedi");
		text = ok ? tr("%1 bağlanmana izin verdi.").arg(eventField(e, "owner")) : tr("%1 bu sefer izin vermedi.").arg(eventField(e, "owner"));
		// Waiting for it: ask again now rather than in up to 5 s
		if(session_state == "waiting")
			lease(false);
	}
	else if(type == "extend_answer")
	{
		const bool ok = eventField(e, "ok") == "true" || eventField(e, "ok") == "1";
		title = ok ? tr("Ek süre verildi") : tr("Ek süre verilmedi");
		text = ok ? tr("%1 sana %2 dakika daha verdi.").arg(eventField(e, "owner"), eventField(e, "minutes")) : tr("%1 ek süre vermedi.").arg(eventField(e, "owner"));
		session_warned = 0;
	}
	else if(type == "session_rule")
	{
		title = tr("Oturum kapandı");
		text = eventField(e, "reason") == "game" ? tr("%1, izin vermediğin bir oyunu açtığı için %2 oturumu kapandı.").arg(eventField(e, "guest"), eventField(e, "console"))
												 : tr("%1 için verdiğin süre doldu; %2 oturumu kapandı.").arg(eventField(e, "guest"), eventField(e, "console"));
	}
	else if(type == "kicked")
	{
		title = tr("Oturumdan çıkarıldın");
		text = tr("%1 seni %2 adlı PS5'ten çıkardı.").arg(eventField(e, "by"), eventField(e, "console"));
	}
	else
		return;
	if(!e.value("text").toString().isEmpty())
		text = e.value("text").toString();
	qint64 at = qint64(e.value("t").toDouble());
	if(!at)
		at = qint64(e.value("time").toDouble());
	if(!at)
		at = qint64(e.value("at").toDouble());
	if(at > 0 && at < 100000000000LL)
		at *= 1000;  // seconds
	if(at <= 0)
		at = QDateTime::currentMSecsSinceEpoch();
	QVariantMap item {
		{ "type", type }, { "title", title }, { "text", text },
		{ "time", QDateTime::fromMSecsSinceEpoch(at).toString("dd.MM HH:mm") },
	};
	notification_list.prepend(item);
	while(notification_list.size() > 30)
		notification_list.removeLast();
	if(announce)
		unread++;
	emit notificationsChanged();
	if(!announce || !notifyEnabled(type))
		return;
	qCInfo(yakamozServer) << "event:" << type;
	// In front: a toast inside the window; otherwise a Windows notification from the tray icon
	// (testing off-screen: YAKAMOZ_TEST_TOAST_ICERDE keeps it inside, no Windows notification on the desktop)
	if(QGuiApplication::applicationState() == Qt::ApplicationActive || !QSystemTrayIcon::isSystemTrayAvailable()
		|| qEnvironmentVariableIsSet("YAKAMOZ_TEST_TOAST_ICERDE"))
	{
		emit toast(title, text);
		return;
	}
	auto icon = qobject_cast<QSystemTrayIcon *>(tray);
	if(!icon)
	{
		icon = new QSystemTrayIcon(this);
		QIcon app_icon = QGuiApplication::windowIcon();
		icon->setIcon(app_icon);
		icon->setToolTip("Oviora");
		connect(icon, &QSystemTrayIcon::messageClicked, this, []() {
			for(QWindow *w : QGuiApplication::topLevelWindows())
				if(w->isVisible())
				{
					w->showNormal();
					w->requestActivate();
					break;
				}
		});
		tray = icon;
	}
	icon->show();
	icon->showMessage(title, text, QSystemTrayIcon::Information, 8000);
}

void OvioraServer::setQueue(const QString &duid, bool on)
{
	// --test-psn-links' made-up PS5s are only on screen
	if(test_psn_links && duid.startsWith(QLatin1String("test-")))
		return;
	const quint64 gen = auth_gen;
	post("queue", { { "duid", duid }, { "on", on } }, [this, gen, on](const QJsonObject &r, const QString &error) {
		if(gen != auth_gen)
			return;
		if(!error.isEmpty())
		{
			emit message(on ? tr("Sıraya girilemedi") : tr("Sıradan çıkılamadı"), error);
			return;
		}
		if(on)
			qCInfo(yakamozServer) << "queue position" << r.value("position").toInt() << "of" << r.value("waiting").toInt();
		refresh();
	});
}

// ---------------------------------------------------------------- problem reports

static QStringList recentSessionLogs(int count)
{
	QDir dir(QString::fromStdWString(oviora::runtime::logsDirectory().wstring()));
	QFileInfoList files = dir.entryInfoList({ "*.log" }, QDir::Files, QDir::Time);
	QStringList out;
	for(int i = 0; i < files.size() && out.size() < count; i++)
		if(files[i].size() > 0)
			out << files[i].absoluteFilePath();
	return out;
}

QVariantList OvioraServer::reportFiles() const
{
	QVariantList out;
	for(const QString &path : recentSessionLogs(3))
	{
		QFileInfo fi(path);
		out << QVariantMap { { "name", fi.fileName() }, { "size", int((fi.size() + 1023) / 1024) },
			{ "time", fi.lastModified().toString("dd.MM.yyyy HH:mm") } };
	}
	return out;
}

static quint32 crc32Of(const QByteArray &in)
{
	quint32 crc = 0xFFFFFFFFu;
	for(const char ch : in)
	{
		crc ^= quint8(ch);
		for(int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

// gzip without zlib: qCompress gives a 4-byte size, then a zlib stream (2-byte header, raw deflate,
// 4-byte Adler-32); the raw deflate part goes into a gzip frame (header, deflate, CRC-32, size)
static QByteArray gzipData(const QByteArray &in)
{
	const QByteArray z = qCompress(in, 9);
	if(z.size() < 4 + 2 + 4)
		return {};
	static const char header[10] = { char(0x1f), char(0x8b), 8, 0, 0, 0, 0, 0, 2, char(0xff) };
	QByteArray out(header, 10);
	out += z.mid(4 + 2, z.size() - 4 - 2 - 4);
	const quint32 crc = crc32Of(in), size = quint32(in.size());
	for(int i = 0; i < 4; i++)
		out += char((crc >> (8 * i)) & 0xFF);
	for(int i = 0; i < 4; i++)
		out += char((size >> (8 * i)) & 0xFF);
	return out;
}

// Tokens and ids never leave the PC (the session log itself already hides addresses and console ids)
QString RedactForReport(QString text, const QString &oviora_token)
{
	if(!oviora_token.isEmpty())
		text.replace(oviora_token, "<redacted-token>");
	static const QRegularExpression labelled(
		R"(((?:token|access_token|refresh_token|npsso|authorization|code|password|pass|sifre|account_?id|accountid)["']?\s*[:=]\s*["']?)([^\s"',;&]+))",
		QRegularExpression::CaseInsensitiveOption);
	text.replace(labelled, "\\1<redacted>");
	static const QRegularExpression bearer(R"((Bearer\s+)\S+)", QRegularExpression::CaseInsensitiveOption);
	text.replace(bearer, "\\1<redacted>");
	// JWTs, long base64-like strings with letters and digits mixed, long numbers (PSN account ids)
	static const QRegularExpression jwt(R"([A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,})");
	text.replace(jwt, "<redacted-token>");
	static const QRegularExpression b64(R"((?<![A-Za-z0-9+_\-])(?=[A-Za-z0-9+_\-]*\d)(?=[A-Za-z0-9+_\-]*[A-Za-z])[A-Za-z0-9+_\-]{32,}={0,2})");
	text.replace(b64, "<redacted-token>");
	// The Windows user name in paths
	static const QRegularExpression user_dir(R"(([\\/][Uu]sers[\\/])[^\\/\s]+)");
	text.replace(user_dir, "\\1<user>");
	static const QRegularExpression digits(R"(\b\d{12,}\b)");
	text.replace(digits, "<redacted-id>");
	return text;
}

void OvioraServer::sendReport(const QString &note, const QString &kind)
{
	if(report_sending || token.isEmpty())
		return;
	static const QStringList kinds = { "baglanti", "goruntu", "ses", "altyazi", "diger" };
	const QString k = kinds.contains(kind) ? kind : QString("diger");
	// Server limit: 3 MB after base64 for all files; long logs lose their start (the end matters most)
	const int budget = 3 * 1024 * 1024 / 4 * 3 - 64 * 1024;
	QJsonArray files;
	int used = 0;
	for(const QString &path : recentSessionLogs(3))
	{
		QFile f(path);
		if(!f.open(QIODevice::ReadOnly))
			continue;
		QString text = RedactForReport(QString::fromLocal8Bit(f.readAll()), token);
		QByteArray gz = gzipData(text.toUtf8());
		const int left = budget - used;
		while(!gz.isEmpty() && gz.size() > left && text.size() > 4096)
		{
			text = "[... başı kesildi ...]\n" + text.right(text.size() / 2);
			gz = gzipData(text.toUtf8());
		}
		if(gz.isEmpty() || gz.size() > left)
			continue;
		used += gz.size();
		files.append(QJsonObject { { "name", QFileInfo(path).fileName() }, { "gz_b64", QString::fromLatin1(gz.toBase64()) } });
	}
	QJsonObject device {
		{ "platform", "windows" },
		{ "model", QSysInfo::currentCpuArchitecture() },
		{ "os", QSysInfo::prettyProductName() },
		{ "app_version", installedVersion().isEmpty() ? QCoreApplication::applicationVersion() : installedVersion() },
	};
	report_sending = true;
	emit reportChanged();
	qCInfo(yakamozServer) << "Sending problem report:" << k << files.size() << "files," << used << "bytes gzip";
	post("report", { { "note", note.left(2000) }, { "kind", k }, { "files", files }, { "device", device } },
		[this](const QJsonObject &r, const QString &error) {
			report_sending = false;
			emit reportChanged();
			if(!error.isEmpty())
				emit reportDone(false, error);
			else
				emit reportDone(true, tr("Bildirimin gönderildi (no. %1). Teşekkürler!").arg(r.value("id").toVariant().toString()));
		});
}

// ---------------------------------------------------------------- optional components

namespace {
struct Component { const char *id; const char *title; QStringList prefixes; };
const QList<Component> &componentList()
{
	static const QList<Component> list = {
		{ "vfg", "NVIDIA VFG", { QStringLiteral("runtime/nvidia-vfg/") } },
		{ "dlssnr", "NVIDIA DLSS NR", { QStringLiteral("runtime/experimental/nr-ampere/"), QStringLiteral("runtime/experimental/nr-original/") } },
	};
	return list;
}
}

QString OvioraServer::componentOf(const QString &path)
{
	const QString p = QDir::fromNativeSeparators(path);
	for(const Component &c : componentList())
		for(const QString &prefix : c.prefixes)
			if(p.startsWith(prefix, Qt::CaseInsensitive))
				return QString::fromLatin1(c.id);
	return QString();
}

bool OvioraServer::componentInstalled(const QString &id) const
{
	for(const Component &c : componentList())
		if(id == QLatin1String(c.id))
			for(const QString &prefix : c.prefixes)
			{
				QDir dir(appDir() + "/" + prefix);
				if(dir.exists() && !dir.entryList(QDir::Files).isEmpty())
					return true;
			}
	return false;
}

QVariantMap OvioraServer::components() const
{
	QVariantMap out;
	for(const Component &c : componentList())
	{
		const QString id = QString::fromLatin1(c.id);
		const bool busy = id == component_id;
		out[id] = QVariantMap {
			{ "title", QString::fromLatin1(c.title) },
			{ "installed", componentInstalled(id) },
			{ "state", busy ? component_state : QStringLiteral("none") },
			{ "text", busy ? component_text : QString() },
			{ "progress", busy ? component_progress : 0.0 },
		};
	}
	return out;
}

void OvioraServer::installComponent(const QString &id)
{
	if(component_state == "working" || token.isEmpty() || !components().contains(id))
		return;
	component_id = id;
	component_state = "working";
	component_progress = 0;
	component_text = tr("Sunucuya soruluyor…");
	emit componentsChanged();
	auto fail = [this](const QString &text) {
		component_state = "error";
		component_text = text;
		emit componentsChanged();
	};
	// The server keeps the components in the newest release that carried them ("component"); a
	// server without that action answers with an error, then the current release is asked
	auto take = [this, id, fail](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty() || r.value("none").toBool())
			return fail(error.isEmpty() ? tr("Bu bileşen sunucudaki sürümde yok; bir sonraki güncellemeyle gelecek.") : error);
		const QByteArray manifest = r.value("manifest").toString().toUtf8();
		if(!verifySignature(manifest, QByteArray::fromBase64(r.value("sig").toString().toLatin1())))
			return fail(tr("İmza doğrulanamadı, indirilmedi."));
		component_queue.clear();
		component_total = component_done = 0;
		for(const QJsonValue &v : QJsonDocument::fromJson(manifest).object().value("files").toArray())
		{
			const QJsonObject f = v.toObject();
			const QString path = f.value("path").toString();
			if(path.isEmpty() || path.contains("..") || path.contains('\\') || path.contains(':') || QDir::isAbsolutePath(path) || componentOf(path) != id)
				continue;
			component_queue.append({ path, f.value("sha256").toString(), qint64(f.value("size").toDouble()) });
			component_total += qint64(f.value("size").toDouble());
		}
		if(component_queue.isEmpty())
			return fail(tr("Bu bileşen sunucudaki sürümde yok; bir sonraki güncellemeyle gelecek."));
		componentNext();
	};
	post("component", {}, [this, take](const QJsonObject &r, const QString &error) {
		if(error.isEmpty())
			return take(r, error);
		post("update", { { "version", installedVersion() } }, take);
	});
}

void OvioraServer::componentNext()
{
	if(component_queue.isEmpty())
	{
		component_state = "done";
		component_progress = 1;
		component_text = tr("Yüklendi. Kullanmak için Oviora'yı yeniden başlat.");
		qCInfo(yakamozServer) << "Component installed:" << component_id;
		emit componentsChanged();
		return;
	}
	const UpdateFile f = component_queue.first();
	const QString target = appDir() + "/" + f.path;
	QDir().mkpath(QFileInfo(target).absolutePath());
	auto *file = new QFile(target + ".part", this);
	if(!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		delete file;
		component_state = "error";
		component_text = tr("Dosya yazılamadı: %1").arg(f.path);
		emit componentsChanged();
		return;
	}
	QUrl url(server_url);
	QUrlQuery q;
	q.addQueryItem("a", "file");
	q.addQueryItem("h", f.sha256);
	url.setQuery(q);
	QNetworkRequest req(url);
	req.setRawHeader("X-Yakamoz-Token", token.toUtf8());
	req.setTransferTimeout(60000);
	QNetworkReply *reply = net->get(req);
	pin(reply);
	connect(reply, &QNetworkReply::readyRead, this, [this, reply, file]() {
		const QByteArray chunk = reply->readAll();
		file->write(chunk);
		component_done += chunk.size();
		component_progress = component_total ? double(component_done) / double(component_total) : 0;
		component_text = tr("İndiriliyor: %1 / %2 MB").arg(component_done / 1048576).arg(component_total / 1048576);
		emit componentsChanged();
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, file, f, target]() {
		reply->deleteLater();
		file->write(reply->readAll());
		file->close();
		bool ok = reply->error() == QNetworkReply::NoError;
		QCryptographicHash h(QCryptographicHash::Sha256);
		if(ok && file->open(QIODevice::ReadOnly))
		{
			h.addData(file);
			file->close();
		}
		if(!ok || QString::fromLatin1(h.result().toHex()) != f.sha256)
		{
			file->remove();
			file->deleteLater();
			component_state = "error";
			component_text = tr("İndirilemedi (%1), tekrar dene.").arg(f.path);
			emit componentsChanged();
			return;
		}
		QFile::remove(target);
		file->rename(target);
		file->deleteLater();
		component_queue.removeFirst();
		componentNext();
	});
}

void OvioraServer::restartApp()
{
	QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList(), appDir());
	QCoreApplication::quit();
}
