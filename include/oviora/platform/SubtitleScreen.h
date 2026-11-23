// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Oviora: the translated subtitle on a phone. A small HTTP server on this PC's home network (on when the user
// turns it on in the Ctrl+O menu; it stays on at the next starts until turned off) pushes every new line at once
// over one open connection (server-sent events), so nothing waits for the phone to ask.
// Two ways in: the Oviora phone app finds this PC on the network (a UDP question on port 47823) and pairs once
// with the 6-digit code shown here; any other phone opens the page from the QR code. A key (kept, so a paired
// phone comes back by itself; "Kodu yenile" makes a new one) keeps other devices out; nothing goes to the internet.
//
// Protocol (protocol version 1):
//   UDP 47823  "OVIORA-ALTYAZI-ARA 1"  ->  {"oviora":"altyazi","v":1,"id":<this PC>,"name":<PC name>,"port":<TCP>}
//   GET /eslestir?c=<code>  ->  {"k":<key>,"id","name"} (403 wrong code; 429 after 10 wrong ones in 10 minutes)
//   GET /olay?k=<key>       ->  text/event-stream, data: {"t":<Turkish line>,"s":<original line>,"n":<serial>}
//   GET /?k=<key>           ->  the page for a phone's browser
#ifndef OVIORA_PLATFORM_SUBTITLESCREEN_H
#define OVIORA_PLATFORM_SUBTITLESCREEN_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

class SubtitleScreen : public QObject
{
	Q_OBJECT
	Q_PROPERTY(bool running READ running NOTIFY stateChanged)
	// http://<this PC on the home network>:<port>/?k=<key>; "" when stopped or no home network was found
	Q_PROPERTY(QString url READ url NOTIFY stateChanged)
	// The address as a QR code (an SVG data URL for an Image)
	Q_PROPERTY(QString qr READ qr NOTIFY stateChanged)
	// The 6-digit code the phone app asks for once ("123 456" on screen)
	Q_PROPERTY(QString code READ code NOTIFY stateChanged)
	// Phones with the page or the app open now
	Q_PROPERTY(int clients READ clients NOTIFY clientsChanged)
	Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
	explicit SubtitleScreen(QObject *parent = nullptr);
	~SubtitleScreen() override;

	bool running() const;
	QString url() const { return address; }
	QString qr() const { return qr_code; }
	QString code() const { return pair_code; }
	int clients() const { return int(streams.size()); }
	QString error() const { return error_text; }

	/** Starts serving and remembers it (the next start of Oviora serves again); Windows may ask to allow it once */
	Q_INVOKABLE void start();
	/** Stops serving, drops every phone and remembers it */
	Q_INVOKABLE void stop();
	/** A new key and code: paired phones must pair again */
	Q_INVOKABLE void renewCode();
	/** At Oviora's start: serves again when it was left on */
	void resume();
	/** Test runs: this PC only (127.0.0.1), so no firewall question comes up; nothing is remembered */
	void setLocalOnly(bool on) { local_only = on; }

public slots:
	/** The subtitle line on screen now ("" for none) and its original: pushed to every phone at once */
	void setText(const QString &text, const QString &source = QString());

signals:
	void stateChanged();
	void clientsChanged();

private:
	void listen();
	void close();
	void loadKeys(bool renew);
	void onConnection();
	void onReadable(QTcpSocket *socket);
	void onDatagram();
	void respond(QTcpSocket *socket, const QByteArray &status, const QByteArray &type, const QByteArray &body);
	void openStream(QTcpSocket *socket);
	bool pairingAllowed(const QString &ip);
	QByteArray event() const;
	QByteArray page() const;
	static QString homeAddress();

	QTcpServer *server = nullptr;
	QUdpSocket *finder = nullptr;
	QList<QPointer<QTcpSocket>> streams;
	QHash<QString, QList<qint64>> wrong_codes;	// per address: when a wrong code came (the last 10 minutes)
	QTimer heartbeat;
	QString key, pair_code, pc_id, address, qr_code, error_text;
	QString current, current_source;
	quint64 serial = 0;
	bool local_only = false;
};

#endif // OVIORA_PLATFORM_SUBTITLESCREEN_H
