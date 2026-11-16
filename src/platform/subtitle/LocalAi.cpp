// Oviora: the local translation AI, ported from the old Oviora (localai.cpp)

#include "LocalAi.h"
#include "OllamaHashes.h"

#include <algorithm>

#include <QCoreApplication>
#include <QHostAddress>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCryptographicHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <zlib.h>

Q_LOGGING_CATEGORY(ovioraLocalAi, "oviora.localai")

// The Ollama release this build was tested with; its zip holds every GPU backend (~3 GB unpacked)
static const char *RUNTIME_ZIP_URL = "https://github.com/ollama/ollama/releases/download/v0.40.15/ollama-windows-amd64.zip";
static const char *MODEL = "translategemma:4b";
static const char *SERVER = "http://127.0.0.1:11434";
static const int MAX_RETRIES = 8;

static QString mb(quint64 bytes) { return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 0); }
static QString gb(quint64 bytes) { return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1).replace('.', ','); }

LocalAi::LocalAi(Hooks hooks, QObject *parent)
	: QObject(parent), hooks(std::move(hooks)), net(new QNetworkAccessManager(this))
{
	QTimer::singleShot(0, this, &LocalAi::refresh);
	if(qEnvironmentVariableIsSet("OVIORA_LOCALAI_TEST_INSTALL"))
		QTimer::singleShot(4000, this, &LocalAi::install);
}

LocalAi::~LocalAi()
{
	cancelled = true;
	if(reply)
		reply->abort();
	stopOwnServer();
	if(zs)
	{
		inflateEnd(zs);
		delete zs;
	}
	delete out;
}

QString LocalAi::dir() const
{
	return QCoreApplication::applicationDirPath() + QStringLiteral("/ollama");
}

QString LocalAi::gpu() const
{
	QString vendor = hooks.gpu_vendor ? hooks.gpu_vendor() : QString();
	if(vendor == "nvidia")
		return tr("NVIDIA (CUDA)");
	if(vendor == "amd")
		return tr("AMD (ROCm)");
	return tr("Vulkan (yavaş olabilir)");
}

bool LocalAi::runtimeReady() const
{
	return ollamaRuntimeVerified(dir());
}

bool LocalAi::modelReady() const
{
	return QFileInfo::exists(dir() + "/models/manifests/registry.ollama.ai/library/translategemma/4b");
}

void LocalAi::setState(const QString &state, const QString &status, double progress)
{
	current_state = state;
	status_text = status;
	current_progress = progress;
	emit changed();
}

void LocalAi::fail(const QString &message)
{
	qCWarning(ovioraLocalAi) << message;
	stopOwnServer();
	setState("error", message);
}

void LocalAi::refresh()
{
	if(current_state == "working")
		return;
	// Downloaded next to Oviora and verified file by file (an older or unverified folder installs again:
	// matching files are kept, the rest downloaded)
	if(runtimeReady() && modelReady())
	{
		setState("installed", tr("Kurulu. Altyazılar yerel yapay zeka ve Google ile yarışarak çevrilir."));
		return;
	}
	// An Ollama installed on this PC that already has the model counts too
	QNetworkReply *r = net->get(QNetworkRequest(QUrl(QString(SERVER) + "/api/tags")));
	r->setProperty("probe", true);
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		r->deleteLater();
		bool has_model = false;
		if(r->error() == QNetworkReply::NoError)
			for(const QJsonValue &m : QJsonDocument::fromJson(r->readAll()).object().value("models").toArray())
				if(m.toObject().value("name").toString().startsWith("translategemma:4b"))
					has_model = true;
		if(current_state == "working")
			return;
		if(has_model)
			setState("system", tr("Bu bilgisayarda kurulu Ollama kullanılıyor."));
		else
			setState("missing", tr("Kurulu değil. Altyazılar Google ile çevrilir."));
	});
}

void LocalAi::install()
{
	if(current_state == "working")
		return;
	cancelled = false;
	QDir().mkpath(dir());
	// Ollama already here (downloaded earlier, or installed on this PC and running): only the model is missing
	QTcpSocket probe;
	probe.connectToHost(QHostAddress::LocalHost, 11434);
	if(runtimeReady() || probe.waitForConnected(400))
	{
		startServer();
		return;
	}
	QString vendor = qEnvironmentVariable("OVIORA_LOCALAI_TEST_GPU");  // testing: force a backend
	if(vendor.isEmpty() && hooks.gpu_vendor)
		vendor = hooks.gpu_vendor();
	gpu_dir = vendor == "nvidia" ? "cuda_v13" : vendor == "amd" ? "rocm" : "vulkan";
	setState("working", tr("Ollama dosya listesi alınıyor…"));
	zip_size = 0;
	entry_retries = 0;
	fetchTail();
}

void LocalAi::cancel()
{
	cancelled = true;
	if(reply)
		reply->abort();
	stopOwnServer();
	if(zs)
	{
		inflateEnd(zs);
		delete zs;
		zs = nullptr;
	}
	if(out)
	{
		out->close();
		delete out;
		out = nullptr;
	}
	current_state.clear();
	refresh();
}

void LocalAi::uninstall()
{
	if(current_state == "working")
		return;
	if(hooks.stop_server)
		hooks.stop_server();
	stopOwnServer();
	// Only the folder next to Oviora; an Ollama installed on the PC is left alone
	QTimer::singleShot(1500, this, [this]() {
		QDir(dir()).removeRecursively();
		current_state.clear();
		refresh();
	});
}

QNetworkReply *LocalAi::getRange(quint64 from, qint64 to)
{
	QNetworkRequest req{QUrl(RUNTIME_ZIP_URL)};
	req.setRawHeader("Range", to < 0 ? QByteArray("bytes=") + QByteArray::number(from) + "-"
		: QByteArray("bytes=") + QByteArray::number(from) + "-" + QByteArray::number(to));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	req.setTransferTimeout(60000);
	return net->get(req);
}

// ---------------------------------------------------------------- zip directory

void LocalAi::fetchTail()
{
	// GitHub's asset host refuses suffix ranges (bytes=-N, HTTP 501): learn the size from a one-byte
	// range first, then ask for the last 128 KB with both ends
	if(zip_size == 0)
	{
		QNetworkRequest probe{QUrl(RUNTIME_ZIP_URL)};
		probe.setRawHeader("Range", "bytes=0-0");
		probe.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
		probe.setTransferTimeout(60000);
		reply = net->get(probe);
		connect(reply, &QNetworkReply::finished, this, [this, r = reply.data()]() {
			r->deleteLater();
			if(cancelled)
				return;
			QByteArray range = r->rawHeader("Content-Range"); // bytes 0-0/total
			quint64 total = range.mid(range.lastIndexOf('/') + 1).toULongLong();
			if(r->error() != QNetworkReply::NoError || total < 1024)
			{
				if(++entry_retries <= MAX_RETRIES)
				{
					QTimer::singleShot(3000, this, &LocalAi::fetchTail);
					return;
				}
				fail(tr("Ollama indirilemedi (GitHub'a ulaşılamıyor: %1). Sonra tekrar dene.").arg(r->errorString()));
				return;
			}
			zip_size = total;
			fetchTail();
		});
		return;
	}
	QNetworkRequest req{QUrl(RUNTIME_ZIP_URL)};
	const quint64 tail_from = zip_size > 131072 ? zip_size - 131072 : 0;
	req.setRawHeader("Range", "bytes=" + QByteArray::number(tail_from) + "-" + QByteArray::number(zip_size - 1));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	req.setTransferTimeout(60000);
	reply = net->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, r = reply.data()]() {
		r->deleteLater();
		if(cancelled)
			return;
		if(r->error() != QNetworkReply::NoError || r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206)
		{
			if(++entry_retries <= MAX_RETRIES)
			{
				QTimer::singleShot(3000, this, &LocalAi::fetchTail);
				return;
			}
			fail(tr("Ollama indirilemedi (GitHub'a ulaşılamıyor: %1). Sonra tekrar dene.").arg(r->errorString()));
			return;
		}
		entry_retries = 0;
		QByteArray tail = r->readAll();
		QByteArray range = r->rawHeader("Content-Range"); // bytes a-b/total
		zip_size = range.mid(range.lastIndexOf('/') + 1).toULongLong();
		if(zip_size < quint64(tail.size()))
		{
			fail(tr("Ollama paketi beklenmedik biçimde."));
			return;
		}
		quint64 tail_start = zip_size - quint64(tail.size());

		int eocd = tail.lastIndexOf(QByteArray::fromHex("504b0506"));
		if(eocd < 0)
		{
			fail(tr("Ollama paketi beklenmedik biçimde."));
			return;
		}
		quint64 cd_size = qFromLittleEndian<quint32>(tail.constData() + eocd + 12);
		quint64 cd_offset = qFromLittleEndian<quint32>(tail.constData() + eocd + 16);
		if(cd_offset == 0xffffffffu || cd_size == 0xffffffffu)
		{
			// Zip64: the locator just before the end record points at the zip64 end record
			int loc = tail.lastIndexOf(QByteArray::fromHex("504b0607"), eocd);
			if(loc < 0)
			{
				fail(tr("Ollama paketi beklenmedik biçimde."));
				return;
			}
			quint64 z64 = qFromLittleEndian<quint64>(tail.constData() + loc + 8);
			if(z64 < tail_start)
			{
				fail(tr("Ollama paketi beklenmedik biçimde."));
				return;
			}
			const char *rec = tail.constData() + (z64 - tail_start);
			cd_size = qFromLittleEndian<quint64>(rec + 40);
			cd_offset = qFromLittleEndian<quint64>(rec + 48);
		}
		if(cd_offset >= tail_start)
			parseCentralDirectory(tail.mid(int(cd_offset - tail_start), int(cd_size)));
		else
			fetchCentralDirectory(cd_offset, cd_size);
	});
}

void LocalAi::fetchCentralDirectory(quint64 offset, quint64 size)
{
	reply = getRange(offset, qint64(offset + size - 1));
	connect(reply, &QNetworkReply::finished, this, [this, r = reply.data(), offset, size]() {
		r->deleteLater();
		if(cancelled)
			return;
		if(r->error() != QNetworkReply::NoError)
		{
			if(++entry_retries <= MAX_RETRIES)
			{
				QTimer::singleShot(3000, this, [this, offset, size]() { fetchCentralDirectory(offset, size); });
				return;
			}
			fail(tr("Ollama indirilemedi: %1").arg(r->errorString()));
			return;
		}
		entry_retries = 0;
		parseCentralDirectory(r->readAll());
	});
}

void LocalAi::parseCentralDirectory(const QByteArray &cd)
{
	entries.clear();
	QStringList gpu_dirs;
	int pos = 0;
	while(pos + 46 <= cd.size() && qFromLittleEndian<quint32>(cd.constData() + pos) == 0x02014b50)
	{
		const char *h = cd.constData() + pos;
		Entry e;
		e.method = qFromLittleEndian<quint16>(h + 10);
		e.compressed = qFromLittleEndian<quint32>(h + 20);
		e.size = qFromLittleEndian<quint32>(h + 24);
		quint16 nlen = qFromLittleEndian<quint16>(h + 28);
		quint16 elen = qFromLittleEndian<quint16>(h + 30);
		quint16 clen = qFromLittleEndian<quint16>(h + 32);
		e.local_offset = qFromLittleEndian<quint32>(h + 42);
		e.name = QString::fromUtf8(h + 46, nlen).replace('\\', '/');
		// Zip64 extra field holds whichever sizes did not fit, in this order
		const char *x = h + 46 + nlen;
		for(int xp = 0; xp + 4 <= elen;)
		{
			quint16 id = qFromLittleEndian<quint16>(x + xp);
			quint16 len = qFromLittleEndian<quint16>(x + xp + 2);
			if(id == 0x0001)
			{
				int p = xp + 4;
				if(e.size == 0xffffffffu) { e.size = qFromLittleEndian<quint64>(x + p); p += 8; }
				if(e.compressed == 0xffffffffu) { e.compressed = qFromLittleEndian<quint64>(x + p); p += 8; }
				if(e.local_offset == 0xffffffffu) { e.local_offset = qFromLittleEndian<quint64>(x + p); }
			}
			xp += 4 + len;
		}
		pos += 46 + nlen + elen + clen;

		if(e.name.endsWith('/'))
			continue;
		// Never outside the Ollama folder (an entry like ../x or C:/x is skipped)
		if(e.name.startsWith('/') || e.name.contains(':') || e.name.split('/').contains(".."))
			continue;
		if(e.name.startsWith("lib/ollama/"))
		{
			QString rest = e.name.mid(11);
			int slash = rest.indexOf('/');
			if(slash >= 0)
			{
				QString sub = rest.left(slash);
				if(!gpu_dirs.contains(sub))
					gpu_dirs.append(sub);
				// AMD: the zip names the ROCm folder with its version
				if(!(sub == gpu_dir || (gpu_dir == "rocm" && sub.startsWith("rocm"))))
					continue;
			}
			entries.append(e);
		}
		else if(e.name == "ollama.exe")
			entries.append(e);
	}
	qCInfo(ovioraLocalAi) << "GPU folders in the zip" << gpu_dirs << "taking" << gpu_dir << "files" << entries.size();
	if(entries.isEmpty() || std::none_of(entries.begin(), entries.end(), [](const Entry &e) { return e.name == "ollama.exe"; }))
	{
		fail(tr("Ollama paketinde gerekli dosyalar bulunamadı."));
		return;
	}
	bytes_total = 0;
	bytes_done = 0;
	for(const Entry &e : entries)
		bytes_total += e.compressed;
	entry_index = 0;
	nextEntry();
}

// ---------------------------------------------------------------- entries

void LocalAi::nextEntry()
{
	if(cancelled)
		return;
	// Files finished in an earlier attempt are kept
	while(entry_index < entries.size())
	{
		const Entry &e = entries[entry_index];
		QFileInfo fi(dir() + "/" + e.name);
		if(!fi.exists() || quint64(fi.size()) != e.size || !ollamaFileMatches(fi.filePath(), e.name))
			break;
		bytes_done += e.compressed;
		entry_index++;
	}
	if(entry_index >= entries.size())
	{
		{
			QFile mark(dir() + "/runtime.ok");
			if(mark.open(QIODevice::WriteOnly | QIODevice::Truncate))
				mark.write(OLLAMA_RUNTIME_MARK);
		}
		qCInfo(ovioraLocalAi) << "Runtime ready";
		if(qEnvironmentVariableIsSet("OVIORA_LOCALAI_TEST_RUNTIME_ONLY"))
		{
			setState("missing", tr("Test: Ollama dosyaları indi, model atlandı."));
			return;
		}
		startServer();
		return;
	}
	const Entry &e = entries[entry_index];
	// The local header's name and extra lengths can differ from the central directory's
	reply = getRange(e.local_offset, qint64(e.local_offset + 29));
	connect(reply, &QNetworkReply::finished, this, [this, r = reply.data()]() {
		r->deleteLater();
		if(cancelled)
			return;
		QByteArray h = r->readAll();
		if(r->error() != QNetworkReply::NoError || h.size() < 30 || qFromLittleEndian<quint32>(h.constData()) != 0x04034b50)
		{
			if(++entry_retries <= MAX_RETRIES)
			{
				QTimer::singleShot(3000, this, &LocalAi::nextEntry);
				return;
			}
			fail(tr("Ollama indirilemedi: %1").arg(r->errorString()));
			return;
		}
		const Entry &e = entries[entry_index];
		quint64 data_offset = e.local_offset + 30 + qFromLittleEndian<quint16>(h.constData() + 26) + qFromLittleEndian<quint16>(h.constData() + 28);

		QString path = dir() + "/" + e.name;
		QDir().mkpath(QFileInfo(path).absolutePath());
		delete out;
		out = new QFile(path + ".part");
		if(!out->open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			fail(tr("Dosya yazılamadı: %1").arg(path));
			return;
		}
		if(zs)
		{
			inflateEnd(zs);
			delete zs;
			zs = nullptr;
		}
		if(e.method == 8)
		{
			zs = new z_stream_s{};
			inflateInit2(zs, -MAX_WBITS); // raw deflate, as stored in zip files
		}
		entry_received = 0;
		fetchEntryData(data_offset);
	});
}

void LocalAi::fetchEntryData(quint64 data_offset)
{
	entry_data_offset = data_offset;
	const Entry &e = entries[entry_index];
	if(e.compressed == 0)
	{
		finishEntry();
		return;
	}
	reply = getRange(data_offset + entry_received, qint64(data_offset + e.compressed - 1));
	connect(reply, &QNetworkReply::readyRead, this, [this, r = reply.data()]() {
		if(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206)
			return;
		QByteArray chunk = r->readAll();
		entry_received += quint64(chunk.size());
		bytes_done += quint64(chunk.size());
		if(zs)
		{
			zs->next_in = reinterpret_cast<Bytef *>(chunk.data());
			zs->avail_in = uInt(chunk.size());
			char buf[1 << 16];
			do
			{
				zs->next_out = reinterpret_cast<Bytef *>(buf);
				zs->avail_out = sizeof(buf);
				int ret = inflate(zs, Z_NO_FLUSH);
				if(ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
				{
					r->abort();
					return;
				}
				out->write(buf, qint64(sizeof(buf) - zs->avail_out));
			} while(zs->avail_out == 0);
		}
		else
			out->write(chunk);
		setState("working", tr("Ollama indiriliyor (%1): %2 / %3 MB").arg(gpu()).arg(mb(bytes_done)).arg(mb(bytes_total)),
			bytes_total ? double(bytes_done) / double(bytes_total) : -1);
	});
	connect(reply, &QNetworkReply::finished, this, [this, r = reply.data()]() {
		r->deleteLater();
		if(cancelled)
			return;
		const Entry &e = entries[entry_index];
		if(entry_received >= e.compressed)
		{
			finishEntry();
			return;
		}
		// Connection dropped mid-file: continue from the next compressed byte
		if(++entry_retries <= MAX_RETRIES)
		{
			qCWarning(ovioraLocalAi) << "Range request ended early, continuing" << e.name << r->errorString();
			QTimer::singleShot(3000, this, [this]() { fetchEntryData(entry_data_offset); });
			return;
		}
		fail(tr("Ollama indirilemedi: %1").arg(r->errorString()));
	});
}

void LocalAi::finishEntry()
{
	const Entry &e = entries[entry_index];
	out->close();
	QString path = dir() + "/" + e.name;
	if(zs)
	{
		inflateEnd(zs);
		delete zs;
		zs = nullptr;
	}
	if(quint64(out->size()) != e.size)
	{
		out->remove();
		fail(tr("İndirilen dosya bozuk: %1").arg(e.name));
		return;
	}
	// Every file that will run must be exactly the release's (OllamaHashes.h), nothing else
	if(!ollamaFileMatches(out->fileName(), e.name))
	{
		out->remove();
		qCWarning(ovioraLocalAi) << "hash mismatch, not installed:" << e.name;
		fail(tr("İndirilen dosya bozuk: %1").arg(e.name));
		return;
	}
	QFile::remove(path);
	out->rename(path);
	delete out;
	out = nullptr;
	entry_retries = 0;
	entry_index++;
	nextEntry();
}

// ---------------------------------------------------------------- server and model

void LocalAi::startServer()
{
	if(cancelled)
		return;
	QTcpSocket probe;
	probe.connectToHost(QHostAddress::LocalHost, 11434);
	if(!probe.waitForConnected(400))
	{
		if(!runtimeReady())
		{
			fail(tr("Yerel yapay zeka dosyaları doğrulanamadı; yeniden kur."));
			return;
		}
		server = new QProcess(this);
		QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
		env.insert("OLLAMA_MODELS", QDir::toNativeSeparators(dir() + "/models"));
		env.insert("OLLAMA_HOST", "127.0.0.1:11434");
		server->setProcessEnvironment(env);
		server->setWorkingDirectory(dir());
		server->start(dir() + "/ollama.exe", { "serve" });
		if(!server->waitForStarted(5000))
		{
			fail(tr("Yerel çeviri sunucusu başlatılamadı."));
			return;
		}
	}
	setState("working", tr("Model indirmeye hazırlanılıyor…"));
	QTimer::singleShot(server ? 3000 : 0, this, &LocalAi::pullModel);
}

void LocalAi::pullModel()
{
	if(cancelled)
		return;
	QNetworkRequest req{QUrl(QString(SERVER) + "/api/pull")};
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	req.setTransferTimeout(0);
	pull_buffer.clear();
	reply = net->post(req, QJsonDocument(QJsonObject{ { "model", MODEL }, { "stream", true } }).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::readyRead, this, [this, r = reply.data()]() {
		pull_buffer += r->readAll();
		int nl;
		while((nl = pull_buffer.indexOf('\n')) >= 0)
		{
			QJsonObject o = QJsonDocument::fromJson(pull_buffer.left(nl)).object();
			pull_buffer.remove(0, nl + 1);
			if(o.contains("error"))
			{
				fail(tr("Model indirilemedi: %1").arg(o.value("error").toString()));
				r->abort();
				return;
			}
			double total = o.value("total").toDouble(), done = o.value("completed").toDouble();
			if(total > 100e6)
				setState("working", tr("Çeviri modeli indiriliyor: %1 / %2 GB").arg(gb(quint64(done))).arg(gb(quint64(total))), done / total);
			else if(o.value("status").toString() == "success")
				setState("working", tr("Model hazırlanıyor…"), 1);
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, r = reply.data()]() {
		r->deleteLater();
		if(cancelled || current_state == "error")
			return;
		if(r->error() != QNetworkReply::NoError)
		{
			// Ollama resumes the layers it already has; try again a few times on a flaky line
			if(++entry_retries <= MAX_RETRIES)
			{
				QTimer::singleShot(5000, this, &LocalAi::pullModel);
				return;
			}
			fail(tr("Model indirilemedi: %1").arg(r->errorString()));
			return;
		}
		entry_retries = 0;
		stopOwnServer();
		current_state.clear();
		refresh();
		if(hooks.installed)
			hooks.installed();
	});
}

void LocalAi::stopOwnServer()
{
	if(!server)
		return;
	server->kill();
	server->waitForFinished(3000);
	delete server;
	server = nullptr;
}
