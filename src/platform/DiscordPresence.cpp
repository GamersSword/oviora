// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Oviora: see DiscordPresence.h. Discord's IPC frame: int32 opcode, int32 length (both little
// endian), then that many bytes of UTF-8 JSON. Opcodes: 0 handshake, 1 frame, 2 close, 3 ping, 4 pong.
// QLocalSocket opens "discord-ipc-N" as \\.\pipe\discord-ipc-N on Windows (overlapped, no thread).
#include "oviora/platform/DiscordPresence.h"
#include "oviora/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QUrl>
#include <QUuid>
#include <QtEndian>

#include <format>

namespace {

constexpr auto client_id = "1573005738928771112";
constexpr int pipe_count = 10;
constexpr int retry_ms = 30000;
constexpr int handshake_ms = 10000;
// Discord allows 5 SET_ACTIVITY per 20 s
constexpr qint64 send_gap_ms = 4000;

enum Op { OpHandshake = 0, OpFrame = 1, OpClose = 2, OpPing = 3, OpPong = 4 };

// Discord's limits for details/state: 2..128 characters
QString fit(QString text)
{
	text = text.simplified();
	if(text.size() > 128)
	{
		text.truncate(128);
		if(text.back().isHighSurrogate())
			text.chop(1);
	}
	return text;
}

std::string utf8(const QString &s) { return s.toStdString(); }

} // namespace

// The Turkish genitive after a name: Ahmet'in, Ali'nin, Burak'ın, Oya'nın, Umut'un, Gökhan'ın, Ürün'ün
QString DiscordPresence::turkishGenitive(const QString &name)
{
	QChar last;
	for(qsizetype i = name.size() - 1; i >= 0 && last.isNull(); i--)
		if(QStringLiteral("aıAIeiEİoOuUöÖüÜ").contains(name.at(i)))
			last = name.at(i);
	if(last.isNull() || !name.back().isLetter())
		return name + QStringLiteral("'in");
	QChar vowel = QChar(u'i');
	if(QStringLiteral("aıAI").contains(last))
		vowel = QChar(u'ı');
	else if(QStringLiteral("oOuU").contains(last))
		vowel = QChar(u'u');
	else if(QStringLiteral("öÖüÜ").contains(last))
		vowel = QChar(u'ü');
	const bool endsVowel = QStringLiteral("aıAIeiEİoOuUöÖüÜ").contains(name.back());
	return name + QChar('\'') + (endsVowel ? QStringLiteral("n") : QString()) + vowel + QChar('n');
}

DiscordPresence::DiscordPresence(QObject *parent)
	: QObject(parent)
{
	retry_timer.setSingleShot(true);
	connect(&retry_timer, &QTimer::timeout, this, &DiscordPresence::startConnect);
	handshake_timer.setSingleShot(true);
	connect(&handshake_timer, &QTimer::timeout, this, [this] { dropLink(true); });
	flush_timer.setSingleShot(true);
	connect(&flush_timer, &QTimer::timeout, this, &DiscordPresence::flush);
}

DiscordPresence::~DiscordPresence()
{
	// Closing the pipe is enough: Discord drops the activity of a client that went away
	dropLink(false);
}

void DiscordPresence::setEnabled(bool on)
{
	if(enabled == on)
		return;
	enabled = on;
	if(on)
	{
		if(wanted())
			startConnect();
		return;
	}
	retry_timer.stop();
	flush_timer.stop();
	if(link == Link::Ready && !sent.isEmpty())
	{
		send(OpFrame, QJsonObject{ { "cmd", "SET_ACTIVITY" },
			{ "args", QJsonObject{ { "pid", qint64(QCoreApplication::applicationPid()) }, { "activity", QJsonValue::Null } } },
			{ "nonce", QUuid::createUuid().toString(QUuid::WithoutBraces) } });
		oviora::log::info("discord", "activity cleared");
		// disconnectFromServer() writes the clear first; disconnected() then drops the link
		socket->disconnectFromServer();
		return;
	}
	dropLink(false);
}

void DiscordPresence::setPlaying(const QString &game_name, const QString &icon_url, bool friendConsole, const QString &owner_name, qint64 startedMs)
{
	playing = true;
	game = game_name;
	// Only https covers on Sony's hosts (the server checks it too); Discord fetches them through its proxy
	const QUrl url(icon_url);
	const QString host = url.host().toLower();
	icon = url.isValid() && url.scheme() == QLatin1String("https") && icon_url.size() <= 256
		&& (host.endsWith(QLatin1String(".playstation.com")) || host.endsWith(QLatin1String(".playstation.net"))) ? icon_url : QString();
	friend_console = friendConsole;
	owner = owner_name;
	started_ms = startedMs;
	if(!enabled)
		return;
	if(link == Link::Off)
	{
		// Discord not found: the retry timer comes back to it
		if(!retry_timer.isActive())
			startConnect();
		return;
	}
	flush();
}

void DiscordPresence::setIdle()
{
	if(!playing)
		return;
	playing = false;
	retry_timer.stop();
	if(link == Link::Ready)
		flush();
}

// ---------------------------------------------------------------- pipe

void DiscordPresence::startConnect()
{
	if(link != Link::Off || !wanted())
		return;
	link = Link::Connecting;
	pipe_index = 0;
	connectNext();
}

void DiscordPresence::connectNext()
{
	if(link != Link::Connecting)
		return;
	if(socket)
	{
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
		socket = nullptr;
	}
	if(pipe_index >= pipe_count)
	{
		link = Link::Off;
		if(wanted())
			retry_timer.start(retry_ms);
		emit unavailable();
		return;
	}
	socket = new QLocalSocket(this);
	connect(socket, &QLocalSocket::connected, this, [this] {
		link = Link::Handshake;
		buffer.clear();
		handshake_timer.start(handshake_ms);
		send(OpHandshake, QJsonObject{ { "v", 1 }, { "client_id", client_id } });
	});
	connect(socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
		if(link == Link::Connecting)
		{
			// This pipe isn't there (or is busy): the next one, from the event loop
			pipe_index++;
			QMetaObject::invokeMethod(this, &DiscordPresence::connectNext, Qt::QueuedConnection);
			return;
		}
		dropLink(true);
	});
	connect(socket, &QLocalSocket::disconnected, this, [this] { dropLink(true); });
	connect(socket, &QLocalSocket::readyRead, this, &DiscordPresence::onReadyRead);
	socket->connectToServer(QStringLiteral("discord-ipc-%1").arg(pipe_index));
}

void DiscordPresence::dropLink(bool retry)
{
	const bool wasReady = link == Link::Ready;
	if(socket)
	{
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
		socket = nullptr;
	}
	link = Link::Off;
	buffer.clear();
	handshake_timer.stop();
	flush_timer.stop();
	// Discord drops the activity with the pipe
	sent = QJsonObject();
	logged.clear();
	pending_nonce.clear();
	if(wasReady)
		oviora::log::info("discord", "disconnected");
	if(retry && wanted())
		retry_timer.start(retry_ms);
}

void DiscordPresence::send(int op, const QJsonObject &payload)
{
	if(!socket)
		return;
	const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	QByteArray frame(8, '\0');
	qToLittleEndian<qint32>(op, frame.data());
	qToLittleEndian<qint32>(qint32(json.size()), frame.data() + 4);
	socket->write(frame + json);
}

void DiscordPresence::onReadyRead()
{
	if(!socket)
		return;
	buffer += socket->readAll();
	while(buffer.size() >= 8)
	{
		const qint32 op = qFromLittleEndian<qint32>(buffer.constData());
		const qint32 length = qFromLittleEndian<qint32>(buffer.constData() + 4);
		if(length < 0 || length > (1 << 20))
		{
			oviora::log::warn("discord", "malformed frame");
			dropLink(true);
			return;
		}
		if(buffer.size() < 8 + length)
			return;
		const QByteArray body = buffer.mid(8, length);
		buffer.remove(0, 8 + length);
		handleFrame(op, body);
		if(!socket)
			return;
	}
}

void DiscordPresence::handleFrame(int op, const QByteArray &body)
{
	switch(op)
	{
	case OpPing:
	{
		// The pong carries the ping's payload back as it came
		QByteArray frame(8, '\0');
		qToLittleEndian<qint32>(OpPong, frame.data());
		qToLittleEndian<qint32>(qint32(body.size()), frame.data() + 4);
		socket->write(frame + body);
		return;
	}
	case OpPong:
		return;
	case OpClose:
	{
		const QJsonObject o = QJsonDocument::fromJson(body).object();
		oviora::log::info("discord", std::format("closed by Discord code={}", o.value("code").toInt()));
		dropLink(true);
		return;
	}
	case OpFrame:
		break;
	default:
		return;
	}

	const QJsonObject o = QJsonDocument::fromJson(body).object();
	const QString cmd = o.value("cmd").toString(), evt = o.value("evt").toString();
	if(cmd == QLatin1String("DISPATCH") && evt == QLatin1String("READY"))
	{
		handshake_timer.stop();
		link = Link::Ready;
		if(verbose)
		{
			// The test's look at the reply; the Discord user in it is left out
			const QJsonObject data = o.value("data").toObject(), config = data.value("config").toObject();
			oviora::log::info("discord", std::format("READY v={} api={} cdn={} environment={} user={}",
				data.value("v").toInt(), utf8(config.value("api_endpoint").toString()), utf8(config.value("cdn_host").toString()),
				utf8(config.value("environment").toString()), data.contains("user") ? "present (not logged)" : "none"));
		}
		oviora::log::info("discord", "connected");
		emit ready();
		flush();
		return;
	}
	if(cmd == QLatin1String("SET_ACTIVITY"))
	{
		if(o.value("nonce").toString() != pending_nonce)
			return;
		pending_nonce.clear();
		const bool ok = evt != QLatin1String("ERROR");
		if(!ok)
		{
			const QJsonObject data = o.value("data").toObject();
			oviora::log::warn("discord", std::format("SET_ACTIVITY refused code={} {}", data.value("code").toInt(), utf8(data.value("message").toString())));
			// An older Discord that does not know status_display_type: once more without it
			if(display_type && sent.contains(QLatin1String("status_display_type")))
			{
				display_type = false;
				sent = QJsonObject();
				last_send_ms = 0;
				QMetaObject::invokeMethod(this, &DiscordPresence::flush, Qt::QueuedConnection);
				return;
			}
		}
		emit activityAnswered(ok, pending_cleared);
		return;
	}
	if(evt == QLatin1String("ERROR"))
		oviora::log::warn("discord", std::format("{} error code={}", utf8(cmd), o.value("data").toObject().value("code").toInt()));
}

// ---------------------------------------------------------------- activity

QJsonObject DiscordPresence::buildActivity() const
{
	QString details = fit(game);
	const bool known = details.size() >= 2;
	if(!known)
		details = tr("PS5'te oynuyor");
	QString state;
	if(!friend_console)
		state = tr("Kendi PS5'inde");
	else if(owner.trimmed().isEmpty())
		state = tr("Bir arkadaşının PS5'inde");
	else
	{
		const QString name = fit(owner).left(100);
		const QString pattern = tr("%1'in PS5'inde");
		// Turkish (the source text): the suffix follows the name's vowels; other languages as translated
		state = pattern == QStringLiteral("%1'in PS5'inde") ? turkishGenitive(name) + QStringLiteral(" PS5'inde") : pattern.arg(name);
	}
	// "Kendi PS5'inde · Oviora ile": the card says the game runs through Oviora
	state = tr("%1 · Oviora ile").arg(state);
	// The game's cover large with Oviora's mark in its corner; without a cover Oviora's logo
	QJsonObject assets;
	if(!icon.isEmpty())
		assets = QJsonObject{ { "large_image", icon }, { "large_text", fit(details) },
			{ "small_image", "oviora" }, { "small_text", fit(tr("Oviora · PS5 uzaktan oynatma")) } };
	else
		assets = QJsonObject{ { "large_image", "oviora" }, { "large_text", fit(tr("Oviora · PS5 uzaktan oynatma")) } };
	QJsonObject activity{
		{ "type", 0 },
		{ "details", fit(details) },
		{ "state", fit(state) },
		{ "assets", assets },
		// Seen by others, not by the user: where Oviora is
		{ "buttons", QJsonArray{ QJsonObject{ { "label", tr("Oviora'yı indir").left(32) }, { "url", "https://oviora.tr" } } } },
		{ "instance", false },
	};
	// The member list's "<game> oynuyor" instead of "Oviora oynuyor" (2: details) when the game is known
	if(known && display_type)
		activity["status_display_type"] = 2;
	if(started_ms > 0)
		activity["timestamps"] = QJsonObject{ { "start", started_ms / 1000 } };
	return activity;
}

void DiscordPresence::flush()
{
	if(link != Link::Ready)
		return;
	const QJsonObject want = wanted() ? buildActivity() : QJsonObject();
	if(want == sent)
		return;
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if(now - last_send_ms < send_gap_ms)
	{
		if(!flush_timer.isActive())
			flush_timer.start(int(send_gap_ms - (now - last_send_ms)));
		return;
	}
	pending_nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
	pending_cleared = want.isEmpty();
	QJsonObject args{ { "pid", qint64(QCoreApplication::applicationPid()) } };
	args["activity"] = want.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(want);
	send(OpFrame, QJsonObject{ { "cmd", "SET_ACTIVITY" }, { "args", args }, { "nonce", pending_nonce } });
	sent = want;
	last_send_ms = now;
	// One line per change of what the profile shows (the start time alone is not logged)
	const QString line = want.isEmpty() ? QString()
		: want.value("details").toString() + QStringLiteral(" / ") + want.value("state").toString();
	if(line != logged || want.isEmpty())
		oviora::log::info("discord", want.isEmpty() ? std::string("activity cleared") : "activity: " + utf8(line));
	logged = line;
}

// ---------------------------------------------------------------- game names

QString DiscordPresence::gameForTitleId(const QString &titleId)
{
	static QHash<QString, QString> names;
	static bool loaded = false;
	if(titleId.isEmpty())
		return QString();
	if(!loaded)
	{
		loaded = true;
		const QDir dir(QCoreApplication::applicationDirPath() + QStringLiteral("/runtime/altyazi/terim"));
		for(const QFileInfo &fi : dir.entryInfoList({ QStringLiteral("*.json") }, QDir::Files))
		{
			QFile f(fi.absoluteFilePath());
			if(!f.open(QIODevice::ReadOnly))
				continue;
			const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
			const QString game = obj.value("game").toString().trimmed();
			if(game.isEmpty())
				continue;
			for(const QJsonValue &id : obj.value("titleIds").toArray())
				names.insert(id.toString().toUpper(), game);
		}
	}
	return names.value(titleId.toUpper());
}

// ---------------------------------------------------------------- --test-discord

int DiscordPresence::runTest()
{
	DiscordPresence presence;
	presence.verbose = true;
	QEventLoop loop;
	int result = 1;
	connect(&presence, &DiscordPresence::unavailable, &loop, [&] {
		oviora::log::info("discord", "test: no Discord pipe answered (Discord not running)");
		result = 2;
		loop.quit();
	});
	connect(&presence, &DiscordPresence::activityAnswered, &loop, [&](bool ok, bool cleared) {
		if(!ok)
		{
			loop.quit();
			return;
		}
		if(!cleared)
		{
			oviora::log::info("discord", "test: activity accepted, cleared in 8 s");
			QTimer::singleShot(8000, &presence, [&presence] { presence.setIdle(); });
			return;
		}
		oviora::log::info("discord", "test: clear accepted");
		result = 0;
		loop.quit();
	});
	QTimer::singleShot(30000, &loop, [&] {
		oviora::log::warn("discord", "test: no answer in 30 s");
		loop.quit();
	});
	// --test-discord <cover url>: that cover (a Sony https address), else Oviora's logo
	const QStringList args = QCoreApplication::arguments();
	const qsizetype at = args.indexOf(QStringLiteral("--test-discord"));
	const QString cover = at >= 0 && at + 1 < args.size() && !args.at(at + 1).startsWith(QLatin1String("--")) ? args.at(at + 1) : QString();
	presence.setPlaying(QStringLiteral("Grand Theft Auto VI"), cover, false, QString(), QDateTime::currentMSecsSinceEpoch());
	loop.exec();
	oviora::log::info("discord", std::format("test result {}", result));
	return result;
}
