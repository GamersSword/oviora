#ifndef OVIORA_TRANSLATIONMEMORY_H
#define OVIORA_TRANSLATIONMEMORY_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include "oviora/platform/OvioraServer.h"

/**
 * Oviora: the shared translation memory on the Oviora server (tm_lookup / tm_submit / tm_variants /
 * tm_correct / tm_vote, see the Oviora server). Lines are keyed by the SHA-256 of their
 * normalized English (the same on every client) under a game key (the PS5 title id, else "genel").
 * Only the subtitle text is sent. Lookups and new lines are batched; answers are cached here for 7 days.
 */
class TranslationMemory : public QObject
{
	Q_OBJECT

public:
	struct Hit
	{
		qint64 id = 0;
		QString tr;
		QString source;		// google, llm, user
		int score = 0;
		int mine = 0;		// this user's vote on it: -1, 0, 1
	};

	explicit TranslationMemory(QObject *parent = nullptr);
	~TranslationMemory();

	void setServer(OvioraServer *server) { this->server = server; }
	void setEnabled(bool on) { enabled = on; }
	/** The PS5 title id of the running game (e.g. PPSA03016); "" = "genel" */
	void setGame(const QString &title_id);
	QString game() const { return game_key; }
	/** On and signed in */
	bool active() const;

	/** Normalized English → lower case hex SHA-256; "" when the line is too short to share */
	static QString lineSha(const QString &english);

	/** A translation known here for the line (from an earlier answer); nullptr if none */
	const Hit *cached(const QString &english) const;
	/** A new line was read: ask the server (batched); found() comes if it has one */
	void lookup(const QString &english);
	/** A machine translation was made: sent to the server once it is clear it has none */
	void offer(const QString &english, const QString &translation, const QString &source);

	/** "Düzelt": the candidates for a line; answers with variants() */
	void loadVariants(const QString &english);
	/** A correction from this user; answers with variants() (the new list) and found() */
	void correct(const QString &english, const QString &translation);
	/** Vote on a candidate: 1, -1, 0 = take back */
	void vote(qint64 id, int value, const QString &english);

	/** Saves the cache (end of a stream) */
	void save();

signals:
	/** The server has a translation for this line (sha as from lineSha) */
	void found(const QString &sha, const QString &translation);
	/** shown: the id shown for the line; list: id, tr, source, score, mine, by_me, pinned (best first) */
	void variants(const QString &sha, qint64 shown, const QVariantList &list, const QString &error);

private:
	struct Pending
	{
		QString en;
		QString tr;
		QString source;
	};

	QString cacheKey(const QString &sha) const { return game_key + QLatin1Char('|') + sha; }
	void sendLookups();
	void sendOffers();
	void takeVariants(const QString &sha, const QJsonObject &r, const QString &error);
	void load();
	bool paused(qint64 until) const;

	QPointer<OvioraServer> server;
	bool enabled = true;
	QString game_key = QStringLiteral("genel");

	QHash<QString, Hit> hits;			// game|sha → translation
	QHash<QString, qint64> hit_at;		// game|sha → when it was cached (ms)
	QSet<QString> misses;				// game|sha the server didn't have (this run)
	QSet<QString> asking;				// game|sha being looked up now
	QSet<QString> lookup_queue;			// sha
	QHash<QString, Pending> offer_waiting;	// game|sha: machine translation waiting for the lookup answer
	QHash<QString, Pending> offer_queue;	// sha → to submit
	QTimer lookup_timer;
	QTimer offer_timer;
	qint64 lookup_paused_until = 0;
	qint64 offer_paused_until = 0;
	bool loaded = false;
	bool dirty = false;
};

#endif // OVIORA_TRANSLATIONMEMORY_H
