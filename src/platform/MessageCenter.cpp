// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "oviora/platform/MessageCenter.h"
#include "oviora/platform/OvioraServer.h"
#include "oviora/platform/SecretStore.h"
#include "oviora/RuntimePaths.h"

#include <chiaki/e2e.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSettings>

#include <algorithm>

Q_LOGGING_CATEGORY(ovioraMessages, "oviora.messages")

// Kept on this PC per friend (the server keeps them 30 days, encrypted)
static constexpr int KEEP_PER_FRIEND = 500;
static constexpr int MAX_TEXT = 2000;

// The new Oviora's own data folder: platform.ini (the device key, group "mesaj") and mesajlar.dat.
// The old Oviora's settings and folders are only ever read (takeOverOldData).
static QString dataPath(const wchar_t *name)
{
	return QString::fromStdWString((oviora::runtime::localDataDirectory() / name).wstring());
}
static QString store_path() { return dataPath(L"mesajlar.dat"); }
#define APP_SETTINGS(name) QSettings name(dataPath(L"platform.ini"), QSettings::IniFormat)

// First start: the old Oviora's device key (portable "ayarlar" folder next to the exe, else the
// registry) and its history (its app data folder), so chats and the key a friend trusts carry on.
// The key and the file are DPAPI-encrypted for this Windows user and are copied as they are.
static void takeOverOldData()
{
	{
		APP_SETTINGS(now);
		if(!now.contains(QStringLiteral("mesaj/device")))
		{
			const QString ini = QCoreApplication::applicationDirPath() + QStringLiteral("/ayarlar/Oviora/Oviora.ini");
			QList<QSettings *> sources;
			if(QFileInfo::exists(ini))
				sources << new QSettings(ini, QSettings::IniFormat);
			sources << new QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("Oviora"), QStringLiteral("Oviora"));
			for(QSettings *old : sources)
			{
				const QString device = old->value(QStringLiteral("mesaj/device")).toString();
				const QString key = old->value(QStringLiteral("mesaj/key")).toString();
				if(device.isEmpty() || key.isEmpty())
					continue;
				now.setValue(QStringLiteral("mesaj/device"), device);
				now.setValue(QStringLiteral("mesaj/key"), key);
				qCInfo(ovioraMessages) << "Took over the old Oviora's message key from" << old->fileName();
				break;
			}
			qDeleteAll(sources);
		}
	}
	const QString target = store_path();
	if(QFileInfo::exists(target))
		return;
	const QString appdata = qEnvironmentVariable("APPDATA");
	if(appdata.isEmpty())
		return;
	const QString old_file = QDir::fromNativeSeparators(appdata) + QStringLiteral("/Oviora/Oviora/mesajlar.dat");
	if(!QFileInfo::exists(old_file))
		return;
	QDir().mkpath(QFileInfo(target).absolutePath());
	if(QFile::copy(old_file, target))
		qCInfo(ovioraMessages) << "Took over the old Oviora's message history from" << old_file;
}

static QByteArray b64(const QString &s) { return QByteArray::fromBase64(s.toLatin1()); }
static QString to_b64(const QByteArray &b) { return QString::fromLatin1(b.toBase64()); }

MessageCenter::MessageCenter(OvioraServer *server, QObject *parent)
	: QObject(parent), server(server)
{
	poll_timer.setInterval(10000);
	connect(&poll_timer, &QTimer::timeout, this, &MessageCenter::fetch);
	// "yazıyor…" ends by itself when no newer word came (the server keeps it 6 s)
	typing_timer.setInterval(1000);
	connect(&typing_timer, &QTimer::timeout, this, [this]() {
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		bool changed = false;
		for(auto it = typing_until.begin(); it != typing_until.end();)
			if(it.value() <= now) { it = typing_until.erase(it); changed = true; }
			else ++it;
		if(typing_until.isEmpty())
			typing_timer.stop();
		if(changed)
			emit typingChanged();
	});
	connect(server, &OvioraServer::changed, this, &MessageCenter::onLogin);
	// A group renamed, left or deleted: its title and the open conversation follow
	connect(server, &OvioraServer::groupsChanged, this, &MessageCenter::chatChanged);
	// A groups view really came (refreshGroups calls back on errors too)
	connect(server, &OvioraServer::groupsTaken, this, [this]() { groups_seen++; });
	takeOverOldData();
	save_thread = std::thread([this]() {
		for(;;)
		{
			QString path;
			QByteArray plain;
			{
				std::unique_lock<std::mutex> lock(save_mutex);
				save_cv.wait(lock, [this]() { return save_pending || save_stop; });
				if(!save_pending && save_stop)
					break;
				path = std::move(pending_save_path);
				plain = std::move(pending_save_plain);
				save_pending = false;
				save_writing = true;
			}
			if(!path.isEmpty() && !plain.isEmpty())
			{
				QDir().mkpath(QFileInfo(path).absolutePath());
				const QByteArray sealed = SecretStore::protect(QString::fromUtf8(plain)).toLatin1();
				QSaveFile f(path);
				if(f.open(QIODevice::WriteOnly))
				{
					f.write(sealed);
					f.commit();
				}
			}
			{
				std::lock_guard<std::mutex> lock(save_mutex);
				save_writing = false;
			}
			save_cv.notify_all();
		}
	});
	onLogin();
}

MessageCenter::~MessageCenter()
{
	{
		std::lock_guard<std::mutex> lock(save_mutex);
		save_stop = true;
	}
	save_cv.notify_all();
	if(save_thread.joinable())
		save_thread.join();
}

QString MessageCenter::me() const
{
	return server ? server->user().toLower() : QString();
}

void MessageCenter::onLogin()
{
	if(!server || !server->signedIn())
	{
		poll_timer.stop();
		++session_gen;
		publishing = false;
		fetching = false;
		messages.clear();
		unread_count.clear();
		group_unread.clear();
		my_read.clear();
		read_sent.clear();
		peer_read.clear();
		group_read.clear();
		reads_last = 0;
		typing_until.clear();
		keys.clear();
		trusted.clear();
		key_changed.clear();
		chat_with.clear();
		chat_group = 0;
		key_published = false;
		loaded_for.clear();
		last_id = 0;
		if(server && server->testGroups())
			fillTestMessages();
		emit chatChanged();
		emit unreadChanged();
		emit readyChanged();
		return;
	}
	if(key_published)
		return;
	ensureKey();
	// Only once per account: a second load would drop what came or was written since
	if(loaded_for != me())
	{
		load();
		loaded_for = me();
	}
	publishKey();
	// Also while the key is not on the server yet: each round tries to publish it again (fetch)
	poll_timer.start();
}

// The device key: made once, kept DPAPI-encrypted in the settings (per Windows user and this app)
void MessageCenter::ensureKey()
{
	if(!priv.isEmpty())
		return;
	APP_SETTINGS(s);
	s.beginGroup(QStringLiteral("mesaj"));
	device_id = s.value(QStringLiteral("device")).toString();
	priv = b64(SecretStore::unprotect(s.value(QStringLiteral("key")).toString()));
	if(priv.size() != CHIAKI_E2E_KEY_SIZE || device_id.isEmpty())
	{
		priv.resize(CHIAKI_E2E_KEY_SIZE);
		pub.resize(CHIAKI_E2E_KEY_SIZE);
		if(chiaki_e2e_keygen(reinterpret_cast<uint8_t *>(priv.data()), reinterpret_cast<uint8_t *>(pub.data())) != CHIAKI_ERR_SUCCESS)
		{
			priv.clear();
			pub.clear();
			return;
		}
		device_id = QString::fromLatin1(QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0'));
		s.setValue(QStringLiteral("device"), device_id);
		s.setValue(QStringLiteral("key"), SecretStore::protect(to_b64(priv)));
		qCInfo(ovioraMessages) << "New device key" << device_id;
	}
	else
	{
		pub.resize(CHIAKI_E2E_KEY_SIZE);
		chiaki_e2e_public(reinterpret_cast<const uint8_t *>(priv.constData()), reinterpret_cast<uint8_t *>(pub.data()));
	}
	s.endGroup();
}

void MessageCenter::publishKey()
{
	if(priv.isEmpty() || !server || publishing)
		return;
	publishing = true;
	const quint64 gen = session_gen;
	server->call(QStringLiteral("e2e_key_put"), { { "device", device_id }, { "pub", to_b64(pub) } },
		[this, gen](const QJsonObject &, const QString &error) {
			if(gen != session_gen)
				return;
			publishing = false;
			if(!error.isEmpty())
			{
				qCInfo(ovioraMessages) << "e2e_key_put failed:" << error;
				return;	// tried again at the next fetch round
			}
			qCInfo(ovioraMessages) << "e2e_key_put ok, device" << device_id;
			key_published = true;
			emit readyChanged();
			fetch();
		});
}

void MessageCenter::loadKeys(const QStringList &users, std::function<void(bool ok)> then)
{
	if(!server)
		return;
	QJsonArray list;
	for(const QString &u : users)
		list.append(u);
	const quint64 gen = session_gen;
	server->call(QStringLiteral("e2e_keys"), { { "users", list } }, [this, gen, then, asked = users.size()](const QJsonObject &r, const QString &error) {
		if(gen != session_gen)
			return;
		if(!error.isEmpty())
			qCInfo(ovioraMessages) << "e2e_keys failed:" << error;
		if(error.isEmpty())
		{
			const QJsonObject all = r.value("keys").toObject();
			qCInfo(ovioraMessages) << "e2e_keys ok," << all.size() << "of" << asked << "users have keys";
			bool trusted_changed = false;
			bool keys_changed = false;
			for(auto it = all.constBegin(); it != all.constEnd(); ++it)
			{
				const QString user = it.key().toLower();
				QList<DeviceKey> devices;
				bool changed = false;
				for(const QJsonValue &v : it.value().toArray())
				{
					const QJsonObject o = v.toObject();
					DeviceKey k { o.value("device").toString(), b64(o.value("pub").toString()) };
					if(k.pub.size() != CHIAKI_E2E_KEY_SIZE || k.device.isEmpty())
						continue;
					devices.append(k);
					// Trust on first use; a different key for a known device is a warning
					const QString tkey = user + QLatin1Char('|') + k.device;
					const auto t = trusted.constFind(tkey);
					if(t == trusted.constEnd())
					{
						trusted.insert(tkey, k.pub);
						trusted_changed = true;
					}
					else if(t.value() != k.pub)
						changed = true;
				}
				if(keys.value(user) != devices)
				{
					keys.insert(user, devices);
					keys_changed = true;
				}
				if(user != me() && key_changed.value(user, false) != changed)
				{
					key_changed.insert(user, changed);
					keys_changed = true;
				}
			}
			if(trusted_changed)
				save();
			if(keys_changed)
				emit chatChanged();
		}
		if(then)
			then(error.isEmpty());
	});
}

QByteArray MessageCenter::aad(const QString &from, const QString &to, const QString &mid) const
{
	return QStringLiteral("oviora-msg-v1|%1|%2|%3").arg(from.toLower(), to.toLower(), mid).toUtf8();
}

QJsonArray MessageCenter::encryptFor(const QString &user, const QList<DeviceKey> &devices, const QByteArray &plain, const QByteArray &ad) const
{
	QJsonArray items;
	for(const DeviceKey &k : devices)
	{
		if(user == me() && k.device == device_id)
			continue;	// this device keeps its own copy in plain text locally
		QByteArray eph(CHIAKI_E2E_KEY_SIZE, 0), nonce(CHIAKI_E2E_NONCE_SIZE, 0), ct(plain.size() + CHIAKI_E2E_TAG_SIZE, 0);
		if(chiaki_e2e_encrypt(reinterpret_cast<const uint8_t *>(priv.constData()), reinterpret_cast<const uint8_t *>(k.pub.constData()),
			reinterpret_cast<const uint8_t *>(ad.constData()), size_t(ad.size()),
			reinterpret_cast<const uint8_t *>(plain.constData()), size_t(plain.size()),
			reinterpret_cast<uint8_t *>(eph.data()), reinterpret_cast<uint8_t *>(nonce.data()), reinterpret_cast<uint8_t *>(ct.data())) != CHIAKI_ERR_SUCCESS)
			continue;
		items.append(QJsonObject { { "device", k.device }, { "eph", to_b64(eph) }, { "nonce", to_b64(nonce) }, { "ct", to_b64(ct) } });
	}
	return items;
}

void MessageCenter::send(const QString &text_in)
{
	const QString text = text_in.trimmed().left(MAX_TEXT);
	const QString to = chat_with;
	const int group = chat_group;
	if(text.isEmpty() || (to.isEmpty() && !group) || !server || !server->signedIn() || priv.isEmpty())
		return;
	Message m;
	m.mid = QString::fromLatin1(QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0'));
	m.peer = group ? server->user() : to;
	m.group = group;
	m.mine = true;
	m.text = text;
	m.at = QDateTime::currentMSecsSinceEpoch();
	messages.append(m);
	emit chatChanged();
	stopTyping();
	if(group)
		sendGroup(m.mid);
	else
		sendDirect(m.mid);
}

void MessageCenter::resend(const QString &mid)
{
	auto it = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
	if(it == messages.end() || !it->mine || !it->failed || it->id != 0 || !server || !server->signedIn() || priv.isEmpty())
		return;
	// The same id: the server keeps a message only once, should the first try have reached it after all
	Message m = *it;
	messages.erase(it);
	m.failed = false;
	m.at = QDateTime::currentMSecsSinceEpoch();
	messages.append(m);
	emit chatChanged();
	if(m.group)
		sendGroup(m.mid);
	else
		sendDirect(m.mid);
}

void MessageCenter::sendDirect(const QString &mid)
{
	auto first = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
	if(first == messages.end())
		return;
	const QString to = first->peer;
	const QString text = first->text;
	// Fresh keys for both sides right before encrypting (new devices, changed keys)
	loadKeys({ to, me() }, [this, to, mid, text](bool) {
		auto it = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
		if(it == messages.end())
			return;
		const QList<DeviceKey> theirs = keys.value(to.toLower());
		if(theirs.isEmpty())
		{
			it->failed = true;
			emit chatChanged();
			emit failed(tr("%1 henüz mesajlaşmayı açmamış (uygulaması güncel değil ya da hiç giriş yapmamış).").arg(to));
			return;
		}
		const QByteArray plain = QJsonDocument(QJsonObject { { "t", text }, { "at", double(it->at) } }).toJson(QJsonDocument::Compact);
		const QByteArray ad = aad(me(), to, mid);
		const QJsonArray to_items = encryptFor(to.toLower(), theirs, plain, ad);
		const QJsonArray self_items = encryptFor(me(), keys.value(me()), plain, ad);
		server->call(QStringLiteral("msg_send"), { { "to", to }, { "mid", mid }, { "from_device", device_id },
			{ "items", to_items }, { "self_items", self_items } },
			[this, mid](const QJsonObject &r, const QString &error) {
				auto it = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
				if(it == messages.end())
					return;
				if(!error.isEmpty())
				{
					it->failed = true;
					emit failed(error);
				}
				else
				{
					it->id = (qint64)r.value("id").toDouble();
					it->sat = (qint64)r.value("at").toDouble();
				}
				save();
				emit chatChanged();
			});
	});
}

// A group message: the members and their keys read again right before, one copy per device of every
// other member (to: that member) and of my other devices (to: me), all in one msg_send with `group`.
// Each copy is sealed exactly like a direct message's (the phone app reads the same).
void MessageCenter::sendGroup(const QString &mid, bool retried)
{
	server->refreshGroups([this, mid, retried]() { sendGroupNow(mid, retried); });
}

void MessageCenter::sendGroupNow(const QString &mid, bool retried)
{
	auto it = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
	if(it == messages.end() || !server)
		return;
	const int group = it->group;
	const QStringList members = groupMembers(group);
	auto fail = [this, mid](const QString &why) {
		auto it = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
		if(it != messages.end())
			it->failed = true;
		emit chatChanged();
		emit failed(why);
	};
	if(members.isEmpty())
	{
		fail(tr("Grupta senden başka kimse yok."));
		return;
	}
	loadKeys(QStringList(members) << me(), [this, group, members, mid, fail, retried](bool) {
		auto it = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
		if(it == messages.end())
			return;
		const QByteArray plain = QJsonDocument(QJsonObject { { "t", it->text }, { "at", double(it->at) } }).toJson(QJsonDocument::Compact);
		QJsonArray items;
		for(const QString &member : members)
		{
			// The server takes at most 8 devices of a user
			const QList<DeviceKey> devices = keys.value(member.toLower()).mid(0, 8);
			for(const QJsonValue &v : encryptFor(member.toLower(), devices, plain, aad(me(), member, mid)))
			{
				QJsonObject item = v.toObject();
				item.insert("user", member);
				items.append(item);
			}
		}
		if(items.isEmpty())
		{
			fail(tr("Gruptakilerin hiçbiri henüz mesajlaşmayı açmamış."));
			return;
		}
		const QJsonArray self_items = encryptFor(me(), keys.value(me()), plain, aad(me(), me(), mid));
		server->call(QStringLiteral("msg_send"), { { "group", group }, { "mid", mid }, { "from_device", device_id },
			{ "items", items }, { "self_items", self_items } },
			[this, mid, retried](const QJsonObject &r, const QString &error) {
				auto it = std::find_if(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
				if(it == messages.end())
					return;
				const int status = r.value("http_status").toInt();
				// Someone left meanwhile (not_member) or a member's devices changed (409): once more with the
				// members and keys read again, with the same id (the server keeps a message only once)
				if(!error.isEmpty() && !retried && (r.contains("not_member") || status == 409))
				{
					qCInfo(ovioraMessages) << "group msg_send refused with" << status << "- trying once more";
					sendGroup(mid, true);
					return;
				}
				if(!error.isEmpty())
				{
					it->failed = true;
					emit failed(error);
					// I am no longer in it: the groups again
					if(status == 403)
						server->refresh();
				}
				else
				{
					it->id = (qint64)r.value("id").toDouble();
					it->sat = (qint64)r.value("at").toDouble();
				}
				save();
				emit chatChanged();
			});
	});
}

bool MessageCenter::decryptOne(const QJsonObject &m, Message *out)
{
	const QString from = m.value("from").toString();
	const QString to = m.value("to").toString();
	const QString from_device = m.value("from_device").toString();
	const QString mid = m.value("mid").toString();
	QByteArray sender_pub;
	for(const DeviceKey &k : keys.value(from.toLower()))
		if(k.device == from_device)
			sender_pub = k.pub;
	if(sender_pub.size() != CHIAKI_E2E_KEY_SIZE)
		return false;
	const QByteArray eph = b64(m.value("eph").toString()), nonce = b64(m.value("nonce").toString()), ct = b64(m.value("ct").toString());
	if(eph.size() != CHIAKI_E2E_KEY_SIZE || nonce.size() != CHIAKI_E2E_NONCE_SIZE || ct.size() < CHIAKI_E2E_TAG_SIZE)
		return false;
	const QByteArray ad = aad(from, to, mid);
	QByteArray plain(ct.size() - CHIAKI_E2E_TAG_SIZE, 0);
	if(chiaki_e2e_decrypt(reinterpret_cast<const uint8_t *>(priv.constData()), reinterpret_cast<const uint8_t *>(sender_pub.constData()),
		reinterpret_cast<const uint8_t *>(eph.constData()), reinterpret_cast<const uint8_t *>(nonce.constData()),
		reinterpret_cast<const uint8_t *>(ad.constData()), size_t(ad.size()),
		reinterpret_cast<const uint8_t *>(ct.constData()), size_t(ct.size()), reinterpret_cast<uint8_t *>(plain.data())) != CHIAKI_ERR_SUCCESS)
		return false;
	const QJsonObject body = QJsonDocument::fromJson(plain).object();
	// A group copy is sealed like a direct one (sender | receiver | message id, the same as the phone
	// app); the group travels only as the plain `group` field
	const int group = m.value("group").toInt();
	out->id = (qint64)m.value("id").toDouble();
	out->mid = mid;
	out->mine = from.toLower() == me();
	out->group = group;
	out->peer = group ? from : (out->mine ? to : from);
	out->text = body.value("t").toString().left(MAX_TEXT);
	out->sat = (qint64)m.value("at").toDouble();
	out->at = out->sat * 1000;
	return true;
}

QString MessageCenter::convKey(int group, const QString &peer)
{
	return group ? QStringLiteral("#%1").arg(group) : peer.toLower();
}

QString MessageCenter::openKey() const
{
	return chat_group ? convKey(chat_group, QString()) : (chat_with.isEmpty() ? QString() : chat_with.toLower());
}

void MessageCenter::recomputeUnread()
{
	QHash<QString, int> direct;
	QHash<int, int> groups;
	const QString open = openKey();
	for(const Message &m : messages)
	{
		if(m.mine)
			continue;
		const QString key = convKey(m.group, m.peer);
		if(key == open || m.sat <= my_read.value(key))
			continue;
		if(m.group)
			groups[m.group]++;
		else
			direct[m.peer.toLower()]++;
	}
	if(direct == unread_count && groups == group_unread)
		return;
	unread_count = direct;
	group_unread = groups;
	emit unreadChanged();
}

void MessageCenter::markOpenRead()
{
	const QString key = openKey();
	if(key.isEmpty())
		return;
	qint64 newest = 0;
	for(const Message &m : messages)
		if(!m.mine && convKey(m.group, m.peer) == key)
			newest = qMax(newest, m.sat);
	if(newest > my_read.value(key))
	{
		my_read.insert(key, newest);
		save();
	}
	if(newest <= read_sent.value(key) || !server || !server->signedIn() || (server && server->testGroups()))
		return;
	read_sent.insert(key, newest);
	QJsonObject body { { "upto", double(newest) } };
	if(chat_group)
		body.insert("group", chat_group);
	else
		body.insert("to", chat_with);
	server->call(QStringLiteral("msg_read"), body, [](const QJsonObject &, const QString &error) {
		if(!error.isEmpty())
			qCInfo(ovioraMessages) << "msg_read failed:" << error;
	});
}

bool MessageCenter::takeState(const QJsonObject &r)
{
	bool changed = false;
	if(r.contains("reads_last"))
		reads_last = qMax(reads_last, (qint64)r.value("reads_last").toDouble());
	for(const QJsonValue &v : r.value("reads").toArray())
	{
		const QJsonObject o = v.toObject();
		const QString reader = o.value("reader").toString().toLower();
		const int group = o.value("group").toInt();
		const qint64 upto = (qint64)o.value("upto").toDouble();
		if(reader == me())
		{
			// Read on another of my devices (or this one earlier)
			const QString key = group ? convKey(group, QString()) : o.value("peer").toString().toLower();
			if(upto > my_read.value(key))
			{
				my_read.insert(key, upto);
				changed = true;
			}
			read_sent.insert(key, qMax(read_sent.value(key), upto));
		}
		else if(group)
		{
			qint64 &mark = group_read[group][reader];
			if(upto > mark) { mark = upto; changed = true; }
		}
		else if(upto > peer_read.value(reader))
		{
			peer_read.insert(reader, upto);
			changed = true;
		}
	}
	if(r.contains("typing"))
	{
		QHash<QString, qint64> now_typing;
		const qint64 until = QDateTime::currentMSecsSinceEpoch() + 7000;
		for(const QJsonValue &v : r.value("typing").toArray())
		{
			const QJsonObject o = v.toObject();
			const int group = o.value("group").toInt();
			const QString user = o.value("user").toString();
			now_typing.insert(group ? QStringLiteral("#%1|%2").arg(group).arg(user) : user.toLower(), until);
		}
		if(QSet<QString>(now_typing.keyBegin(), now_typing.keyEnd()) != QSet<QString>(typing_until.keyBegin(), typing_until.keyEnd()))
		{
			typing_until = now_typing;
			emit typingChanged();
		}
		else
			typing_until = now_typing;
		if(!typing_until.isEmpty() && !typing_timer.isActive())
			typing_timer.start();
	}
	return changed;
}

void MessageCenter::fetch()
{
	if(server && server->signedIn() && !key_published)
	{
		publishKey();	// failed before (offline at the start): nothing can come until it is on the server
		return;
	}
	if(!server || !server->signedIn() || !key_published || fetching)
		return;
	fetching = true;
	const quint64 gen = session_gen;
	server->call(QStringLiteral("msg_fetch"), { { "device", device_id }, { "since", double(last_id) }, { "reads_since", double(reads_last) } },
		[this, gen](const QJsonObject &r, const QString &error) {
			if(gen != session_gen)
				return;
			if(!error.isEmpty())
			{
				qCInfo(ovioraMessages) << "msg_fetch failed:" << error;
				fetching = false;
				return;
			}
			if(takeState(r))
			{
				recomputeUnread();
				save();
				emit chatChanged();
			}
			const QJsonArray list = r.value("messages").toArray();
			if(!list.isEmpty())
				qCInfo(ovioraMessages) << "msg_fetch ok, since" << last_id << "new" << list.size();
			if(list.isEmpty())
			{
				fetching = false;
				return;
			}
			// Unknown senders' keys first, then all of them in order
			QStringList need;
			bool unknown_group = false;
			for(const QJsonValue &v : list)
			{
				const QString from = v.toObject().value("from").toString().toLower();
				if(!need.contains(from))
					need << from;
				const int group = v.toObject().value("group").toInt();
				if(group && server->group(group).isEmpty())
					unknown_group = true;
			}
			auto take = [this, gen, list, need]() {
				loadKeys(need, [this, gen, list](bool ok) {
					if(gen != session_gen)
						return;
					// Without the senders' keys every copy would be kept as unreadable for good: this page
					// waits for the next round instead (the cursor stays)
					if(ok)
						takeFetched(list);
					fetching = false;
				});
			};
			// A group I was just added to: the groups once more first (copies of groups I left are dropped
			// then). Only with a groups view that really came: otherwise they would be dropped as a left
			// group's, so this page waits for the next round
			if(unknown_group)
			{
				const int seen = groups_seen;
				server->refreshGroups([this, gen, take, seen]() {
					if(gen != session_gen)
						return;
					if(groups_seen == seen)
					{
						fetching = false;
						return;
					}
					take();
				});
			}
			else
				take();
		});
}

void MessageCenter::takeFetched(const QJsonArray &list)
{
	const qint64 prev_last_id = last_id;
	bool changed = false;
	for(const QJsonValue &v : list)
	{
		const QJsonObject m = v.toObject();
		last_id = qMax(last_id, (qint64)m.value("id").toDouble());
		const QString mid = m.value("mid").toString();
		const bool known = std::any_of(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
		if(known)
			continue;	// my own message, already here
		// A copy for a group I have just left or that was deleted
		const int group = m.value("group").toInt();
		if(group && server && server->groupsLoaded() && server->group(group).isEmpty())
			continue;
		Message msg;
		if(!decryptOne(m, &msg))
		{
			msg.id = (qint64)m.value("id").toDouble();
			msg.mid = mid;
			msg.peer = m.value("from").toString();
			msg.group = group;
			msg.text = tr("[Bu mesaj açılamadı: gönderenin anahtarı değişmiş olabilir]");
			msg.at = (qint64)m.value("at").toDouble() * 1000;
			msg.failed = true;
		}
		messages.append(msg);
		changed = true;
		if(!msg.mine)
		{
			// The word came: "yazıyor…" ends
			if(typing_until.remove(msg.group ? QStringLiteral("#%1|%2").arg(msg.group).arg(msg.peer) : msg.peer.toLower()))
				emit typingChanged();
			// Already read on another of my devices: no toast
			const QString key = convKey(msg.group, msg.peer);
			if(key != openKey() && msg.sat > my_read.value(key))
				emit incoming(msg.peer, msg.text.left(80), msg.group, msg.group && server ? server->group(msg.group).value("name").toString() : QString());
		}
	}
	if(!changed)
	{
		if(last_id != prev_last_id)
			save();
		return;
	}
	std::sort(messages.begin(), messages.end(), [](const Message &a, const Message &b) { return a.at < b.at; });
	markOpenRead();
	recomputeUnread();
	save();
	emit chatChanged();
}

void MessageCenter::openChat(const QString &friend_name)
{
	if(chat_with != friend_name || chat_group)
		stopTyping();
	chat_with = friend_name;
	chat_group = 0;
	poll_timer.setInterval(chat_with.isEmpty() ? 10000 : 2000);
	if(!chat_with.isEmpty())
	{
		markOpenRead();
		if(server && server->signedIn())
		{
			loadKeys({ chat_with, me() }, nullptr);
			fetch();
		}
	}
	recomputeUnread();
	emit chatChanged();
	emit typingChanged();
}

void MessageCenter::openGroupChat(int group_id)
{
	if(chat_group != group_id || !chat_with.isEmpty())
		stopTyping();
	chat_with.clear();
	chat_group = group_id;
	poll_timer.setInterval(chat_group ? 2000 : 10000);
	if(chat_group)
	{
		markOpenRead();
		if(server && server->signedIn())
		{
			loadKeys(groupMembers(chat_group) << me(), nullptr);
			fetch();
		}
	}
	recomputeUnread();
	emit chatChanged();
	emit typingChanged();
}

void MessageCenter::typing()
{
	const QString key = openKey();
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if(key.isEmpty() || !server || !server->signedIn() || server->testGroups() || (key == typing_sent_to && now - typing_sent_at < 3500))
		return;
	typing_sent_to = key;
	typing_sent_at = now;
	QJsonObject body;
	if(chat_group)
		body.insert("group", chat_group);
	else
		body.insert("to", chat_with);
	server->call(QStringLiteral("msg_typing"), body, [](const QJsonObject &, const QString &) {});
}

void MessageCenter::stopTyping()
{
	if(typing_sent_to.isEmpty() || QDateTime::currentMSecsSinceEpoch() - typing_sent_at > 6000 || !server || !server->signedIn())
	{
		typing_sent_to.clear();
		return;
	}
	QJsonObject body { { "stop", true } };
	if(typing_sent_to.startsWith(QLatin1Char('#')))
		body.insert("group", typing_sent_to.mid(1).toInt());
	else
		body.insert("to", typing_sent_to);
	typing_sent_to.clear();
	server->call(QStringLiteral("msg_typing"), body, [](const QJsonObject &, const QString &) {});
}

QStringList MessageCenter::chatTyping() const
{
	QStringList out;
	if(chat_group)
	{
		const QString prefix = QStringLiteral("#%1|").arg(chat_group);
		for(auto it = typing_until.constBegin(); it != typing_until.constEnd(); ++it)
			if(it.key().startsWith(prefix))
				out << it.key().mid(prefix.size());
	}
	else if(!chat_with.isEmpty() && typing_until.contains(chat_with.toLower()))
		out << chat_with;
	return out;
}

QVariantMap MessageCenter::lastMessages() const
{
	QVariantMap out;
	for(const Message &m : messages)	// oldest first: the newest one stays
		out.insert(convKey(m.group, m.peer), QVariantMap { { "text", m.text }, { "at", m.at }, { "mine", m.mine }, { "failed", m.failed } });
	return out;
}

QStringList MessageCenter::groupMembers(int group_id) const
{
	QStringList out;
	if(!server)
		return out;
	for(const QVariant &v : server->group(group_id).value("members").toList())
	{
		const QVariantMap member = v.toMap();
		if(!member.value("me").toBool() && member.value("name").toString().toLower() != me())
			out << member.value("name").toString();
	}
	return out;
}

QString MessageCenter::chatTitle() const
{
	if(chat_group)
		return server ? server->group(chat_group).value("name").toString() : QString();
	return chat_with;
}

QVariantList MessageCenter::chat() const
{
	QVariantList out;
	for(const Message &m : messages)
	{
		if(chat_group ? m.group != chat_group : (m.group || m.peer.compare(chat_with, Qt::CaseInsensitive) != 0))
			continue;
		QVariantMap item { { "id", m.id }, { "mid", m.mid }, { "mine", m.mine }, { "text", m.text }, { "at", m.at }, { "failed", m.failed },
			{ "sending", m.mine && m.id == 0 && !m.failed }, { "from", m.group && !m.mine ? m.peer : QString() } };
		if(m.mine && m.sat > 0)
		{
			if(m.group)
			{
				int seen = 0;
				for(const qint64 mark : group_read.value(m.group))
					seen += mark >= m.sat;
				item.insert("seenBy", seen);
				item.insert("seenOf", int(groupMembers(m.group).size()));
			}
			else
				item.insert("read", peer_read.value(m.peer.toLower()) >= m.sat);
		}
		out.append(item);
	}
	return out;
}

bool MessageCenter::chatKeyChanged() const
{
	if(chat_group)
		return !chatKeyChangedNames().isEmpty();
	return key_changed.value(chat_with.toLower());
}

QStringList MessageCenter::chatKeyChangedNames() const
{
	QStringList out;
	if(chat_group)
		for(const QString &member : groupMembers(chat_group))
			if(key_changed.value(member.toLower()))
				out << member;
	return out;
}

QVariantMap MessageCenter::unread() const
{
	QVariantMap out;
	for(auto it = unread_count.constBegin(); it != unread_count.constEnd(); ++it)
		out.insert(it.key(), it.value());
	return out;
}

QVariantMap MessageCenter::groupUnread() const
{
	QVariantMap out;
	for(auto it = group_unread.constBegin(); it != group_unread.constEnd(); ++it)
		out.insert(QString::number(it.key()), it.value());
	return out;
}

int MessageCenter::unreadTotal() const
{
	int n = 0;
	for(int c : unread_count)
		n += c;
	for(int c : group_unread)
		n += c;
	return n;
}

QString MessageCenter::safetyCode(const QString &friend_name) const
{
	const QList<DeviceKey> theirs = keys.value(friend_name.toLower());
	if(theirs.isEmpty() || pub.isEmpty())
		return QString();
	// The same code on both sides: each user's device keys (sorted) hashed, then the two hashes in order
	auto user_hash = [](QList<DeviceKey> devices, const QByteArray &also) {
		QList<QByteArray> sorted;
		for(const DeviceKey &k : devices)
			sorted << k.pub;
		if(!also.isEmpty() && !sorted.contains(also))
			sorted << also;
		std::sort(sorted.begin(), sorted.end());
		return QCryptographicHash::hash(sorted.join(), QCryptographicHash::Sha256);
	};
	const QByteArray mine = user_hash(keys.value(me()), pub);
	const QByteArray other = user_hash(theirs, QByteArray());
	uint8_t fp[CHIAKI_E2E_FINGERPRINT_SIZE];
	chiaki_e2e_fingerprint(reinterpret_cast<const uint8_t *>(mine.constData()), reinterpret_cast<const uint8_t *>(other.constData()), fp);
	QStringList groups;
	for(int i = 0; i < 12; i++)
	{
		const uint32_t v = (uint32_t(fp[i * 2]) << 8 | fp[i * 2 + 1]) % 100000;
		groups << QStringLiteral("%1").arg(v, 5, 10, QLatin1Char('0'));
	}
	return groups.join(QLatin1Char(' '));
}

void MessageCenter::confirmKeys(const QString &friend_name)
{
	const QString user = friend_name.toLower();
	for(const DeviceKey &k : keys.value(user))
		trusted.insert(user + QLatin1Char('|') + k.device, k.pub);
	key_changed.insert(user, false);
	save();
	emit chatChanged();
}

void MessageCenter::flushSaves()
{
	std::unique_lock<std::mutex> lock(save_mutex);
	save_cv.wait(lock, [this]() { return !save_pending && !save_writing; });
}

// Local history and trusted keys, DPAPI-encrypted as a whole (per Windows user)
void MessageCenter::load()
{
	flushSaves();
	QFile f(store_path());
	if(!f.open(QIODevice::ReadOnly))
		return;
	const QString plain = SecretStore::unprotect(QString::fromLatin1(f.readAll()));
	const QJsonObject all = QJsonDocument::fromJson(plain.toUtf8()).object();
	if(all.value("user").toString() != me())
		return;	// another account's
	last_id = (qint64)all.value("last").toDouble();
	const QJsonObject t = all.value("trusted").toObject();
	for(auto it = t.constBegin(); it != t.constEnd(); ++it)
		trusted.insert(it.key(), b64(it.value().toString()));
	messages.clear();
	// Direct conversations in "messages" (the old shape), groups' next to them in "group_messages"
	for(const char *list : { "messages", "group_messages" })
		for(const QJsonValue &v : all.value(list).toArray())
		{
			const QJsonObject o = v.toObject();
			Message m;
			m.id = (qint64)o.value("id").toDouble();
			m.mid = o.value("mid").toString();
			m.peer = o.value("peer").toString();
			m.group = o.value("group").toInt();
			m.mine = o.value("mine").toBool();
			m.text = o.value("text").toString();
			m.at = (qint64)o.value("at").toDouble();
			// Before read marks only the time was kept: a received message's is the server's
			m.sat = o.contains("sat") ? (qint64)o.value("sat").toDouble() : (m.mine ? 0 : m.at / 1000);
			m.failed = o.value("failed").toBool();
			// Closed while it was being sent: shown as not sent, not as sending forever
			if(m.mine && m.id == 0)
				m.failed = true;
			messages.append(m);
		}
	std::sort(messages.begin(), messages.end(), [](const Message &a, const Message &b) { return a.at < b.at; });
	const QJsonObject marks = all.value("reads").toObject();
	if(all.contains("reads"))
	{
		reads_last = (qint64)marks.value("last").toDouble();
		for(const char *list : { "mine", "sent", "peers" })
		{
			const QJsonObject o = marks.value(list).toObject();
			QHash<QString, qint64> &into = list[0] == 'm' ? my_read : list[0] == 's' ? read_sent : peer_read;
			for(auto it = o.constBegin(); it != o.constEnd(); ++it)
				into.insert(it.key(), (qint64)it.value().toDouble());
		}
		const QJsonObject g = marks.value("groups").toObject();
		for(auto it = g.constBegin(); it != g.constEnd(); ++it)
		{
			const QJsonObject members = it.value().toObject();
			for(auto m = members.constBegin(); m != members.constEnd(); ++m)
				group_read[it.key().toInt()].insert(m.key(), (qint64)m.value().toDouble());
		}
	}
	else
	{
		// History from before read marks: all of it counts as read (unread counts were never kept)
		for(const Message &m : messages)
			if(!m.mine)
				my_read.insert(convKey(m.group, m.peer), qMax(my_read.value(convKey(m.group, m.peer)), m.sat));
	}
	recomputeUnread();
	qCInfo(ovioraMessages) << "History loaded:" << messages.size() << "messages, last id" << last_id;
}

void MessageCenter::save()
{
	if(me().isEmpty())
		return;
	// The newest KEEP_PER_FRIEND of each conversation (a group's counts as one)
	QHash<QString, int> per_peer;
	QJsonArray list, group_list;
	for(int i = messages.size() - 1; i >= 0; i--)
	{
		const Message &m = messages.at(i);
		if(++per_peer[m.group ? QStringLiteral("#%1").arg(m.group) : m.peer.toLower()] > KEEP_PER_FRIEND)
			continue;
		QJsonObject o { { "id", double(m.id) }, { "mid", m.mid }, { "peer", m.peer }, { "mine", m.mine },
			{ "text", m.text }, { "at", double(m.at) }, { "sat", double(m.sat) }, { "failed", m.failed } };
		if(m.group)
		{
			o.insert("group", m.group);
			group_list.prepend(o);
		}
		else
			list.prepend(o);
	}
	QJsonObject t;
	for(auto it = trusted.constBegin(); it != trusted.constEnd(); ++it)
		t.insert(it.key(), to_b64(it.value()));
	auto marks = [](const QHash<QString, qint64> &h) {
		QJsonObject o;
		for(auto it = h.constBegin(); it != h.constEnd(); ++it)
			o.insert(it.key(), double(it.value()));
		return o;
	};
	QJsonObject groups;
	for(auto it = group_read.constBegin(); it != group_read.constEnd(); ++it)
		groups.insert(QString::number(it.key()), marks(it.value()));
	const QJsonObject reads { { "last", double(reads_last) }, { "mine", marks(my_read) }, { "sent", marks(read_sent) },
		{ "peers", marks(peer_read) }, { "groups", groups } };
	const QJsonObject all { { "user", me() }, { "last", double(last_id) }, { "trusted", t }, { "messages", list },
		{ "group_messages", group_list }, { "reads", reads } };
	const QByteArray plain = QJsonDocument(all).toJson(QJsonDocument::Compact);
	{
		std::lock_guard<std::mutex> lock(save_mutex);
		pending_save_path = store_path();
		pending_save_plain = plain;
		save_pending = true;
	}
	save_cv.notify_all();
}

// --test-groups: a made-up conversation in the first made-up group and one with a friend, with unread
// counts, for screenshots. Signed out: nothing is saved (no user) and nothing can be sent.
void MessageCenter::fillTestMessages()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	qint64 id = 0;
	auto add = [&](int group, const QString &peer, bool mine, const QString &text, int minutes_ago) {
		Message m;
		m.id = ++id;
		m.mid = QStringLiteral("test-%1").arg(id);
		m.peer = peer;
		m.group = group;
		m.mine = mine;
		m.text = text;
		m.at = now - qint64(minutes_ago) * 60000;
		m.sat = m.at / 1000;
		messages.append(m);
	};
	add(-1, QStringLiteral("ayse"), false, QStringLiteral("Bu akşam kim var? 21:00 gibi açarım."), 42);
	add(-1, QStringLiteral("deneme"), true, QStringLiteral("Ben varım. GTA mı, Astro Bot mu?"), 38);
	add(-1, QStringLiteral("mehmet"), false, QStringLiteral("GTA olur, benim PS5'te zaten açık."), 31);
	add(-1, QStringLiteral("can"), false, QStringLiteral("Ben biraz geç katılırım, yemekteyim."), 12);
	add(-1, QStringLiteral("ayse"), false, QStringLiteral("Tamam, Salon PS5'i de paylaştım."), 4);
	add(0, QStringLiteral("ayse"), false, QStringLiteral("Selam! Akşam bakalım mı?"), 50);
	add(0, QStringLiteral("ayse"), true, QStringLiteral("Olur, 9 gibi yazarım."), 49);
	// The two newest in the group and ayse's message unread; ayse saw my answer, two of the group my question
	my_read.insert(convKey(-1, QString()), (now - 31 * 60000) / 1000);
	peer_read.insert(QStringLiteral("ayse"), now / 1000);
	group_read[-1].insert(QStringLiteral("ayse"), now / 1000);
	group_read[-1].insert(QStringLiteral("mehmet"), now / 1000);
	add(0, QStringLiteral("ayse"), false, QStringLiteral("Süper, görüşürüz."), 2);
	my_read.insert(QStringLiteral("ayse"), (now - 49 * 60000) / 1000);
	recomputeUnread();
}
