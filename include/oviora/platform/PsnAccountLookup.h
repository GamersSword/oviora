// Oviora: the old "PSN ile giriş yap" of the console dialog. Sony's sign-in page (PsnWebView) for any
// PSN account on the console; the redirect's code is exchanged for a token only to read that
// account's id, which goes into the dialog. Nothing is stored or linked: the token is dropped at once,
// and the Oviora account's linked PSN account stays as it is. QML: the context property "psnLookup".
#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;

class PsnAccountLookup : public QObject
{
	Q_OBJECT
	Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
	explicit PsnAccountLookup(QObject *parent = nullptr);

	bool busy() const { return working; }
	/** Sony's sign-in page; it always asks for the account (prompt=always) */
	Q_INVOKABLE QString loginUrl() const;
	/** The sign-in page's redirect (PsnWebView::redirected): answers with found() or failed() */
	Q_INVOKABLE void finish(const QString &redirectUrl);

signals:
	void busyChanged();
	void found(const QString &accountId, const QString &onlineId);
	void failed(const QString &text);

private:
	void fail(const QString &text);
	QNetworkAccessManager *nam = nullptr;
	bool working = false;
};
