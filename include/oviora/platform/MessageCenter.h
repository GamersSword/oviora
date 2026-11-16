// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#ifndef OVIORA_PLATFORM_MESSAGECENTER_H
#define OVIORA_PLATFORM_MESSAGECENTER_H

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class OvioraServer;

/**
 * Oviora: end-to-end encrypted messages between friends (chiaki/e2e.h for the crypto).
 * This device has its own X25519 key; the private key never leaves it (stored DPAPI-encrypted), the server
 * only relays ciphertext: every message is encrypted for each of the friend's devices and for this user's
 * other devices. Friends' keys are trusted on first use; a changed key is shown as a warning and its
 * safety code can be compared.
 *
 * Server actions: e2e_key_put, e2e_keys, msg_send, msg_fetch. Shared with the old Oviora: same server,
 * same wire format, so both talk to each other.
 *
 * Storage (the new Oviora's own data folder, never the old program's): the device key in platform.ini
 * (group "mesaj"), the history and trusted keys in mesajlar.dat. On the first start the old Oviora's
 * device key and history are taken over (only read).
 */
class MessageCenter : public QObject
{
	Q_OBJECT
	/** Unread messages per friend: { name: count } */
	Q_PROPERTY(QVariantMap unread READ unread NOTIFY unreadChanged)
	Q_PROPERTY(int unreadTotal READ unreadTotal NOTIFY unreadChanged)
	/** The open conversation: name, and its messages (id, mine, text, at, failed, sending) oldest first */
	Q_PROPERTY(QString chatWith READ chatWith NOTIFY chatChanged)
	Q_PROPERTY(QVariantList chat READ chat NOTIFY chatChanged)
	/** The friend's key changed since we first saw it (until confirmed) */
	Q_PROPERTY(bool chatKeyChanged READ chatKeyChanged NOTIFY chatChanged)
	Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
	explicit MessageCenter(OvioraServer *server, QObject *parent = nullptr);

	QVariantMap unread() const;
	int unreadTotal() const;
	QString chatWith() const { return chat_with; }
	QVariantList chat() const;
	bool chatKeyChanged() const;
	bool ready() const { return key_published; }

	/** Opens a conversation (loads keys, marks read); "" closes it */
	Q_INVOKABLE void openChat(const QString &friend_name);
	Q_INVOKABLE void send(const QString &text);
	/** The safety code with this friend, as 12 groups of 5 digits; "" if the key is not known yet */
	Q_INVOKABLE QString safetyCode(const QString &friend_name) const;
	/** The user compared the code: the friend's current keys are trusted from now on */
	Q_INVOKABLE void confirmKeys(const QString &friend_name);
	/** Asks the server for new messages now */
	Q_INVOKABLE void fetch();

signals:
	void unreadChanged();
	void chatChanged();
	void readyChanged();
	/** A new message from a friend while its conversation is not open (for a toast) */
	void incoming(const QString &from, const QString &preview);
	void failed(const QString &text);

private:
	struct Message
	{
		qint64 id = 0;		// server id (0 while sending)
		QString mid;		// client message id
		QString peer;		// the friend
		bool mine = false;
		QString text;
		qint64 at = 0;		// ms
		bool failed = false;
	};
	struct DeviceKey
	{
		QString device;
		QByteArray pub;
	};

	void onLogin();
	void ensureKey();
	void publishKey();
	void loadKeys(const QStringList &users, std::function<void()> then);
	void takeFetched(const QJsonArray &list);
	bool decryptOne(const QJsonObject &m, Message *out);
	void load();
	void save();
	QByteArray aad(const QString &from, const QString &to, const QString &mid) const;
	QString me() const;

	QPointer<OvioraServer> server;
	QByteArray priv;			// this device's private key (memory only; DPAPI on disk)
	QByteArray pub;
	QString device_id;
	bool key_published = false;
	QHash<QString, QList<DeviceKey>> keys;	// user (lower case) -> devices
	QHash<QString, QByteArray> trusted;		// "user|device" -> first seen key
	QHash<QString, bool> key_changed;		// user -> a device key differs from the trusted one
	QList<Message> messages;
	QHash<QString, int> unread_count;
	QString chat_with;
	qint64 last_id = 0;
	bool fetching = false;
	QTimer poll_timer;
};

#endif // OVIORA_PLATFORM_MESSAGECENTER_H
