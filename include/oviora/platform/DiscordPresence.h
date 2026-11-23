// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Oviora: Discord Rich Presence over Discord's local IPC (the named pipe \\.\pipe\discord-ipc-0..9),
// no SDK. While a PS5 stream is live the profile shows the game (with its cover when PSN "Oyun bilgisi"
// knows it; the member list then reads "<game> oynuyor"), whose console it is, that it runs through Oviora and
// since when; otherwise the activity is cleared. Discord not running is not an error: while there is something
// to show, the pipes are tried again every 30 s, quietly. Only the game name, the friend's display
// name the user already sees and the start time are sent; never an address or an account id.
#ifndef OVIORA_PLATFORM_DISCORDPRESENCE_H
#define OVIORA_PLATFORM_DISCORDPRESENCE_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class QLocalSocket;

class DiscordPresence : public QObject
{
	Q_OBJECT

public:
	explicit DiscordPresence(QObject *parent = nullptr);
	~DiscordPresence() override;

	/** Ayarlar > Genel > "Discord'da oynadığını göster"; off clears the activity and closes the pipe */
	void setEnabled(bool on);
	/**
	 * A stream is live. game: "" when unknown; icon: the game's cover on Sony's image server ("" when none);
	 * friendConsole: someone else's PS5, owner its owner's display name ("" when it must not be shown);
	 * startedMs: when the stream connected (epoch ms).
	 */
	void setPlaying(const QString &game, const QString &icon, bool friendConsole, const QString &owner, qint64 startedMs);
	/** No stream: the activity is cleared */
	void setIdle();

	/** The game name of a PS5/PS4 title id from the subtitle term lists (runtime/altyazi/terim), "" if none */
	static QString gameForTitleId(const QString &titleId);
	/** The Turkish genitive after a name: Ahmet'in, Ali'nin, Burak'ın (also the friends' presence line) */
	static QString turkishGenitive(const QString &name);
	/** --test-discord: a sample activity for ~8 s, then cleared. 0 = done, 2 = Discord not running, 1 = failed */
	static int runTest();

signals:
	/** READY received (handshake done) */
	void ready();
	/** Discord answered a SET_ACTIVITY; cleared: it was the empty activity */
	void activityAnswered(bool ok, bool cleared);
	/** None of the ten pipes answered */
	void unavailable();

private:
	enum class Link { Off, Connecting, Handshake, Ready };

	void connectNext();
	void startConnect();
	void dropLink(bool retry);
	void onReadyRead();
	void handleFrame(int op, const QByteArray &body);
	void send(int op, const QJsonObject &payload);
	/** Sends the wanted activity if it differs from what Discord has, within its rate limit */
	void flush();
	QJsonObject buildActivity() const;
	bool wanted() const { return enabled && playing; }

	QLocalSocket *socket = nullptr;
	Link link = Link::Off;
	int pipe_index = 0;
	QByteArray buffer;
	QTimer retry_timer, handshake_timer, flush_timer;

	bool enabled = true;
	bool playing = false;
	QString game, icon, owner;
	// status_display_type (the member list shows the game, not "Oviora"): dropped for good if Discord refuses it
	bool display_type = true;
	bool friend_console = false;
	qint64 started_ms = 0;

	// What Discord shows now (null: nothing), what was last logged, when it was last set
	QJsonObject sent;
	bool sent_valid = false;
	QString logged;
	qint64 last_send_ms = 0;
	QString pending_nonce;
	bool pending_cleared = false;

public:
	bool verbose = false;	// --test-discord: the READY reply is logged too
};

#endif // OVIORA_PLATFORM_DISCORDPRESENCE_H
