// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Oviora: see SubtitleScreen.h. HTTP/1.1 by hand (GET only, four addresses), anything else 404. The key and
// the code are compared in constant time; the key is kept DPAPI-sealed in platform.ini (group "altyazi-ekrani").
#include "oviora/platform/SubtitleScreen.h"
#include "oviora/platform/SecretStore.h"
#include "oviora/Log.h"
#include "oviora/RuntimePaths.h"

#include "qrcodegen.hpp"

#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUrlQuery>

#include <format>

namespace {

// UDP: where the phone app asks; TCP: the first free one from the same number (the address stays the same
// between runs unless taken)
constexpr quint16 finder_port = 47823;
constexpr quint16 first_port = 47823;
constexpr int port_tries = 20;
constexpr int max_phones = 6;
constexpr qsizetype max_request = 8192;
constexpr int max_wrong_codes = 10;
constexpr qint64 wrong_code_window_ms = 10 * 60 * 1000;

QString settingsPath()
{
	return QString::fromStdWString((oviora::runtime::localDataDirectory() / L"platform.ini").wstring());
}

QString qrSvg(const QString &text)
{
	try
	{
		const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
		const int border = 4;
		const int size = qr.getSize() + border * 2;
		QString path;
		for(int y = 0; y < qr.getSize(); y++)
			for(int x = 0; x < qr.getSize(); x++)
				if(qr.getModule(x, y))
					path += QStringLiteral("M%1,%2h1v1h-1z").arg(x + border).arg(y + border);
		const QString svg = QStringLiteral(
			"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %1 %1\" width=\"%2\" height=\"%2\" shape-rendering=\"crispEdges\">"
			"<rect width=\"%1\" height=\"%1\" fill=\"#ffffff\"/><path d=\"%3\" fill=\"#0e0d12\"/></svg>")
			.arg(size).arg(size * 8).arg(path);
		return QStringLiteral("data:image/svg+xml;base64,") + QString::fromLatin1(svg.toUtf8().toBase64());
	}
	catch(const std::exception &)
	{
		return QString();
	}
}

bool same(const QByteArray &a, const QByteArray &b)
{
	if(a.size() != b.size() || a.isEmpty())
		return false;
	unsigned char diff = 0;
	for(qsizetype i = 0; i < a.size(); i++)
		diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
	return diff == 0;
}

bool privateV4(const QHostAddress &a)
{
	const quint32 v = a.toIPv4Address();
	return (v >> 24) == 10 || (v >> 20) == (172u << 4 | 1) || (v >> 16) == (192u << 8 | 168);
}

QString randomKey(int bytes)
{
	QByteArray raw(bytes, Qt::Uninitialized);
	QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()), raw.size() / 4);
	return QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace

SubtitleScreen::SubtitleScreen(QObject *parent)
	: QObject(parent)
{
	// A comment line now and then: phones and proxies keep an idle stream open
	heartbeat.setInterval(15000);
	connect(&heartbeat, &QTimer::timeout, this, [this]() {
		for(const QPointer<QTcpSocket> &s : streams)
			if(s)
				s->write(": \n\n");
	});
}

SubtitleScreen::~SubtitleScreen()
{
	close();
}

bool SubtitleScreen::running() const
{
	return server && server->isListening();
}

// The key, the 6-digit code and this PC's id, made once and kept (renew: new ones)
void SubtitleScreen::loadKeys(bool renew)
{
	if(local_only)
	{
		if(key.isEmpty() || renew)
		{
			key = randomKey(12);
			pair_code = QString::number(100000 + QRandomGenerator::system()->bounded(900000));
			pc_id = randomKey(9);
		}
		return;
	}
	QSettings s(settingsPath(), QSettings::IniFormat);
	s.beginGroup(QStringLiteral("altyazi-ekrani"));
	key = renew ? QString() : SecretStore::unprotect(s.value(QStringLiteral("key")).toString());
	pair_code = renew ? QString() : s.value(QStringLiteral("code")).toString();
	pc_id = s.value(QStringLiteral("id")).toString();
	if(key.size() < 16 || pair_code.size() != 6)
	{
		key = randomKey(12);
		pair_code = QString::number(100000 + QRandomGenerator::system()->bounded(900000));
		s.setValue(QStringLiteral("key"), SecretStore::protect(key));
		s.setValue(QStringLiteral("code"), pair_code);
	}
	if(pc_id.isEmpty())
	{
		pc_id = randomKey(9);
		s.setValue(QStringLiteral("id"), pc_id);
	}
}

// The PC's address on the home network: a private IPv4 of a working Wi-Fi or Ethernet adapter, the
// virtual ones (Hyper-V, WSL, VirtualBox, VMware, VPNs) last
QString SubtitleScreen::homeAddress()
{
	QString best;
	int best_score = -1;
	for(const QNetworkInterface &nic : QNetworkInterface::allInterfaces())
	{
		const auto flags = nic.flags();
		if(!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) || (flags & QNetworkInterface::IsLoopBack))
			continue;
		const QString name = nic.humanReadableName() + QLatin1Char(' ') + nic.name();
		const bool virtual_nic = name.contains(QLatin1String("vEthernet"), Qt::CaseInsensitive) || name.contains(QLatin1String("Virtual"), Qt::CaseInsensitive)
			|| name.contains(QLatin1String("VMware"), Qt::CaseInsensitive) || name.contains(QLatin1String("WSL"), Qt::CaseInsensitive)
			|| name.contains(QLatin1String("Hyper-V"), Qt::CaseInsensitive) || name.contains(QLatin1String("TAP"), Qt::CaseSensitive)
			|| name.contains(QLatin1String("VPN"), Qt::CaseInsensitive);
		const bool physical = nic.type() == QNetworkInterface::Wifi || nic.type() == QNetworkInterface::Ethernet;
		for(const QNetworkAddressEntry &e : nic.addressEntries())
		{
			const QHostAddress ip = e.ip();
			if(ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback() || ip.isLinkLocal())
				continue;
			const int score = (privateV4(ip) ? 4 : 0) + (virtual_nic ? 0 : 2) + (physical ? 1 : 0);
			if(score > best_score)
			{
				best_score = score;
				best = ip.toString();
			}
		}
	}
	return best;
}

void SubtitleScreen::start()
{
	if(!local_only)
	{
		QSettings s(settingsPath(), QSettings::IniFormat);
		s.setValue(QStringLiteral("altyazi-ekrani/on"), true);
	}
	listen();
}

void SubtitleScreen::resume()
{
	QSettings s(settingsPath(), QSettings::IniFormat);
	if(s.value(QStringLiteral("altyazi-ekrani/on"), false).toBool())
		listen();
}

void SubtitleScreen::stop()
{
	if(!local_only)
	{
		QSettings s(settingsPath(), QSettings::IniFormat);
		s.setValue(QStringLiteral("altyazi-ekrani/on"), false);
	}
	close();
	emit stateChanged();
}

void SubtitleScreen::renewCode()
{
	const bool was = running();
	close();
	loadKeys(true);
	oviora::log::info("subtitle-screen", "new pairing code and key");
	if(was)
		listen();
	else
		emit stateChanged();
}

void SubtitleScreen::listen()
{
	if(running())
		return;
	error_text.clear();
	loadKeys(false);
	if(!server)
	{
		server = new QTcpServer(this);
		connect(server, &QTcpServer::newConnection, this, &SubtitleScreen::onConnection);
	}
	const QHostAddress bind = local_only ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(QHostAddress::AnyIPv4);
	bool ok = false;
	for(int i = 0; i < port_tries && !ok; i++)
		ok = server->listen(bind, quint16(first_port + i));
	const QString host = local_only ? QStringLiteral("127.0.0.1") : homeAddress();
	if(!ok || host.isEmpty())
	{
		if(ok)
			server->close();
		error_text = !ok ? tr("Bağlantı noktası açılamadı.") : tr("Bu bilgisayar bir ev ağına bağlı görünmüyor.");
		address.clear();
		qr_code.clear();
		oviora::log::warn("subtitle-screen", !ok ? "no free port" : "no home network address");
		emit stateChanged();
		return;
	}
	// The phone app's question on the network (another program on the port: the app is then paired by address)
	if(!finder)
	{
		finder = new QUdpSocket(this);
		connect(finder, &QUdpSocket::readyRead, this, &SubtitleScreen::onDatagram);
	}
	if(!finder->bind(bind, finder_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
		oviora::log::warn("subtitle-screen", "the finder port is taken; the phone app cannot find this PC by itself");
	address = QStringLiteral("http://%1:%2/?k=%3").arg(host).arg(server->serverPort()).arg(key);
	qr_code = qrSvg(address);
	heartbeat.start();
	// The address names the key: only where it is and its port go to the log
	oviora::log::info("subtitle-screen", std::format("serving on {}:{}", host.toStdString(), server->serverPort()));
	emit stateChanged();
}

void SubtitleScreen::close()
{
	heartbeat.stop();
	const bool had = !streams.isEmpty();
	for(const QPointer<QTcpSocket> &s : streams)
		if(s)
			s->abort();
	streams.clear();
	if(finder)
		finder->close();
	if(server && server->isListening())
	{
		server->close();
		oviora::log::info("subtitle-screen", "stopped");
	}
	address.clear();
	qr_code.clear();
	if(had)
		emit clientsChanged();
}

void SubtitleScreen::setText(const QString &text, const QString &source)
{
	if(text == current && source == current_source)
		return;
	current = text;
	current_source = source;
	serial++;
	if(streams.isEmpty())
		return;
	const QByteArray e = event();
	for(const QPointer<QTcpSocket> &s : streams)
		if(s)
		{
			s->write(e);
			s->flush();
		}
}

QByteArray SubtitleScreen::event() const
{
	const QByteArray json = QJsonDocument(QJsonObject{ { "t", current }, { "s", current_source }, { "n", double(serial) } })
		.toJson(QJsonDocument::Compact);
	return "data: " + json + "\n\n";
}

// "OVIORA-ALTYAZI-ARA 1" from the phone app: this PC's id, name and port, to the asker only
void SubtitleScreen::onDatagram()
{
	while(finder && finder->hasPendingDatagrams())
	{
		const QNetworkDatagram d = finder->receiveDatagram(512);
		if(!d.isValid() || !d.data().startsWith("OVIORA-ALTYAZI-ARA"))
			continue;
		const QByteArray answer = QJsonDocument(QJsonObject{ { "oviora", "altyazi" }, { "v", 1 }, { "id", pc_id },
			{ "name", QHostInfo::localHostName().left(64) }, { "port", int(server ? server->serverPort() : 0) } })
			.toJson(QJsonDocument::Compact);
		finder->writeDatagram(answer, d.senderAddress(), quint16(d.senderPort()));
	}
}

void SubtitleScreen::onConnection()
{
	while(QTcpSocket *s = server->nextPendingConnection())
	{
		s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		s->setProperty("req", QByteArray());
		connect(s, &QTcpSocket::readyRead, this, [this, s]() { onReadable(s); });
		connect(s, &QTcpSocket::disconnected, this, [this, s]() {
			const qsizetype before = streams.size();
			streams.removeIf([s](const QPointer<QTcpSocket> &p) { return !p || p == s; });
			if(streams.size() != before)
				emit clientsChanged();
			s->deleteLater();
		});
		// A request that never finishes is dropped
		QTimer::singleShot(10000, s, [s]() { if(!s->property("streaming").toBool() && s->property("req").isValid()) s->abort(); });
	}
}

bool SubtitleScreen::pairingAllowed(const QString &ip)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	QList<qint64> &list = wrong_codes[ip];
	list.removeIf([now](qint64 t) { return t < now - wrong_code_window_ms; });
	return list.size() < max_wrong_codes;
}

void SubtitleScreen::onReadable(QTcpSocket *s)
{
	if(s->property("streaming").toBool())
	{
		s->readAll();
		return;
	}
	QByteArray req = s->property("req").toByteArray() + s->readAll();
	if(req.size() > max_request)
	{
		s->abort();
		return;
	}
	if(!req.contains("\r\n\r\n"))
	{
		s->setProperty("req", req);
		return;
	}
	s->setProperty("req", QVariant());
	const QList<QByteArray> line = req.left(req.indexOf("\r\n")).split(' ');
	if(line.size() < 3 || line.at(0) != "GET")
	{
		respond(s, "405 Method Not Allowed", "text/plain", "");
		return;
	}
	const QByteArray target = line.at(1);
	const qsizetype q = target.indexOf('?');
	const QByteArray path = q < 0 ? target : target.left(q);
	const QUrlQuery query(QString::fromLatin1(q < 0 ? QByteArray() : target.mid(q + 1)));
	// The phone app pairs once with the code shown on this PC and keeps the key it gets
	if(path == "/eslestir")
	{
		const QString ip = s->peerAddress().toString();
		if(!pairingAllowed(ip))
		{
			respond(s, "429 Too Many Requests", "text/plain", "");
			return;
		}
		if(!same(query.queryItemValue(QStringLiteral("c")).toLatin1(), pair_code.toLatin1()))
		{
			wrong_codes[ip].append(QDateTime::currentMSecsSinceEpoch());
			oviora::log::info("subtitle-screen", "wrong pairing code");
			respond(s, "403 Forbidden", "text/plain", "");
			return;
		}
		wrong_codes.remove(ip);
		oviora::log::info("subtitle-screen", "phone paired");
		respond(s, "200 OK", "application/json", QJsonDocument(QJsonObject{ { "k", key }, { "id", pc_id },
			{ "name", QHostInfo::localHostName().left(64) } }).toJson(QJsonDocument::Compact));
		return;
	}
	if(!same(query.queryItemValue(QStringLiteral("k")).toLatin1(), key.toLatin1()))
	{
		respond(s, "403 Forbidden", "text/plain; charset=utf-8", "Oviora");
		return;
	}
	if(path == "/")
		respond(s, "200 OK", "text/html; charset=utf-8", page());
	else if(path == "/olay")
		openStream(s);
	else
		respond(s, "404 Not Found", "text/plain", "");
}

void SubtitleScreen::respond(QTcpSocket *s, const QByteArray &status, const QByteArray &type, const QByteArray &body)
{
	s->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size())
		+ "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n" + body);
	s->disconnectFromHost();
}

void SubtitleScreen::openStream(QTcpSocket *s)
{
	streams.removeIf([](const QPointer<QTcpSocket> &p) { return !p; });
	if(streams.size() >= max_phones)
	{
		respond(s, "503 Service Unavailable", "text/plain", "");
		return;
	}
	s->setProperty("streaming", true);
	// retry: a dropped stream comes back after 1 s (a browser does it by itself); the line on screen now first
	s->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
		"X-Accel-Buffering: no\r\nConnection: keep-alive\r\n\r\nretry: 1000\n\n" + event());
	s->flush();
	streams.append(s);
	oviora::log::info("subtitle-screen", std::format("phone connected ({} open)", streams.size()));
	emit clientsChanged();
}

// A browser's page (phones without the Oviora app): the newest line large at the top, the ones before it under
// it, smaller and greyer; a line stays until the next one comes. A-/A+ (kept on the phone), a tap shows them;
// "Başlat" goes fullscreen and keeps the screen on where the browser allows it
QByteArray SubtitleScreen::page() const
{
	QString html = QStringLiteral(R"HTML(<!doctype html>
<html lang="tr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#000000"><title>Oviora</title>
<style>
html,body{margin:0;height:100%;background:#000;color:#fff;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;overflow:hidden;-webkit-user-select:none;user-select:none}
#lines{position:fixed;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:flex-start;gap:1.2vh;text-align:center;padding:5vh 5vw;box-sizing:border-box}
.l{font-weight:600;line-height:1.2;white-space:pre-line;text-shadow:0 2px 6px #000;transition:all .25s}
#wait{color:#6f6a80;font-size:5vw;margin-top:30vh}
#bar{position:fixed;left:0;right:0;bottom:0;display:flex;gap:10px;justify-content:center;align-items:center;padding:14px;background:linear-gradient(transparent,rgba(0,0,0,.85));transition:opacity .3s}
#bar.off{opacity:0;pointer-events:none}
button{background:#2a2734;color:#fff;border:0;border-radius:999px;padding:12px 20px;font-size:17px}
#dot{width:10px;height:10px;border-radius:5px;background:#e5a13a;margin-right:6px}
#dot.on{background:#3ecf8e}
#start{position:fixed;inset:0;display:flex;flex-direction:column;gap:18px;align-items:center;justify-content:center;background:#000;font-size:18px;color:#bbb}
#start button{background:#7b5cff;font-size:20px;padding:16px 34px}
</style></head><body>
<div id="lines"><div id="wait">%WAIT%</div></div>
<div id="bar" class="off"><span id="dot"></span><button id="minus">A−</button><button id="plus">A+</button><button id="full">%FULL%</button></div>
<div id="start"><div>Oviora</div><button id="go">%START%</button></div>
<script>
const K=new URLSearchParams(location.search).get('k')||'';
const box=document.getElementById('lines'),bar=document.getElementById('bar'),dot=document.getElementById('dot');
let size=parseFloat(localStorage.getItem('ovS')||'6');
const hist=[];const KEEP=4;
function render(){
 if(!hist.length)return;box.textContent='';
 hist.forEach((t,i)=>{const d=document.createElement('div');d.className='l';d.textContent=t;
  const f=Math.max(0.35,1-i*0.22);d.style.fontSize=(size*f)+'vw';d.style.color=i?'rgba(200,198,210,'+Math.max(0.25,0.8-i*0.18)+')':'#fff';
  box.appendChild(d)});
}
function setSize(){localStorage.setItem('ovS',size);render()}
document.getElementById('minus').onclick=e=>{e.stopPropagation();size=Math.max(3,size-0.5);setSize();showBar()};
document.getElementById('plus').onclick=e=>{e.stopPropagation();size=Math.min(14,size+0.5);setSize();showBar()};
document.getElementById('full').onclick=e=>{e.stopPropagation();goFull()};
let hideT=0;function showBar(){bar.classList.remove('off');clearTimeout(hideT);hideT=setTimeout(()=>bar.classList.add('off'),3000)}
document.body.addEventListener('click',showBar);
function goFull(){const d=document.documentElement;(d.requestFullscreen||d.webkitRequestFullscreen||function(){}).call(d)}
let lock=null,keep=null;
async function awake(){
 try{if('wakeLock' in navigator){lock=await navigator.wakeLock.request('screen');return}}catch(e){}
 if(keep)return;
 try{const c=document.createElement('canvas');c.width=c.height=2;const g=c.getContext('2d');let i=0;
  setInterval(()=>{g.fillStyle=(i++%2)?'#000':'#010101';g.fillRect(0,0,2,2)},1000);
  keep=document.createElement('video');keep.muted=true;keep.playsInline=true;keep.setAttribute('playsinline','');
  keep.srcObject=c.captureStream(1);keep.style.cssText='position:fixed;left:0;top:0;width:2px;height:2px;opacity:.01';
  document.body.appendChild(keep);await keep.play()}catch(e){}
}
document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='visible'&&lock)awake()});
document.getElementById('go').onclick=()=>{document.getElementById('start').remove();goFull();awake();showBar()};
let last=-1;
function connect(){
 const es=new EventSource('/olay?k='+encodeURIComponent(K));
 es.onopen=()=>dot.classList.add('on');
 es.onmessage=m=>{try{const d=JSON.parse(m.data);if(d.n===last)return;last=d.n;
  const t=(d.t||'').trim();if(!t||t===hist[0])return;hist.unshift(t);hist.length=Math.min(hist.length,KEEP);render()}catch(e){}};
 es.onerror=()=>{dot.classList.remove('on')};
}
connect();
</script></body></html>)HTML");
	// Translated words go into HTML only (never into the script), escaped
	html.replace(QStringLiteral("%WAIT%"), tr("Altyazı bekleniyor…").toHtmlEscaped());
	html.replace(QStringLiteral("%FULL%"), tr("Tam ekran").toHtmlEscaped());
	html.replace(QStringLiteral("%START%"), tr("Başlat").toHtmlEscaped());
	return html.toUtf8();
}
