// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Yakamoz platform account, only in packages made by the platform's server (sunucu.txt next to the
// exe). Everyone signs in, links their own PSN account (kept on the server), adds friends and opens
// their PS5s to them; connecting to a PS5 gets a short PSN lease of its owner for that session.
// Releases are uploaded on the management site and signed there; every Yakamoz downloads the
// changed files and installs them on restart.

#ifndef OVIORA_PLATFORM_SERVER_H
#define OVIORA_PLATFORM_SERVER_H

#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QHash>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

class OvioraServer : public QObject
{
	Q_OBJECT
	Q_PROPERTY(bool configured READ configured NOTIFY changed)
	Q_PROPERTY(QString user READ user NOTIFY changed)
	Q_PROPERTY(QString status READ status NOTIFY changed)          // none, active
	Q_PROPERTY(QString sessionState READ sessionState NOTIFY changed) // idle, asking, playing
	Q_PROPERTY(QString sessionDuid READ sessionDuid NOTIFY changed)
	// From the server's account view
	Q_PROPERTY(bool loaded READ loaded NOTIFY accountChanged)
	Q_PROPERTY(bool busy READ isBusy NOTIFY accountChanged)
	Q_PROPERTY(bool psnLinked READ psnLinked NOTIFY accountChanged)
	// The linked PSN account's id (base64), for registering a PS5; "" when not linked or an older server
	Q_PROPERTY(QString psnAccountId READ psnAccountId NOTIFY accountChanged)
	Q_PROPERTY(QVariantList psnAccounts READ psnAccounts NOTIFY accountChanged)    // saved PSN accounts: [{id, name}]
	Q_PROPERTY(QVariantList consoles READ consoles NOTIFY accountChanged)     // my PS5s: duid, name, remote_play, shared, guest, sid, since
	Q_PROPERTY(QVariantList available READ available NOTIFY accountChanged)   // PS5s I can connect to: host, duid, name, mine, busy
	Q_PROPERTY(QVariantList friends READ friends NOTIFY accountChanged)       // name, online, shares
	Q_PROPERTY(QStringList incoming READ incoming NOTIFY accountChanged)
	Q_PROPERTY(QStringList outgoing READ outgoing NOTIFY accountChanged)
	Q_PROPERTY(QString updateState READ updateState NOTIFY updateChanged)  // none, checking, downloading, ready, error
	Q_PROPERTY(QString updateText READ updateText NOTIFY updateChanged)
	Q_PROPERTY(double updateProgress READ updateProgress NOTIFY updateChanged)
	Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateChanged)
	Q_PROPERTY(QString updateNotes READ updateNotes NOTIFY updateChanged)
	Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY updateChanged)
	/** A newer Yakamoz is on the site: nothing works until it is installed */
	Q_PROPERTY(bool updateRequired READ updateRequired NOTIFY updateChanged)
	/** Optional components (big NVIDIA runtimes), id -> { title, installed, state (none, working, done, error), text, progress } */
	Q_PROPERTY(QVariantMap components READ components NOTIFY componentsChanged)
	/** Recent events (newest first): type, title, text, time */
	Q_PROPERTY(QVariantList notifications READ notifications NOTIFY notificationsChanged)
	Q_PROPERTY(int unreadCount READ unreadCount NOTIFY notificationsChanged)
	Q_PROPERTY(bool reportSending READ reportSending NOTIFY reportChanged)
	/** My invite: code, url, url_ip, approve, uses ({} until loaded) and the link as a QR picture (data URL) */
	Q_PROPERTY(QVariantMap invite READ invite NOTIFY inviteChanged)
	Q_PROPERTY(QString inviteQr READ inviteQr NOTIFY inviteChanged)
	/** "Oturum geçmişim": id, role (guest/host/own), other, console, start, end, length, live; newest first */
	Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
	Q_PROPERTY(bool historyMore READ historyMore NOTIFY historyChanged)
	Q_PROPERTY(bool historyLoading READ historyLoading NOTIFY historyChanged)

public:
	struct Hooks
	{
		/** relay_sid: not empty when the server relays PSN for this lease (then token is empty) */
		std::function<void(const QString &token, const QString &account_id, const QString &duid, int target, const QString &nickname, const QString &relay_sid)> connect_psn;
		std::function<void()> stop_session;
		std::function<bool()> session_active;
	};

	OvioraServer(Hooks hooks, QObject *parent);
	// At exit, before the objects the hooks capture are destroyed: a reply aborted while the app is
	// torn down must not call into them
	void dropHooks() { hooks = {}; poll_timer.stop(); }

	bool configured() const { return !server_url.isEmpty(); }
	QString user() const { return user_name; }
	QString status() const { return account_status; }
	QString sessionState() const { return session_state; }
	QString sessionDuid() const { return session_duid; }
	bool loaded() const { return account.contains("user"); }
	bool isBusy() const { return busy; }
	bool psnLinked() const { return account.value("psn").toBool(); }
	QString psnAccountId() const { return account.value("psn_account").toString(); }
	QVariantList psnAccounts() const { return account.value("psn_accounts").toVariant().toList(); }
	QVariantList consoles() const { return account.value("consoles").toVariant().toList(); }
	QVariantList available() const { return account.value("available").toVariant().toList(); }
	QVariantList friends() const { return account.value("friends").toVariant().toList(); }
	QStringList incoming() const { return account.value("incoming").toVariant().toStringList(); }
	QStringList outgoing() const { return account.value("outgoing").toVariant().toStringList(); }
	QString updateState() const { return update_state; }
	QString updateText() const { return update_text; }
	double updateProgress() const { return update_progress; }
	QString updateVersion() const { return update_version; }
	QString updateNotes() const { return update_notes; }
	QString installedVersion() const;
	bool updateRequired() const { return update_required; }
	QVariantList notifications() const { return notification_list; }
	int unreadCount() const { return unread; }
	bool reportSending() const { return report_sending; }

	/** Problem report: the files that would be sent (name, size in KB) */
	Q_INVOKABLE QVariantList reportFiles() const;
	Q_INVOKABLE void sendReport(const QString &note, const QString &kind);
	/** Queue for a busy console */
	Q_INVOKABLE void setQueue(const QString &duid, bool on);
	Q_INVOKABLE void markNotificationsRead();
	Q_INVOKABLE bool notifyEnabled(const QString &type) const;
	Q_INVOKABLE void setNotifyEnabled(const QString &type, bool on);

	/** invite: an invite code or link typed at sign-up/sign-in, may be empty */
	Q_INVOKABLE void registerUser(const QString &name, const QString &password, const QString &invite = QString());
	Q_INVOKABLE void login(const QString &name, const QString &password, const QString &invite = QString());

	QVariantMap invite() const { return invite_info; }
	QString inviteQr() const { return invite_qr; }
	/** Loads my invite (made on first use); regenerate: a new code, the old one stops working */
	Q_INVOKABLE void loadInvite(bool regenerate = false);
	/** true: someone coming with my code sends me a friend request; false: we become friends at once */
	Q_INVOKABLE void setInviteApprove(bool approve);
	/** Signed in already: use a friend's code or link */
	Q_INVOKABLE void acceptInvite(const QString &code_or_link);
	/** Who invited with this code; answers with inviteChecked() */
	Q_INVOKABLE void checkInvite(const QString &code_or_link);
	/** The code from a typed code or a full invite link ("" if it doesn't look like one) */
	Q_INVOKABLE static QString inviteCode(const QString &code_or_link);

	QVariantList history() const { return history_list; }
	bool historyMore() const { return history_more; }
	bool historyLoading() const { return history_loading; }
	/** First page (more = false) or the next one */
	Q_INVOKABLE void loadHistory(bool more = false);

	/** Signed in to an Oviora server (requests with a login can be made) */
	bool signedIn() const { return configured() && !token.isEmpty(); }
	/**
	 * Oviora: the same request made from another thread, waiting for the answer at most timeout_ms
	 * (the PSN relay of the library). Never call it on the GUI thread.
	 */
	QJsonObject callBlocking(const QString &action, const QJsonObject &body, QString *error, int timeout_ms);
	/** A request with this login, for helpers outside (e.g. the shared translation memory) */
	void call(const QString &action, const QJsonObject &body, std::function<void(const QJsonObject &reply, const QString &error)> done)
	{
		post(action, body, std::move(done));
	}
	Q_INVOKABLE void logout();
	Q_INVOKABLE void deleteAccount(const QString &password);
	Q_INVOKABLE QString legalUrl(const QString &page) const;
	Q_INVOKABLE void refresh();
	/** Answers with psnLoginUrl(): Sony's sign-in page for this account's server-side login */
	Q_INVOKABLE void requestPsnLoginUrl();
	/** The address Sony's sign-in ended on; the server turns it into this account's PSN login */
	Q_INVOKABLE void linkPsn(const QString &redirect);
	Q_INVOKABLE void unlinkPsn();
	Q_INVOKABLE void savePsnAccount(const QString &id, const QString &name);
	Q_INVOKABLE void removePsnAccount(const QString &id);
	Q_INVOKABLE void refreshConsoles();
	Q_INVOKABLE void addFriend(const QString &name);
	Q_INVOKABLE void acceptFriend(const QString &name);
	Q_INVOKABLE void removeFriend(const QString &name);
	Q_INVOKABLE void setShare(const QString &duid, const QString &friend_name, bool on);
	/** Ends someone's session on one of my PS5s; they lose access to it until I share it again */
	Q_INVOKABLE void kick(const QString &sid);
	Q_INVOKABLE void connectTo(const QString &host, const QString &duid);
	Q_INVOKABLE void checkUpdate();
	/** Moves the downloaded files in place and starts the new Yakamoz */
	Q_INVOKABLE void applyUpdate();
	QVariantMap components() const;
	/** Downloads an optional component of the current release into the program folder */
	Q_INVOKABLE void installComponent(const QString &id);
	/** Starts Oviora again (a newly installed component is loaded at start) */
	Q_INVOKABLE void restartApp();
	/** The stream session ended (from the backend) */
	void sessionEnded();
	/**
	 * My own PS5 seen on the LAN: what it runs (discovery's running-app name/title id). Sent as
	 * `console_app` when it changes and at most every ~30 s otherwise; a console the server says isn't
	 * mine is left alone for 10 minutes.
	 */
	void reportConsoleApp(const QString &duid, const QString &app_name, const QString &title_id, const QString &state);
	/** Title id of the game running on a console I can see (LAN report or `me` app), "" if unknown */
	Q_INVOKABLE QString runningTitleId(const QString &duid) const;

signals:
	void changed();
	void accountChanged();
	void accountDeleted();
	void accountDeleteFailed(const QString &text);
	void updateChanged();
	void message(const QString &title, const QString &text);
	void psnLoginUrl(const QString &url);
	void updateReady();
	void componentsChanged();
	void notificationsChanged();
	void reportChanged();
	void reportDone(bool ok, const QString &text);
	void inviteChanged();
	void inviteChecked(const QString &code, const QString &from, const QString &error);
	void historyChanged();
	/** A new event while the window is in front: shown as an in-app toast */
	void toast(const QString &title, const QString &text);

private:
	using Callback = std::function<void(const QJsonObject &reply, const QString &error)>;
	QNetworkReply *post(const QString &action, QJsonObject body, Callback done);
	/** Server from the "YAKAMOZ-SUNUCU-..." code in sunucu.txt */
	bool readConnectionCode(const QString &code);
	void pin(QNetworkReply *reply);
	void load();
	void save();
	void loggedIn();
	void loggedOut(const QString &why);
	void takeLogin(const QJsonObject &reply);
	void takeAccount(const QJsonObject &reply);
	void requireUpdate();
	/** An account change on the server; its answer is the new account view */
	void accountCall(const QString &action, QJsonObject body, const QString &fail_title);
	void setSession(const QString &state);
	void endSession();

	// Updates
	struct UpdateFile { QString path, sha256; qint64 size = 0; };
	QString appDir() const;
	QString stageDir() const;
	QString hashFile(const QString &path);
	bool verifySignature(const QByteArray &manifest, const QByteArray &signature) const;
	void downloadNext();
	/** The optional component a release file belongs to ("" for an ordinary file) */
	static QString componentOf(const QString &path);
	bool componentInstalled(const QString &id) const;
	void componentNext();
	QString component_id, component_state = "none", component_text;
	double component_progress = 0;
	QList<UpdateFile> component_queue;
	qint64 component_total = 0, component_done = 0;
	void cleanupOldFiles();
	/** After the rename to Oviora: shortcuts, the Windows app list entry and the old Yakamoz.exe */
	void takeOverOldName();

	Hooks hooks;
	QNetworkAccessManager *net;
	QString server_url, cert_fp, publisher_pub;
	QString token, user_name, account_status = "none";
	QJsonObject account;
	QVariantMap invite_info;
	QString invite_qr;
	QVariantList history_list;
	bool history_more = false;
	bool history_loading = false;
	qint64 history_next_before = 0;
	void takeInvite(const QJsonObject &reply);
	/** What a sign-up/sign-in with an invite code did: friends / requested / already / self / invalid */
	void tellInviteResult(const QJsonObject &invite);
	bool busy = false;
	QString session_state = "idle", session_id, session_host, session_duid;
	QTimer poll_timer, account_timer, update_timer;

	QString update_state = "none", update_text, update_version, update_notes;
	bool update_required = false;
	double update_progress = 0;
	QList<UpdateFile> update_files, update_queue;
	qint64 update_bytes_total = 0, update_bytes_done = 0;
	QPointer<QNetworkReply> download;
	QFile *download_file = nullptr;
	// Development build (no OVIORA_PLATFORM_UPDATES): the release the server offers, sent as ours
	QString dev_server_version;

	// Events and notifications
	void fetchEvents();
	void takeEvent(const QJsonObject &e, bool announce);
	QVariantList notification_list;
	int unread = 0;
	qint64 events_last = -1;
	bool events_busy = false;
	bool events_unsupported = false;
	QObject *tray = nullptr;
	bool report_sending = false;

	struct AppReport { QString sig; qint64 at = 0; qint64 refused_until = 0; };
	QHash<QString, AppReport> app_reports;
	QHash<QString, QString> lan_title_ids;	// duid -> title id from my own LAN discovery
};

#endif // OVIORA_PLATFORM_SERVER_H
