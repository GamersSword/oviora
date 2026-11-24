// SPDX-License-Identifier: GPL-3.0-only
#include "oviora/platform/IzletController.h"

#include "oviora/platform/IzletPublisher.h"
#include "oviora/remoteplay/WatchRoute.h"
#include "oviora/platform/OvioraServer.h"
#include "oviora/Log.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>

#include <format>

IzletController::IzletController(OvioraServer *server, Hooks hooks, QObject *parent)
	: QObject(parent), server(server), hooks(std::move(hooks))
{
	poll_timer.setSingleShot(true);
	connect(&poll_timer, &QTimer::timeout, this, &IzletController::poll);
	connect(&live_timer, &QTimer::timeout, this, &IzletController::refreshLive);
	watch_timer.setSingleShot(true);
	connect(&watch_timer, &QTimer::timeout, this, &IzletController::watchPoll);
	live_timer.start(20000);
	QTimer::singleShot(4000, this, &IzletController::refreshLive);
	connect(server, &OvioraServer::accountChanged, this, &IzletController::refreshLive);
}

bool IzletController::canWatch() const
{
	return bool(hooks.connectWatch);
}

void IzletController::refreshLive()
{
	if(live_asking || test_view)
		return;
	if(server->user().isEmpty())
	{
		if(!live_.isEmpty())
		{
			live_.clear();
			emit liveChanged();
		}
		return;
	}
	live_asking = true;
	server->apiCall(QStringLiteral("izle_list"), QJsonObject{}, [this](const QJsonObject &r, const QString &err) {
		live_asking = false;
		if(!err.isEmpty())
			return;
		QVariantList list;
		for(const auto &v : r.value("yayinlar").toArray())
		{
			const QJsonObject y = v.toObject();
			if(!y.value("own").toBool() && y.value("state").toString() == QLatin1String("canli"))
				list.append(y.toVariantMap());
		}
		if(list != live_)
		{
			live_ = list;
			emit liveChanged();
		}
	});
}

void IzletController::watch(const QString &yid)
{
	if(yid.isEmpty() || !hooks.connectWatch)
		return;
	if(!watch_iid.isEmpty())
		leaveWatch(QString());
	const quint64 my = ++watch_gen;
	oviora::log::info("izlet", "watch: joining a broadcast");
	server->apiCall(QStringLiteral("izle_join"), QJsonObject{{"yid", yid}}, [this, my](const QJsonObject &r, const QString &err) {
		if(my != watch_gen)
			return;
		if(!err.isEmpty())
		{
			emit watchFailed(err);
			return;
		}
		watch_iid = r.value("iid").toString();
		watch_after = 0;
		watch_started = false;
		auto route = std::make_shared<oviora::remoteplay::WatchRoute>();
		route->codec = r.value("codec").toString() == QLatin1String("h265") ? "h265" : "h264";
		for(const auto &s : r.value("ice").toArray())
		{
			const QJsonValue urls = s.toObject().value("urls");
			if(urls.isArray())
				for(const auto &u : urls.toArray())
					route->iceServers.push_back(u.toString().toStdString());
			else if(urls.isString())
				route->iceServers.push_back(urls.toString().toStdString());
		}
		// The receiver's offer and candidates come from its own thread: sent from this one
		QPointer<IzletController> self(this);
		const QString iid = watch_iid;
		route->sendSignal = [self, iid](std::string json) {
			QMetaObject::invokeMethod(QCoreApplication::instance(), [self, iid, json = std::move(json)]() {
				if(!self || self->watch_iid != iid)
					return;
				const QJsonObject data = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
				if(!data.isEmpty())
					self->server->apiCall(QStringLiteral("izle_signal"), QJsonObject{{"iid", iid}, {"data", data}},
						[](const QJsonObject &, const QString &) {});
			}, Qt::QueuedConnection);
		};
		watch_route = route;
		const QString title = r.value("owner").toString() + QStringLiteral(" · ") + r.value("console").toString();
		if(!hooks.connectWatch(route, title))
		{
			leaveWatch(QString());
			emit watchFailed(tr("Yayın açılamadı."));
			return;
		}
		watchPoll();
	});
}

void IzletController::watchPoll()
{
	if(watch_iid.isEmpty() || watch_polling)
		return;
	watch_polling = true;
	const quint64 my = watch_gen;
	server->apiCall(QStringLiteral("izle_poll"), QJsonObject{{"iid", watch_iid}, {"after", double(watch_after)}},
		[this, my](const QJsonObject &r, const QString &err) {
		watch_polling = false;
		if(my != watch_gen || watch_iid.isEmpty())
			return;
		if(!err.isEmpty())
		{
			const int status = r.value("http_status").toInt();
			if(status == 403 || status == 404 || status == 410)
				return leaveWatch(err);
			watch_timer.start(1500);
			return;
		}
		for(const auto &v : r.value("signals").toArray())
		{
			const QJsonObject data = v.toObject().value("data").toObject();
			if(watch_route && !data.isEmpty())
				watch_route->pushRemote(QJsonDocument(data).toJson(QJsonDocument::Compact).toStdString());
		}
		watch_after = qint64(r.value("last").toDouble(double(watch_after)));
		const QString s = r.value("state").toString();
		if(s == QLatin1String("bitti") || s == QLatin1String("hata"))
		{
			const QString why = r.value("error").toString();
			return leaveWatch(why.isEmpty() ? tr("Yayın bitti.") : why);
		}
		watch_timer.start(s == QLatin1String("canli") ? 2000 : 50);
	});
}

// The watching ends: the player is told why (an empty reason: the user closed it), the server that the viewer left
void IzletController::leaveWatch(const QString &reason)
{
	const QString iid = watch_iid;
	++watch_gen;
	watch_timer.stop();
	watch_polling = false;
	watch_iid.clear();
	if(watch_route)
	{
		watch_route->setEnded((reason.isEmpty() ? tr("İzleme kapandı.") : reason).toStdString());
		watch_route.reset();
	}
	if(!iid.isEmpty())
	{
		oviora::log::info("izlet", "watch: left the broadcast");
		server->apiCall(QStringLiteral("izle_leave"), QJsonObject{{"iid", iid}}, [](const QJsonObject &, const QString &) {});
	}
}

void IzletController::setWatchState(bool watching, const QString &stage)
{
	if(watch_iid.isEmpty())
		return;
	if(watching && !stage.isEmpty() && stage != QLatin1String("failed"))
	{
		watch_started = true;
		return;
	}
	// The player closed (Bağlantıyı kes, Esc) or gave up: the broadcast is left too
	if(watch_started || stage == QLatin1String("failed"))
		leaveWatch(QString());
}

// The engine (and its tee) may already be gone here; the tee only holds the publisher's shared state
IzletController::~IzletController() = default;

bool IzletController::available() const
{
	return oviora::platform::IzletPublisher::isAvailable();
}

QString IzletController::watchUrl() const
{
	// The site's own name (the app may talk to the server by its address, which a browser cannot open)
	return QStringLiteral("https://oviora.tr/izle.php");
}

void IzletController::setState(const QString &state, const QString &error)
{
	if(state != state_ || error != error_)
		oviora::log::info("izlet", std::format("state={} {}", state.isEmpty() ? "kapali" : state.toStdString(), error.toStdString()));
	state_ = state;
	error_ = error;
	emit changed();
}

void IzletController::setStream(bool on, const QString &codec, const QString &console)
{
	const bool was = stream_on;
	stream_on = on;
	stream_codec = codec == QLatin1String("h265") ? QStringLiteral("h265") : QStringLiteral("h264");
	stream_console = console;
	if(was && !on && !yid.isEmpty())
		stop();
	else if(was != on)
		emit changed();
}

void IzletController::start(bool tellFriends)
{
	if(!available() || !stream_on || !yid.isEmpty() || state_ == QLatin1String("aciliyor"))
		return;
	const quint64 my = ++gen;
	codec_ = stream_codec;
	viewers_.clear();
	kbps_ = 0;
	oviora::log::info("izlet", std::format("start codec={} stream={}", codec_.toStdString(), stream_on));
	setState(QStringLiteral("aciliyor"));
	QJsonObject body{{"console", stream_console.isEmpty() ? QStringLiteral("PS5") : stream_console}, {"codec", codec_}, {"haber", tellFriends}};
	server->apiCall(QStringLiteral("izlet_start"), body, [this, my](const QJsonObject &r, const QString &err) {
		if(my != gen)
		{
			// Stopped meanwhile: the broadcast the server opened is closed again
			if(err.isEmpty())
				server->apiCall(QStringLiteral("izlet_stop"), QJsonObject{{"yid", r.value("yid").toString()}}, [](const QJsonObject &, const QString &) {});
			return;
		}
		if(!err.isEmpty())
			return setState(QStringLiteral("hata"), err);
		yid = r.value("yid").toString();
		after = 0;
		started_ms = QDateTime::currentMSecsSinceEpoch();
		if(!publisher)
		{
			publisher = new oviora::platform::IzletPublisher(this);
			connect(publisher, &oviora::platform::IzletPublisher::idrRequested, this, [this] {
				if(hooks.requestIdr)
					hooks.requestIdr();
			});
			connect(publisher, &oviora::platform::IzletPublisher::stateChanged, this, [this](const QString &s) {
				oviora::log::info("izlet", "publisher " + s.toStdString());
				if(!yid.isEmpty() && s == QLatin1String("failed"))
					end(QStringLiteral("hata"), tr("Yayın sunucusuyla bağlantı koptu."));
			});
		}
		const QString id = yid;
		const bool ok = publisher->start(codec_, r.value("ice").toArray(), [this, id](const QJsonObject &data) {
			if(id == yid)
				server->apiCall(QStringLiteral("izlet_signal"), QJsonObject{{"yid", id}, {"data", data}}, [](const QJsonObject &, const QString &) {});
		});
		if(!ok)
			return end(QStringLiteral("hata"), tr("Yayın başlatılamadı."));
		if(hooks.setTee)
			hooks.setTee(publisher->makeTeeCallbacks());
		poll();
	});
}

void IzletController::poll()
{
	if(yid.isEmpty() || polling)
		return;
	polling = true;
	const quint64 my = gen;
	server->apiCall(QStringLiteral("izlet_poll"), QJsonObject{{"yid", yid}, {"after", double(after)}}, [this, my](const QJsonObject &r, const QString &err) {
		polling = false;
		if(my != gen || yid.isEmpty())
			return;
		if(!err.isEmpty())
		{
			const int status = r.value("http_status").toInt();
			if(status == 404 || status == 410 || status == 403)
				return end(QStringLiteral("hata"), err);
			poll_timer.start(2000);   // the network for a moment: again
			return;
		}
		for(const auto &v : r.value("signals").toArray())
			publisher->onRemoteSignal(v.toObject().value("data").toObject());
		after = qint64(r.value("last").toDouble(double(after)));
		const QString s = r.value("state").toString();
		QVariantList list;
		for(const auto &v : r.value("viewers").toArray())
			list.append(v.toObject().toVariantMap());
		const QJsonObject stats = r.value("stats").toObject();
		kbps_ = stats.value("kbps").toInt();
		const int newLost = stats.value("loss").toInt();
		if(newLost > 0 && (lost_ == 0 || newLost - lost_ >= 25))
			oviora::log::info("izlet", std::format("upload losing packets (loss={}, {} kbit/s up)",
				newLost, stats.value("in_kbps").toInt()));
		lost_ = newLost;
		upload_kbps = stats.value("in_kbps").toInt();
		// The gateway counts over 5 s: what came in (~1150-byte packets) against what was lost. A share that stays high
		// means the upload cannot carry the stream, and the full line holds up the PS5's own traffic too: past 20 s
		// the broadcast stops so the game keeps going
		const double arrived = upload_kbps * 1000.0 * 5.0 / 8.0 / 1150.0;
		const bool bad = newLost > 0 && newLost >= 0.03 * (arrived + newLost);
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if(bad && bad_since == 0)
			bad_since = now;
		else if(!bad)
			bad_since = 0;
		if(bad_since != 0 && now - bad_since > 20000 && s == QLatin1String("canli"))
		{
			oviora::log::info("izlet", std::format("upload too small for the stream ({} kbit/s, loss={}): stopping to spare the game",
				upload_kbps, newLost));
			server->apiCall(QStringLiteral("izlet_stop"), QJsonObject{{"yid", yid}}, [](const QJsonObject &, const QString &) {});
			return end(QStringLiteral("hata"), tr("İnternetinin yükleme hızı bu yayına yetmediği için İzlet durduruldu; oyun bozulmasın diye. PS5 bağlantı ayarlarında bit hızını ya da çözünürlüğü düşürüp yeniden açabilirsin."));
		}
		if(s == QLatin1String("bitti") || s == QLatin1String("hata"))
			return end(s == QLatin1String("bitti") && r.value("error").toString().isEmpty() ? QString() : QStringLiteral("hata"),
				r.value("error").toString());
		if(s != QLatin1String("canli") && QDateTime::currentMSecsSinceEpoch() - started_ms > 25000)
		{
			server->apiCall(QStringLiteral("izlet_stop"), QJsonObject{{"yid", yid}}, [](const QJsonObject &, const QString &) {});
			return end(QStringLiteral("hata"), tr("Görüntü yayın sunucusuna ulaşmadı. Sunucuda UDP 50000-50100 açık mı (Windows güvenlik duvarı ve sağlayıcının paneli)?"));
		}
		if(s == QLatin1String("canli") && state_ != s)
		{
			oviora::log::info("izlet", "on the air");
			state_ = s;
		}
		if(viewers_.size() != list.size())
			oviora::log::info("izlet", std::format("viewers {}", list.size()));
		viewers_ = list;
		emit changed();
		// Before it is on the air the server holds the call (long poll); after, every 2 s for viewers and signals
		poll_timer.start(s == QLatin1String("canli") ? 2000 : 50);
	});
}

void IzletController::stop()
{
	if(!yid.isEmpty() && yid != QLatin1String("test"))
		server->apiCall(QStringLiteral("izlet_stop"), QJsonObject{{"yid", yid}}, [](const QJsonObject &, const QString &) {});
	end(QString(), QString());
}

void IzletController::end(const QString &state, const QString &error)
{
	++gen;
	poll_timer.stop();
	polling = false;
	if(hooks.setTee)
		hooks.setTee({});
	if(publisher)
		publisher->stop();
	yid.clear();
	viewers_.clear();
	kbps_ = 0;
	lost_ = 0;
	bad_since = 0;
	setState(state, error);
}

void IzletController::showTest()
{
	stream_on = true;
	codec_ = stream_codec = QStringLiteral("h265");
	yid = QStringLiteral("test");
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	viewers_ = {QVariantMap{{"name", "deniz"}, {"since", now - 300}}, QVariantMap{{"name", "ece"}, {"since", now - 60}}};
	kbps_ = 46000;
	test_view = true;
	live_ = {QVariantMap{{"yid", QString(32, QLatin1Char('a'))}, {"owner", "ece"}, {"console", "Salon PS5"}, {"codec", "h264"}, {"viewers", 3}}};
	emit liveChanged();
	setState(QStringLiteral("canli"));
}

void IzletController::kick(const QString &name)
{
	if(yid.isEmpty())
		return;
	server->apiCall(QStringLiteral("izlet_kick"), QJsonObject{{"yid", yid}, {"name", name}}, [this](const QJsonObject &, const QString &) {
		poll_timer.start(0);
	});
}
