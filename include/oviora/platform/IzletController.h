// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oviora/remoteplay/ChiakiBackend.h"

#include <memory>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>

class OvioraServer;
namespace oviora::remoteplay { struct WatchRoute; }
namespace oviora::platform { class IzletPublisher; }

/**
 * "İzlet": the PS5 stream being played, watched live by friends and group members in the browser (oviora.tr/izle.php).
 * The PS5's picture and sound go once, not re-encoded, to the server's web gateway, which copies them to the viewers
 * (IzletPublisher). This side keeps the broadcast with the server: izlet_start / izlet_signal / izlet_poll / izlet_stop
 * / izlet_kick (contract: inceleme\agy-gorev\izlet-sozlesme.md). Ctrl+O "İzlet" (StreamMenu.qml) shows it.
 */
class IzletController : public QObject
{
	Q_OBJECT
	Q_PROPERTY(bool available READ available CONSTANT)
	/** Whether a PS5 stream runs now (İzlet can only start then) */
	Q_PROPERTY(bool streaming READ streaming NOTIFY changed)
	/** "" off, "aciliyor" starting, "canli" on the air, "hata" (error says why) */
	Q_PROPERTY(QString state READ state NOTIFY changed)
	Q_PROPERTY(QString error READ error NOTIFY changed)
	/** "h264" or "h265": the stream's own codec, passed on as it is */
	Q_PROPERTY(QString codec READ codec NOTIFY changed)
	/** [{name, since}] who watches now */
	Q_PROPERTY(QVariantList viewers READ viewers NOTIFY changed)
	/** The gateway's outgoing rate to all viewers, kbit/s (0 when not known) */
	Q_PROPERTY(int kbps READ kbps NOTIFY changed)
	/** Video packets from this PC lost on the way to the server in the last few seconds (the upload is not enough) */
	Q_PROPERTY(int lost READ lost NOTIFY changed)
	/** What this PC uploads to the server, kbit/s */
	Q_PROPERTY(int uploadKbps READ uploadKbps NOTIFY changed)
	/** The upload keeps losing a share of the video: shown as a warning, and the broadcast stops on its own if it lasts */
	Q_PROPERTY(bool struggling READ struggling NOTIFY changed)
	Q_PROPERTY(QString watchUrl READ watchUrl CONSTANT)
	/** Friends' and group members' broadcasts on the air now: [{yid, owner, console, codec, since, viewers}] (own one left out) */
	Q_PROPERTY(QVariantList live READ live NOTIFY liveChanged)
	/** Whether broadcasts can be watched in this app (its own player: QmlPlayerBridge::connectWatch) */
	Q_PROPERTY(bool canWatch READ canWatch CONSTANT)

public:
	struct Hooks {
		/** Hands the stream's AUs and Opus packets to the publisher too (empty callbacks: stop) */
		std::function<void(oviora::remoteplay::MediaRelayCallbacks)> setTee;
		/** Asks the PS5 for a new key frame (a viewer could not start from the gateway's memory) */
		std::function<void()> requestIdr;
		/** Plays a friend's broadcast in this app's player (false: could not start) */
		std::function<bool(std::shared_ptr<oviora::remoteplay::WatchRoute>, const QString &title)> connectWatch;
		/** Ends the watching in the player */
		std::function<void()> stopWatch;
	};
	IzletController(OvioraServer *server, Hooks hooks, QObject *parent = nullptr);
	~IzletController() override;

	bool available() const;
	bool streaming() const { return stream_on; }
	QString state() const { return state_; }
	QString error() const { return error_; }
	QString codec() const { return yid.isEmpty() ? stream_codec : codec_; }
	QVariantList viewers() const { return viewers_; }
	int kbps() const { return kbps_; }
	int lost() const { return lost_; }
	int uploadKbps() const { return upload_kbps; }
	bool struggling() const { return bad_since != 0; }
	QString watchUrl() const;
	QVariantList live() const { return live_; }
	bool canWatch() const;
	/** The player's state (main.cpp): a watching that left the player (closed, failed) leaves the broadcast too */
	void setWatchState(bool watching, const QString &stage);

	/** The stream now: whether one runs, its codec ("h264"/"h265") and the PS5's name. A stream that ends ends the broadcast */
	void setStream(bool on, const QString &codec, const QString &console);

	/** --test-izlet: a made-up broadcast on the air with two viewers, for screenshots (nothing is sent) */
	void showTest();

	Q_INVOKABLE void start(bool tellFriends);
	Q_INVOKABLE void stop();
	Q_INVOKABLE void kick(const QString &name);
	/** Plays this broadcast in the app's own player, the way a PS5 plays */
	Q_INVOKABLE void watch(const QString &yid);
	/** Asks the server for the live list now (it is also asked every 20 s while signed in) */
	Q_INVOKABLE void refreshLive();

signals:
	void changed();
	void liveChanged();
	/** Watching could not start: shown as a toast */
	void watchFailed(const QString &text);

private:
	void poll();
	void end(const QString &state, const QString &error);
	void setState(const QString &state, const QString &error = QString());

	OvioraServer *server;
	Hooks hooks;
	oviora::platform::IzletPublisher *publisher = nullptr;
	QTimer poll_timer;
	bool stream_on = false;
	QString stream_codec = QStringLiteral("h264"), stream_console;
	QString yid, codec_, state_, error_;
	qint64 after = 0;
	/** When izlet_start was answered: a broadcast not on the air 25 s later gives up with the likely reason */
	qint64 started_ms = 0;
	bool polling = false;
	/** Raised on every start/stop: answers for an older broadcast are dropped */
	quint64 gen = 0;
	QVariantList viewers_;
	int kbps_ = 0;
	int lost_ = 0, upload_kbps = 0;
	/** Since when the upload has been losing too much (0: it is fine); past a limit the broadcast stops to spare the game */
	qint64 bad_since = 0;
	QVariantList live_;
	QTimer live_timer;
	bool live_asking = false;
	bool test_view = false;
	// The broadcast watched now (izle_join's iid) and its signals
	void watchPoll();
	void leaveWatch(const QString &reason);
	QString watch_iid;
	qint64 watch_after = 0;
	quint64 watch_gen = 0;
	bool watch_polling = false, watch_started = false;
	std::shared_ptr<oviora::remoteplay::WatchRoute> watch_route;
	QTimer watch_timer;
};
