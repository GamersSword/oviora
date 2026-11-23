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
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class OvioraServer;

/**
 * Oviora: end-to-end encrypted messages between friends (chiaki/e2e.h for the crypto).
 * This device has its own X25519 key; the private key never leaves it (stored DPAPI-encrypted), the server
 * only relays ciphertext: every message is encrypted for each of the friend's devices and for this user's
 * other devices. Friends' keys are trusted on first use; a changed key is shown as a warning and its
 * safety code can be compared.
 *
 * Friend groups (OvioraServer::groups): a group message is encrypted the same way, once for every device
 * of every other member and for this user's other devices, and sent with `group`; copies that come back
 * with `group` belong to that group's conversation (keyed by its id), never to a direct one.
 *
 * Server actions: e2e_key_put, e2e_keys, msg_send, msg_fetch, msg_read, msg_typing. Shared with the old
 * Oviora: same server, same wire format, so both talk to each other.
 *
 * Read marks: how far each reader has read a conversation, as the server time of the newest message read
 * (msg_read; msg_fetch with reads_since brings the changes). A friend's mark shows "Görüldü" under my
 * messages, a group's how many have seen them; my own marks (from any of my devices) are what is unread,
 * so reading on one device clears the count on the others. "Yazıyor…" comes the same way (msg_typing).
 * Neither carries any content.
 *
 * Storage (the new Oviora's own data folder, never the old program's): the device key in platform.ini
 * (group "mesaj"), the history and trusted keys in mesajlar.dat. On the first start the old Oviora's
 * device key and history are taken over (only read). Group conversations are kept in the same file
 * under their own key ("group_messages"), so the direct history keeps its old shape.
 */
class MessageCenter : public QObject
{
	Q_OBJECT
	/** Unread messages per friend: { name: count } */
	Q_PROPERTY(QVariantMap unread READ unread NOTIFY unreadChanged)
	/** Unread messages per group: { "<group id>": count } */
	Q_PROPERTY(QVariantMap groupUnread READ groupUnread NOTIFY unreadChanged)
	/** Direct and group messages */
	Q_PROPERTY(int unreadTotal READ unreadTotal NOTIFY unreadChanged)
	/** The open conversation: the friend ("" for a group), and its messages (id, mine, text, at, failed,
	 *  sending; from: the sender in a group) oldest first */
	Q_PROPERTY(QString chatWith READ chatWith NOTIFY chatChanged)
	/** The open group's id, 0 for a direct conversation or none */
	Q_PROPERTY(int chatGroup READ chatGroup NOTIFY chatChanged)
	Q_PROPERTY(bool chatIsGroup READ chatIsGroup NOTIFY chatChanged)
	/** The friend's name or the group's name */
	Q_PROPERTY(QString chatTitle READ chatTitle NOTIFY chatChanged)
	Q_PROPERTY(QVariantList chat READ chat NOTIFY chatChanged)
	/** Who is typing anywhere: friend (lower case) or "#<group id>|member" */
	Q_PROPERTY(QStringList typingNow READ typingNow NOTIFY typingChanged)
	/** Who is typing in the open conversation right now */
	Q_PROPERTY(QStringList chatTyping READ chatTyping NOTIFY typingChanged)
	/** The newest message of each conversation: { friend (lower case) or "#<group id>": {text, at, mine, failed} } */
	Q_PROPERTY(QVariantMap lastMessages READ lastMessages NOTIFY chatChanged)
	/** The friend's key (in a group: a member's key) changed since we first saw it (until confirmed) */
	Q_PROPERTY(bool chatKeyChanged READ chatKeyChanged NOTIFY chatChanged)
	/** In a group: the members whose key changed */
	Q_PROPERTY(QStringList chatKeyChangedNames READ chatKeyChangedNames NOTIFY chatChanged)
	Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
	explicit MessageCenter(OvioraServer *server, QObject *parent = nullptr);
	~MessageCenter() override;

	QVariantMap unread() const;
	QVariantMap groupUnread() const;
	int unreadTotal() const;
	QString chatWith() const { return chat_with; }
	int chatGroup() const { return chat_group; }
	bool chatIsGroup() const { return chat_group != 0; }
	QString chatTitle() const;
	QVariantList chat() const;
	QStringList chatTyping() const;
	QStringList typingNow() const { return typing_until.keys(); }
	QVariantMap lastMessages() const;
	bool chatKeyChanged() const;
	QStringList chatKeyChangedNames() const;
	bool ready() const { return key_published; }

	/** Opens a conversation (loads keys, marks read); "" closes it */
	Q_INVOKABLE void openChat(const QString &friend_name);
	/** Opens a group's conversation (OvioraServer::groups id); 0 closes it */
	Q_INVOKABLE void openGroupChat(int group_id);
	Q_INVOKABLE void send(const QString &text);
	/** A message of mine that could not be sent: once more, as the newest */
	Q_INVOKABLE void resend(const QString &mid);
	/** A key was pressed in the open conversation's field (told to the others at most every few seconds) */
	Q_INVOKABLE void typing();
	/** The field was emptied or the message sent: "yazıyor…" ends at once */
	Q_INVOKABLE void stopTyping();
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
	void typingChanged();
	/** A new message while its conversation is not open (for a toast); group: its id and name, 0 for a direct one */
	void incoming(const QString &from, const QString &preview, int group, const QString &groupName);
	void failed(const QString &text);

private:
	struct Message
	{
		qint64 id = 0;		// server id (0 while sending)
		QString mid;		// client message id
		QString peer;		// the friend; in a group the sender
		int group = 0;		// the group's conversation, 0 for a direct one
		bool mine = false;
		QString text;
		qint64 at = 0;		// ms
		qint64 sat = 0;		// the server's time of it (s), what read marks compare with; 0 while sending
		bool failed = false;
	};
	struct DeviceKey
	{
		QString device;
		QByteArray pub;
		bool operator==(const DeviceKey &o) const { return device == o.device && pub == o.pub; }
	};

	void onLogin();
	void ensureKey();
	void publishKey();
	/** then(ok): ok is false when the server could not be asked (the keys known before stay) */
	void loadKeys(const QStringList &users, std::function<void(bool ok)> then);
	void takeFetched(const QJsonArray &list);
	/** One copy per device; user: the recipient (skips this device when it is me), ad: its AAD */
	QJsonArray encryptFor(const QString &user, const QList<DeviceKey> &devices, const QByteArray &plain, const QByteArray &ad) const;
	void sendDirect(const QString &mid);
	void sendGroup(const QString &mid, bool retried = false);
	void sendGroupNow(const QString &mid, bool retried);
	/** The members of a group other than me, from the server's groups view */
	QStringList groupMembers(int group_id) const;
	/** --test-groups: a made-up history and unread counts, signed out and only in memory */
	void fillTestMessages();
	bool decryptOne(const QJsonObject &m, Message *out);
	/** A conversation's key in the read marks: the friend (lower case) or "#<group id>" */
	static QString convKey(int group, const QString &peer);
	QString openKey() const;
	/** unread_count / group_unread from the messages and my read marks */
	void recomputeUnread();
	/** The open conversation is read up to its newest message: my mark, and the server told once */
	void markOpenRead();
	/** reads / typing of a msg_fetch reply; true when anything shown changed */
	bool takeState(const QJsonObject &r);
	void load();
	void save();
	QByteArray aad(const QString &from, const QString &to, const QString &mid) const;
	QString me() const;

	QPointer<OvioraServer> server;
	QByteArray priv;			// this device's private key (memory only; DPAPI on disk)
	QByteArray pub;
	QString device_id;
	bool key_published = false;
	bool publishing = false;
	quint64 session_gen = 0;	// bumped on logout so stale replies from a previous session are ignored
	QString loaded_for;		// the account whose history is in memory
	int groups_seen = 0;		// counts the groups views taken (OvioraServer::groupsTaken)
	QHash<QString, QList<DeviceKey>> keys;	// user (lower case) -> devices
	QHash<QString, QByteArray> trusted;		// "user|device" -> first seen key
	QHash<QString, bool> key_changed;		// user -> a device key differs from the trusted one
	QList<Message> messages;
	QHash<QString, int> unread_count;
	QHash<int, int> group_unread;
	QHash<QString, qint64> my_read;		// conversation key -> my read mark (any of my devices)
	QHash<QString, qint64> read_sent;	// conversation key -> the mark this device told the server
	QHash<QString, qint64> peer_read;	// friend (lower case) -> how far they read our conversation
	QHash<int, QHash<QString, qint64>> group_read;	// group -> member (lower case) -> mark
	qint64 reads_last = 0;			// msg_fetch reads_since
	QHash<QString, qint64> typing_until;	// friend, or "#<group>|member" -> local ms
	QTimer typing_timer;
	QString typing_sent_to;
	qint64 typing_sent_at = 0;
	QString chat_with;
	int chat_group = 0;
	qint64 last_id = 0;
	bool fetching = false;
	QTimer poll_timer;

	// Background writer so DPAPI protection and atomic disk saves do not block the UI thread
	std::mutex save_mutex;
	std::condition_variable save_cv;
	bool save_stop = false;
	bool save_pending = false;
	bool save_writing = false;	// the writer is between taking a snapshot and finishing the file
	/** Waits until every queued save is on disk (load() must not read an older file) */
	void flushSaves();
	QString pending_save_path;
	QByteArray pending_save_plain;
	std::thread save_thread;
};

#endif // OVIORA_PLATFORM_MESSAGECENTER_H
