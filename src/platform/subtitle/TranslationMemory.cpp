#include "oviora/RuntimePaths.h"
#include "TranslationMemory.h"
#include "oviora/platform/OvioraServer.h"
#include "oviora/Log.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

// Cached answers are kept this long, then asked again
static constexpr qint64 CACHE_MS = 7LL * 24 * 3600 * 1000;
// After a failure the server is left alone this long
static constexpr qint64 PAUSE_MS = 60 * 1000;
static constexpr int MAX_LOOKUP = 200;
static constexpr int MAX_SUBMIT = 100;
static constexpr int MAX_CACHE = 20000;

static qint64 now_ms() { return QDateTime::currentMSecsSinceEpoch(); }

// Into the subtitle log; line hashes and counts only, never the account token
static void log_info(const QString &text) { oviora::log::info("altyazi", text.toStdString()); }
static void log_warn(const QString &text) { oviora::log::warn("altyazi", text.toStdString()); }

static QString cache_path()
{
	// The new Oviora's own data folder: AppDataLocation is the old Oviora's (same organization and app name)
	return QString::fromStdWString((oviora::runtime::localDataDirectory() / L"ceviri_hafizasi.json").wstring());
}

TranslationMemory::TranslationMemory(QObject *parent)
	: QObject(parent)
{
	// Lines are collected for a moment so one request carries several
	lookup_timer.setSingleShot(true);
	lookup_timer.setInterval(300);
	connect(&lookup_timer, &QTimer::timeout, this, &TranslationMemory::sendLookups);
	offer_timer.setSingleShot(true);
	offer_timer.setInterval(2000);
	connect(&offer_timer, &QTimer::timeout, this, &TranslationMemory::sendOffers);
}

TranslationMemory::~TranslationMemory()
{
	save();
}

void TranslationMemory::setGame(const QString &title_id)
{
	// The server's normalization: a title id in capitals as it is, anything else "genel" here
	static const QRegularExpression title(QStringLiteral("^[A-Za-z]{4}\\d{5}$"));
	const QString key = title.match(title_id).hasMatch() ? title_id.toUpper() : QStringLiteral("genel");
	if(key == game_key)
		return;
	game_key = key;
	lookup_queue.clear();
	offer_queue.clear();
	offer_waiting.clear();
	asking.clear();
}

bool TranslationMemory::active() const
{
	return enabled && server && server->signedIn();
}

bool TranslationMemory::paused(qint64 until) const
{
	return now_ms() < until;
}

QString TranslationMemory::lineSha(const QString &english)
{
	// 1) Unicode lower case, 2) every run of characters that are not letters or digits becomes one space,
	// 3) trimmed, 4) SHA-256 of the UTF-8 bytes. Must match the server and the Android app.
	const QString lower = english.toLower();
	QString norm;
	norm.reserve(lower.size());
	bool space = false;
	for(int i = 0; i < lower.size(); i++)
	{
		const QChar c = lower.at(i);
		if(c.isHighSurrogate() && i + 1 < lower.size())
		{
			const char32_t cp = QChar::surrogateToUcs4(c, lower.at(i + 1));
			if(QChar::isLetterOrNumber(cp))
			{
				norm += c;
				norm += lower.at(++i);
				space = false;
				continue;
			}
			i++;
		}
		else if(c.isLetterOrNumber())
		{
			norm += c;
			space = false;
			continue;
		}
		if(!space && !norm.isEmpty())
			norm += QLatin1Char(' ');
		space = true;
	}
	norm = norm.trimmed();
	if(norm.size() < 2)
		return QString();
	return QString::fromLatin1(QCryptographicHash::hash(norm.toUtf8(), QCryptographicHash::Sha256).toHex());
}

const TranslationMemory::Hit *TranslationMemory::cached(const QString &english) const
{
	const_cast<TranslationMemory *>(this)->load();
	const QString sha = lineSha(english);
	if(sha.isEmpty())
		return nullptr;
	const QString key = cacheKey(sha);
	const auto it = hits.constFind(key);
	if(it == hits.constEnd() || now_ms() - hit_at.value(key) > CACHE_MS)
		return nullptr;
	return &it.value();
}

void TranslationMemory::lookup(const QString &english)
{
	if(!active())
	{
		static bool told = false;
		if(!told)
		{
			told = true;
			log_info(QStringLiteral("shared memory not used: on=%1 server=%2 signed in=%3").arg(enabled).arg(server != nullptr)
				.arg(server && server->signedIn()));
		}
		return;
	}
	load();
	const QString sha = lineSha(english);
	if(sha.isEmpty())
		return;
	const QString key = cacheKey(sha);
	if((hits.contains(key) && now_ms() - hit_at.value(key) <= CACHE_MS) || misses.contains(key) || asking.contains(key))
		return;
	lookup_queue.insert(sha);
	if(!lookup_timer.isActive())
		lookup_timer.start();
}

void TranslationMemory::sendLookups()
{
	if(lookup_queue.isEmpty() || !active())
		return;
	if(paused(lookup_paused_until))
	{
		lookup_queue.clear();
		return;
	}
	QJsonArray lines;
	QStringList sent;
	for(const QString &sha : std::as_const(lookup_queue))
	{
		if(sent.size() >= MAX_LOOKUP)
			break;
		lines.append(sha);
		sent << sha;
	}
	for(const QString &sha : std::as_const(sent))
	{
		lookup_queue.remove(sha);
		asking.insert(cacheKey(sha));
	}
	if(!lookup_queue.isEmpty())
		lookup_timer.start();
	const QString game = game_key;
	QElapsedTimer took;
	took.start();
	server->call(QStringLiteral("tm_lookup"), { { "game", game }, { "lines", lines } },
		[this, sent, game, took](const QJsonObject &r, const QString &error) {
			for(const QString &sha : sent)
				asking.remove(game + QLatin1Char('|') + sha);
			if(!error.isEmpty())
			{
				log_warn(QStringLiteral("shared memory lookup: %1 lines (game %2) failed in %3 ms: %4; paused 60 s")
					.arg(sent.size()).arg(game).arg(took.elapsed()).arg(error.left(160)));
				lookup_paused_until = now_ms() + PAUSE_MS;
				return;
			}
			log_info(QStringLiteral("shared memory lookup: %1 lines (game %2) -> %3 hits in %4 ms")
				.arg(sent.size()).arg(game).arg(r.value("hits").toObject().size()).arg(took.elapsed()));
			if(game != game_key)
				return;	// the game changed meanwhile
			const QJsonObject found_hits = r.value("hits").toObject();
			for(const QString &sha : sent)
			{
				const QString key = cacheKey(sha);
				const QJsonObject h = found_hits.value(sha).toObject();
				if(h.isEmpty())
				{
					misses.insert(key);
					// The machine translation made meanwhile goes to the server now
					const auto w = offer_waiting.constFind(key);
					if(w != offer_waiting.constEnd())
					{
						offer_queue.insert(sha, w.value());
						offer_waiting.erase(w);
						if(!offer_timer.isActive())
							offer_timer.start();
					}
					continue;
				}
				offer_waiting.remove(key);
				Hit hit;
				hit.id = (qint64)h.value("id").toDouble();
				hit.tr = h.value("tr").toString();
				hit.source = h.value("source").toString();
				hit.score = h.value("score").toInt();
				hit.mine = h.value("mine").toInt();
				if(hit.tr.isEmpty())
					continue;
				hits.insert(key, hit);
				hit_at.insert(key, now_ms());
				dirty = true;
				emit found(sha, hit.tr);
			}
		});
}

void TranslationMemory::offer(const QString &english, const QString &translation, const QString &source)
{
	if(!active() || translation.trimmed().isEmpty() || english.size() > 500 || translation.size() > 600)
		return;
	const QString sha = lineSha(english);
	if(sha.isEmpty())
		return;
	const QString key = cacheKey(sha);
	if(hits.contains(key))
		return;	// the server has one; its own is shown
	const Pending p { english, translation, source };
	if(misses.contains(key))
	{
		offer_queue.insert(sha, p);
		if(!offer_timer.isActive())
			offer_timer.start();
	}
	else
	{
		// Not answered yet: sent once the lookup says the server has none
		offer_waiting.insert(key, p);
		if(!asking.contains(key))
			lookup(english);
	}
}

void TranslationMemory::sendOffers()
{
	if(offer_queue.isEmpty() || !active())
		return;
	if(paused(offer_paused_until))
	{
		offer_queue.clear();
		return;
	}
	QJsonArray items;
	QStringList sent;
	for(auto it = offer_queue.constBegin(); it != offer_queue.constEnd() && sent.size() < MAX_SUBMIT; ++it)
	{
		items.append(QJsonObject { { "sha", it.key() }, { "en", it.value().en }, { "tr", it.value().tr },
			{ "source", it.value().source } });
		sent << it.key();
	}
	for(const QString &sha : std::as_const(sent))
		offer_queue.remove(sha);
	if(!offer_queue.isEmpty())
		offer_timer.start();
	// Test runs: OVIORA_TEST_TM_NO_SUBMIT=1 goes this far and sends nothing (no test lines in the shared memory)
	if(qEnvironmentVariableIntValue("OVIORA_TEST_TM_NO_SUBMIT") > 0)
	{
		log_info(QStringLiteral("shared memory submit: would submit %1 lines (game %2), not sent (test run)").arg(sent.size()).arg(game_key));
		return;
	}
	const int count = int(sent.size());
	const QString game = game_key;
	server->call(QStringLiteral("tm_submit"), { { "game", game_key }, { "items", items } },
		[this, count, game](const QJsonObject &, const QString &error) {
			if(!error.isEmpty())
			{
				log_warn(QStringLiteral("shared memory submit: %1 lines (game %2) failed: %3; paused 60 s").arg(count).arg(game).arg(error.left(160)));
				offer_paused_until = now_ms() + PAUSE_MS;	// e.g. the hourly limit: quietly later
				return;
			}
			log_info(QStringLiteral("shared memory submit: %1 lines (game %2) ok").arg(count).arg(game));
		});
}

void TranslationMemory::takeVariants(const QString &sha, const QJsonObject &r, const QString &error)
{
	log_info(QStringLiteral("shared memory variants for %1: %2").arg(sha.left(12),
		error.isEmpty() ? QStringLiteral("%1 candidates").arg(r.value("variants").toArray().size()) : QStringLiteral("error ") + error.left(160)));
	if(!error.isEmpty())
	{
		emit variants(sha, 0, QVariantList(), error);
		return;
	}
	const qint64 shown = (qint64)r.value("shown").toDouble();
	const QJsonArray list = r.value("variants").toArray();
	// The one shown becomes this line's translation here too
	for(const QJsonValue &v : list)
	{
		const QJsonObject o = v.toObject();
		if((qint64)o.value("id").toDouble() != shown)
			continue;
		Hit hit;
		hit.id = shown;
		hit.tr = o.value("tr").toString();
		hit.source = o.value("source").toString();
		hit.score = o.value("score").toInt();
		hit.mine = o.value("mine").toInt();
		const QString key = cacheKey(sha);
		const bool changed = hits.value(key).tr != hit.tr;
		hits.insert(key, hit);
		hit_at.insert(key, now_ms());
		misses.remove(key);
		dirty = true;
		if(changed && !hit.tr.isEmpty())
			emit found(sha, hit.tr);
	}
	emit variants(sha, shown, list.toVariantList(), QString());
}

void TranslationMemory::loadVariants(const QString &english)
{
	const QString sha = lineSha(english);
	if(sha.isEmpty() || !active())
	{
		emit variants(sha, 0, QVariantList(), active() ? tr("Bu satır paylaşılamayacak kadar kısa.") : tr("Oviora hesabına giriş yapılmamış."));
		return;
	}
	server->call(QStringLiteral("tm_variants"), { { "game", game_key }, { "sha", sha } },
		[this, sha](const QJsonObject &r, const QString &error) {
			// 404: nobody shared this line yet; the list is just empty (the new server client passes
			// the error body along, so it is told apart by the missing list, not by an empty reply)
			if(!error.isEmpty() && !r.contains(QStringLiteral("variants")) && error.contains(QStringLiteral("yok")))
			{
				emit variants(sha, 0, QVariantList(), QString());
				return;
			}
			takeVariants(sha, r, error);
		});
}

void TranslationMemory::correct(const QString &english, const QString &translation)
{
	const QString sha = lineSha(english);
	const QString text = translation.trimmed();
	if(sha.isEmpty() || text.isEmpty() || !active())
		return;
	server->call(QStringLiteral("tm_correct"), { { "game", game_key }, { "sha", sha }, { "tr", text.left(600) }, { "en", english.left(500) } },
		[this, sha](const QJsonObject &r, const QString &error) { takeVariants(sha, r, error); });
}

void TranslationMemory::vote(qint64 id, int value, const QString &english)
{
	if(!active() || id <= 0)
		return;
	server->call(QStringLiteral("tm_vote"), { { "id", id }, { "v", qBound(-1, value, 1) } },
		[this, english](const QJsonObject &, const QString &error) {
			log_info(QStringLiteral("shared memory vote: %1").arg(error.isEmpty() ? QStringLiteral("ok") : QStringLiteral("error ") + error.left(160)));
			if(!error.isEmpty())
			{
				emit variants(lineSha(english), 0, QVariantList(), error);
				return;
			}
			loadVariants(english);	// scores and the shown one may have changed
		});
}

void TranslationMemory::load()
{
	if(loaded)
		return;
	loaded = true;
	QFile f(cache_path());
	if(!f.open(QIODevice::ReadOnly))
		return;
	const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object();
	const qint64 now = now_ms();
	for(auto it = all.constBegin(); it != all.constEnd(); ++it)
	{
		const QJsonObject o = it.value().toObject();
		const qint64 at = (qint64)o.value("at").toDouble();
		if(now - at > CACHE_MS)
			continue;
		Hit h;
		h.id = (qint64)o.value("id").toDouble();
		h.tr = o.value("tr").toString();
		h.source = o.value("source").toString();
		h.score = o.value("score").toInt();
		h.mine = o.value("mine").toInt();
		if(h.tr.isEmpty())
			continue;
		hits.insert(it.key(), h);
		hit_at.insert(it.key(), at);
	}
}

void TranslationMemory::save()
{
	if(!dirty)
		return;
	dirty = false;
	// The newest are kept when there are too many
	QList<QPair<qint64, QString>> order;
	for(auto it = hit_at.constBegin(); it != hit_at.constEnd(); ++it)
		order.append({ it.value(), it.key() });
	std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	QJsonObject all;
	const qint64 now = now_ms();
	for(int i = 0; i < order.size() && i < MAX_CACHE; i++)
	{
		if(now - order[i].first > CACHE_MS)
			continue;
		const Hit &h = hits[order[i].second];
		all.insert(order[i].second, QJsonObject { { "id", h.id }, { "tr", h.tr }, { "source", h.source },
			{ "score", h.score }, { "mine", h.mine }, { "at", order[i].first } });
	}
	QDir().mkpath(QFileInfo(cache_path()).absolutePath());
	QFile f(cache_path());
	if(f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
}
