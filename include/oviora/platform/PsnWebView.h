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

signals:
	void availableChanged();
	void stateChanged();
	/** Sony's sign-in ended on the remote play redirect address (the navigation was cancelled) */
	void redirected(const QString &url);

private:
	struct Impl;
	Impl *d;
	QPointer<QWindow> window;
	QString state_ = "idle", error_text;

	void setState(const QString &state, const QString &error = QString());
	void applyBounds();
	friend struct Impl;
};

#endif // OVIORA_PSNWEBVIEW_H
