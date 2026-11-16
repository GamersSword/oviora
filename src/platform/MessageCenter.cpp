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
	poll_timer.setInterval(15000);
	connect(&poll_timer, &QTimer::timeout, this, &MessageCenter::fetch);
	connect(server, &OvioraServer::changed, this, &MessageCenter::onLogin);
	takeOverOldData();
	onLogin();
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
		messages.clear();
		unread_count.clear();
		chat_with.clear();
		key_published = false;
		last_id = 0;
		emit chatChanged();
		emit unreadChanged();
		emit readyChanged();
		return;
	}
	if(key_published)
		return;
	ensureKey();
	load();
	publishKey();
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
	if(priv.isEmpty() || !server)
		return;
	server->call(QStringLiteral("e2e_key_put"), { { "device", device_id }, { "pub", to_b64(pub) } },
		[this](const QJsonObject &, const QString &error) {
			if(!error.isEmpty())
			{
				qCInfo(ovioraMessages) << "e2e_key_put failed:" << error;
				return;	// tried again at the next login
			}
			qCInfo(ovioraMessages) << "e2e_key_put ok, device" << device_id;
			key_published = true;
			emit readyChanged();
			fetch();
		});
}

void MessageCenter::loadKeys(const QStringList &users, std::function<void()> then)
{
	if(!server)
		return;
	QJsonArray list;
	for(const QString &u : users)
		list.append(u);
	server->call(QStringLiteral("e2e_keys"), { { "users", list } }, [this, then, asked = users.size()](const QJsonObject &r, const QString &error) {
		if(!error.isEmpty())
			qCInfo(ovioraMessages) << "e2e_keys failed:" << error;
		if(error.isEmpty())
		{
			const QJsonObject all = r.value("keys").toObject();
			qCInfo(ovioraMessages) << "e2e_keys ok," << all.size() << "of" << asked << "users have keys";
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
						trusted.insert(tkey, k.pub);
					else if(t.value() != k.pub)
						changed = true;
				}
				keys.insert(user, devices);
				if(user != me())
					key_changed.insert(user, changed);
			}
			save();
			emit chatChanged();
		}
		if(then)
			then();
	});
}

QByteArray MessageCenter::aad(const QString &from, const QString &to, const QString &mid) const
{
	return QStringLiteral("oviora-msg-v1|%1|%2|%3").arg(from.toLower(), to.toLower(), mid).toUtf8();
}

void MessageCenter::send(const QString &text_in)
{
	const QString text = text_in.trimmed().left(MAX_TEXT);
	const QString to = chat_with;
	if(text.isEmpty() || to.isEmpty() || !server || priv.isEmpty())
		return;
	Message m;
	m.mid = QString::fromLatin1(QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0'));
	m.peer = to;
	m.mine = true;
	m.text = text;
	m.at = QDateTime::currentMSecsSinceEpoch();
	messages.append(m);
	emit chatChanged();
	const QString mid = m.mid;

	// Fresh keys for both sides right before encrypting (new devices, changed keys)
	loadKeys({ to, me() }, [this, to, mid, text]() {
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
		auto encrypt_for = [&](const QString &recipient, const QList<DeviceKey> &devices) {
			QJsonArray items;
			const QByteArray ad = aad(me(), to, mid);
			for(const DeviceKey &k : devices)
			{
				if(recipient == me() && k.device == device_id)
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
		};
		const QJsonArray to_items = encrypt_for(to.toLower(), theirs);
		const QJsonArray self_items = encrypt_for(me(), keys.value(me()));
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
					it->id = (qint64)r.value("id").toDouble();
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
	out->id = (qint64)m.value("id").toDouble();
	out->mid = mid;
	out->mine = from.toLower() == me();
	out->peer = out->mine ? to : from;
	out->text = body.value("t").toString().left(MAX_TEXT);
	out->at = (qint64)m.value("at").toDouble() * 1000;
	return true;
}

void MessageCenter::fetch()
{
	if(!server || !server->signedIn() || !key_published || fetching)
		return;
	fetching = true;
	server->call(QStringLiteral("msg_fetch"), { { "device", device_id }, { "since", double(last_id) } },
		[this](const QJsonObject &r, const QString &error) {
			if(!error.isEmpty())
			{
				qCInfo(ovioraMessages) << "msg_fetch failed:" << error;
				fetching = false;
				return;
			}
			const QJsonArray list = r.value("messages").toArray();
			qCInfo(ovioraMessages) << "msg_fetch ok, since" << last_id << "new" << list.size();
			if(list.isEmpty())
			{
				fetching = false;
				return;
			}
			// Unknown senders' keys first, then all of them in order
			QStringList need;
			for(const QJsonValue &v : list)
			{
				const QString from = v.toObject().value("from").toString().toLower();
				if(!need.contains(from))
					need << from;
			}
			loadKeys(need, [this, list]() {
				takeFetched(list);
				fetching = false;
			});
		});
}

void MessageCenter::takeFetched(const QJsonArray &list)
{
	bool changed = false;
	for(const QJsonValue &v : list)
	{
		const QJsonObject m = v.toObject();
		last_id = qMax(last_id, (qint64)m.value("id").toDouble());
		const QString mid = m.value("mid").toString();
		const bool known = std::any_of(messages.begin(), messages.end(), [&](const Message &x) { return x.mid == mid; });
		if(known)
			continue;	// my own message, already here
		Message msg;
		if(!decryptOne(m, &msg))
		{
			msg.id = (qint64)m.value("id").toDouble();
			msg.mid = mid;
			msg.peer = m.value("from").toString();
			msg.text = tr("[Bu mesaj açılamadı: gönderenin anahtarı değişmiş olabilir]");
			msg.at = (qint64)m.value("at").toDouble() * 1000;
			msg.failed = true;
		}
		messages.append(msg);
		changed = true;
		if(!msg.mine)
		{
			if(msg.peer.compare(chat_with, Qt::CaseInsensitive) != 0)
			{
				unread_count[msg.peer.toLower()]++;
				emit incoming(msg.peer, msg.text.left(80));
			}
		}
	}
	if(!changed)
		return;
	std::sort(messages.begin(), messages.end(), [](const Message &a, const Message &b) { return a.at < b.at; });
	save();
	emit chatChanged();
	emit unreadChanged();
}

void MessageCenter::openChat(const QString &friend_name)
{
	chat_with = friend_name;
	poll_timer.setInterval(chat_with.isEmpty() ? 15000 : 3000);
	if(!chat_with.isEmpty())
	{
		unread_count.remove(chat_with.toLower());
		loadKeys({ chat_with, me() }, nullptr);
		fetch();
	}
	emit chatChanged();
	emit unreadChanged();
}

QVariantList MessageCenter::chat() const
{
	QVariantList out;
	for(const Message &m : messages)
	{
		if(m.peer.compare(chat_with, Qt::CaseInsensitive) != 0)
			continue;
		out.append(QVariantMap { { "id", m.id }, { "mine", m.mine }, { "text", m.text }, { "at", m.at }, { "failed", m.failed },
			{ "sending", m.mine && m.id == 0 && !m.failed } });
	}
	return out;
}

bool MessageCenter::chatKeyChanged() const
{
	return key_changed.value(chat_with.toLower());
}

QVariantMap MessageCenter::unread() const
{
	QVariantMap out;
	for(auto it = unread_count.constBegin(); it != unread_count.constEnd(); ++it)
		out.insert(it.key(), it.value());
	return out;
}

int MessageCenter::unreadTotal() const
{
	int n = 0;
	for(int c : unread_count)
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

// Local history and trusted keys, DPAPI-encrypted as a whole (per Windows user)
void MessageCenter::load()
{
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
	for(const QJsonValue &v : all.value("messages").toArray())
	{
		const QJsonObject o = v.toObject();
		Message m;
		m.id = (qint64)o.value("id").toDouble();
		m.mid = o.value("mid").toString();
		m.peer = o.value("peer").toString();
		m.mine = o.value("mine").toBool();
		m.text = o.value("text").toString();
		m.at = (qint64)o.value("at").toDouble();
		m.failed = o.value("failed").toBool();
		messages.append(m);
	}
	qCInfo(ovioraMessages) << "History loaded:" << messages.size() << "messages, last id" << last_id;
}

void MessageCenter::save()
{
	if(me().isEmpty())
		return;
	// The newest KEEP_PER_FRIEND of each conversation
	QHash<QString, int> per_peer;
	QJsonArray list;
	for(int i = messages.size() - 1; i >= 0; i--)
	{
		const Message &m = messages.at(i);
		if(++per_peer[m.peer.toLower()] > KEEP_PER_FRIEND)
			continue;
		list.prepend(QJsonObject { { "id", double(m.id) }, { "mid", m.mid }, { "peer", m.peer }, { "mine", m.mine },
			{ "text", m.text }, { "at", double(m.at) }, { "failed", m.failed } });
	}
	QJsonObject t;
	for(auto it = trusted.constBegin(); it != trusted.constEnd(); ++it)
		t.insert(it.key(), to_b64(it.value()));
	const QJsonObject all { { "user", me() }, { "last", double(last_id) }, { "trusted", t }, { "messages", list } };
	QDir().mkpath(QFileInfo(store_path()).absolutePath());
	QFile f(store_path());
	if(f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(SecretStore::protect(QString::fromUtf8(QJsonDocument(all).toJson(QJsonDocument::Compact))).toLatin1());
}
