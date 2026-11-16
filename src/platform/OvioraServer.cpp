// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "oviora/platform/OvioraServer.h"
#include "oviora/platform/SecretStore.h"
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
#include <openssl/rand.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

Q_LOGGING_CATEGORY(yakamozServer, "oviora.server")

static const char *GROUP = "yakamoz_server";
static const char *CODE_PREFIX = "YAKAMOZ-SUNUCU-";
static const int POLL_MS = 3000;
static const int ACCOUNT_MS = 10000;
static const int UPDATE_CHECK_MS = 6 * 3600 * 1000;
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
	// Shortcuts, the app list entry and the old exe: a moment after start, the old exe's starter has ended by then
	QTimer::singleShot(4000, this, &OvioraServer::takeOverOldName);
	poll_timer.setInterval(POLL_MS);
	connect(&poll_timer, &QTimer::timeout, this, [this]() {
		post("poll", { { "sid", session_id } }, [this](const QJsonObject &r, const QString &error) {
			if(session_state != "playing" || !error.isEmpty() || !r.value("kick").toBool())
				return;
			qCInfo(yakamozServer) << "The session was ended by the console's owner";
			QString host = session_host;
			endSession();
			if(this->hooks.stop_session)
				this->hooks.stop_session();
			emit message(tr("Oturum sonlandırıldı"), host == user_name
				? tr("Oturum yönetim sitesinden kapatıldı.") : tr("%1 oturumu sonlandırdı.").arg(host));
		});
	});
	// Friends, requests and who plays where change on other PCs: look again now and then
	account_timer.setInterval(ACCOUNT_MS);
	connect(&account_timer, &QTimer::timeout, this, &OvioraServer::refresh);
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

QNetworkReply *OvioraServer::post(const QString &action, QJsonObject body, Callback done)
{
	QNetworkRequest req{QUrl(server_url)};
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	req.setTransferTimeout(30000);
	body["a"] = action;
	// The server only lets the current Yakamoz do anything but update
	body["client"] = "win:" + installedVersion();
	if(!token.isEmpty() && !body.contains("token"))
		body["token"] = token;
	QNetworkReply *reply = net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	pin(reply);
	connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
		reply->deleteLater();
		QJsonObject json = QJsonDocument::fromJson(reply->readAll()).object();
		if(json.value("relogin").toBool() && !token.isEmpty())
			loggedOut(json.value("error").toString());
		if(json.value("update_required").toBool())
			requireUpdate();
		if(json.contains("error"))
			done(json, json.value("error").toString());
		else if(reply->error() != QNetworkReply::NoError)
			done(json, tr("Sunucuya ulaşılamadı (%1)").arg(reply->errorString()));
		else
			done(json, QString());
	});
	return reply;
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

void OvioraServer::reportConsoleApp(const QString &duid, const QString &app_name, const QString &title_id, const QString &state)
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
	account_status = "active";
	emit changed();
	refresh();
	account_timer.start();
	QTimer::singleShot(8000, this, &OvioraServer::checkUpdate);
	update_timer.start();
}

void OvioraServer::loggedOut(const QString &why)
{
	token.clear();
	account_status = "none";
	account = QJsonObject();
	invite_info.clear();
	invite_qr.clear();
	history_list.clear();
	history_more = false;
	history_next_before = 0;
	emit inviteChanged();
	emit historyChanged();
	notification_list.clear();
	unread = 0;
	emit notificationsChanged();
	account_timer.stop();
	update_timer.stop();
	save();
	emit changed();
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
	post("invite", body, [this](const QJsonObject &r, const QString &error) {
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
	post("invite", { { "approve", approve } }, [this](const QJsonObject &r, const QString &error) {
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
	post("invite_accept", { { "code", code } }, [this](const QJsonObject &r, const QString &error) {
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
	post("history", body, [this, more](const QJsonObject &r, const QString &error) {
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

void OvioraServer::takeAccount(const QJsonObject &r)
{
	if(!r.contains("user"))
		return;
	account = r;
	if(r.value("update_required").toBool())
		requireUpdate();
	else if(update_required)
	{
		update_required = false;
		emit updateChanged();
	}
	emit accountChanged();
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
	post(action, body, [this, fail_title](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty())
		{
			emit message(fail_title, error);
			busy = false;
			emit accountChanged();
			return;
		}
		busy = false;
		takeAccount(r);
	});
}

void OvioraServer::refresh()
{
	if(!configured() || token.isEmpty())
		return;
	post("me", {}, [this](const QJsonObject &r, const QString &error) {
		if(error.isEmpty())
			takeAccount(r);
	});
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

void OvioraServer::linkPsn(const QString &redirect)
{
	busy = true;
	emit accountChanged();
	accountCall("psn_link", { { "redirect", redirect } }, tr("PSN hesabı bağlanamadı"));
}

void OvioraServer::unlinkPsn()
{
	accountCall("psn_unlink", {}, tr("PSN hesabı kaldırılamadı"));
}

void OvioraServer::savePsnAccount(const QString &id, const QString &name)
{
	const QString trimmedId = id.trimmed();
	if(!signedIn() || trimmedId.isEmpty())
		return;
	post("psn_account_save", { { "id", trimmedId }, { "name", name.trimmed().left(64) } }, [this](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty())
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
	post("psn_account_remove", { { "id", trimmedId } }, [this](const QJsonObject &r, const QString &error) {
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

void OvioraServer::refreshConsoles()
{
	busy = true;
	emit accountChanged();
	accountCall("consoles_refresh", {}, tr("Konsollar yenilenemedi"));
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

void OvioraServer::setShare(const QString &duid, const QString &friend_name, bool on)
{
	accountCall("share", { { "duid", duid }, { "friend", friend_name }, { "on", on } }, tr("Paylaşım değiştirilemedi"));
}

void OvioraServer::kick(const QString &sid)
{
	accountCall("kick", { { "sid", sid } }, tr("Çıkarılamadı"));
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
	session_id = QString::fromLatin1(QCryptographicHash::hash(QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + QByteArray::number(QRandomGenerator::global()->generate64()), QCryptographicHash::Sha1).toHex().left(16));
	session_host = host;
	session_duid = duid;
	setSession("asking");
	// "relay": this app can let the server make the PSN requests (S2): the owner's token never comes here
	post("lease", { { "host", host }, { "duid", duid }, { "sid", session_id }, { "relay", 1 } }, [this](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty() || r.value("mode").toString() != "psn")
		{
			setSession("idle");
			emit message(tr("Bağlanılamadı"), error.isEmpty() ? tr("Sunucu beklenmedik bir cevap verdi.") : error);
			refresh();
			return;
		}
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

bool OvioraServer::verifySignature(const QByteArray &manifest, const QByteArray &signature) const
{
	QByteArray pub = QByteArray::fromBase64(publisher_pub.toLatin1());
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
	if(qEnvironmentVariableIsSet("YAKAMOZ_TEST_GUNCELLEME_YOK"))
		return;
	update_state = "checking";
	update_text = tr("Güncelleme denetleniyor…");
	emit updateChanged();
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
		update_version = r.value("version").toString();
		update_notes = r.value("notes").toString();
#ifndef OVIORA_PLATFORM_UPDATES
		// Development build: never download or install the server's release (the old Oviora)
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
#endif
		QList<UpdateFile> files;
		for(const QJsonValue &v : QJsonDocument::fromJson(manifest).object().value("files").toArray())
		{
			QJsonObject f = v.toObject();
			QString path = f.value("path").toString();
			// Never outside the program folder
			if(path.isEmpty() || path.contains("..") || path.contains('\\') || path.contains(':') || QDir::isAbsolutePath(path))
				continue;
			files.append({ path, f.value("sha256").toString(), qint64(f.value("size").toDouble()) });
		}
		// Which files differ from what is installed (hashing runs off the UI thread)
		auto watcher = new QFutureWatcher<QList<UpdateFile>>(this);
		connect(watcher, &QFutureWatcher<QList<UpdateFile>>::finished, this, [this, watcher, files]() {
			watcher->deleteLater();
			update_files = files;
			update_queue = watcher->result();
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
	});
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
	post("events", { { "since", double(since) } }, [this, announce_after](const QJsonObject &r, const QString &error) {
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
		if(r.contains("last"))
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
	post("queue", { { "duid", duid }, { "on", on } }, [this, on](const QJsonObject &r, const QString &error) {
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
