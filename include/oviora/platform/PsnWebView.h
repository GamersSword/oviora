// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Oviora: Sony's PSN sign-in inside the app on Windows, with Microsoft Edge WebView2 (there is no
// QtWebEngine in the MinGW build). A native child window of the main window shows the page over an
// area of the QML dialog; the redirect address the sign-in ends on is caught before it loads and
// handed to QML. WebView2Loader.dll ships next to Oviora.exe; without the WebView2 runtime the
// dialogs fall back to the normal browser and pasting the address.

#ifndef OVIORA_PSNWEBVIEW_H
#define OVIORA_PSNWEBVIEW_H

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QUrl>

#include <QWindow>

#include <functional>

class PsnWebView : public QObject
{
	Q_OBJECT
	/** WebView2Loader.dll and the Edge WebView2 runtime are both there */
	Q_PROPERTY(bool available READ available NOTIFY availableChanged)
	/** idle, starting, loading, ready, error */
	Q_PROPERTY(QString state READ state NOTIFY stateChanged)
	Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

public:
	PsnWebView(QWindow *window, QObject *parent);
	~PsnWebView();

	/** The window the page is shown in (known only after QML has loaded) */
	void setWindow(QWindow *w) { window = w; }
	bool available() const;
	QString state() const { return state_; }
	QString errorText() const { return error_text; }

	/** Shows Sony's page at url inside the main window, over the given rect (QML scene coordinates) */
	Q_INVOKABLE void open(const QUrl &url, qreal x, qreal y, qreal w, qreal h);
	/** The QML area moved or was resized */
	Q_INVOKABLE void setRect(qreal x, qreal y, qreal w, qreal h);
	Q_INVOKABLE void setVisible(bool visible);
	/** Closes the page and deletes everything it kept (cookies, cache) */
	Q_INVOKABLE void close();
	/**
	 * Oyun bilgisi: link > 0 makes the next sign-in (open()) one for that linked PSN account. When Sony's
	 * sign-in ends, the same page reads Sony's SSO cookie page out of sight and hands its value straight to
	 * the sink (C++ only: never in a signal, property or log), then everything the page kept is deleted
	 * and wideFinished() comes. 0: an ordinary sign-in again. close() also sets it back to 0.
	 */
	Q_INVOKABLE void setWideLink(int link);
	/**
	 * "PSN hesabını bağla" with "Oyun bilgisini de aç": on makes the next ordinary sign-in keep its page when
	 * Sony's redirect is caught. The page is hidden at once and held (redirected() still comes, so the
	 * account is linked as usual); readWideFor() then reads Sony's SSO cookie page in that same session
	 * for the account just linked, or close() drops it. close() also sets it back to off.
	 */
	Q_INVOKABLE void setLinkWide(bool on);
	/** A linked-and-wide sign-in's page is held after the redirect (QML must not close it then) */
	Q_INVOKABLE bool holdingForWide() const;
	/** C++ only: the held page reads Sony's SSO cookie page for link (> 0) and hands it to the sink */
	void readWideFor(int link);
	/** Where the read value goes (OvioraServer::wideLinkPsn); the sink wipes it, and it is wiped again after.
	 *  linked: it comes from "PSN hesabını bağla" (the link itself already counts; a failure is only soft) */
	using WideSink = std::function<void(int link, QByteArray &value, bool linked)>;
	void setWideSink(WideSink sink) { wide_sink = std::move(sink); }

signals:
	void availableChanged();
	void stateChanged();
	/** Sony's sign-in ended on the remote play redirect address (the navigation was cancelled) */
	void redirected(const QString &url);
	/** Oyun bilgisi sign-in done: sent (the server's answer comes from OvioraServer), or not, with why */
	void wideFinished(bool sent, const QString &error);
	/** The held page of "PSN hesabını bağla" could not read Sony's sign-in (the PSN link still counts) */
	void linkWideFailed();

private:
	struct Impl;
	Impl *d;
	QPointer<QWindow> window;
	QString state_ = "idle", error_text;
	WideSink wide_sink;

	void setState(const QString &state, const QString &error = QString());
	void applyBounds();
	void startWideRead();
	void holdForWide();
	void finishWide(bool sent, const QString &error);
	friend struct Impl;
};

#endif // OVIORA_PSNWEBVIEW_H
