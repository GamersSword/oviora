// Oviora: the local translation AI (Ollama + TranslateGemma) is an optional download from the
// subtitle settings instead of part of the package. Only the Ollama files this PC needs are taken
// out of the official release zip (HTTP range requests: base files plus one GPU backend), then the
// model is pulled through Ollama itself. Both steps continue where they stopped.

#ifndef OVIORA_LOCALAI_H
#define OVIORA_LOCALAI_H

#include <QObject>
#include <QPointer>
#include <QList>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QFile;
struct z_stream_s;

class LocalAi : public QObject
{
	Q_OBJECT
	Q_PROPERTY(QString state READ state NOTIFY changed)          // missing, system, installed, working, error
	Q_PROPERTY(double progress READ progress NOTIFY changed)     // 0..1 of the current step, -1 unknown
	Q_PROPERTY(QString status READ status NOTIFY changed)
	Q_PROPERTY(QString gpu READ gpu NOTIFY changed)

public:
	struct Hooks
	{
		/** "nvidia", "amd" or "other": picks the GPU backend to download */
		std::function<QString()> gpu_vendor;
		/** The subtitle translator's own Ollama process must be stopped before files are removed */
		std::function<void()> stop_server;
		/** Called once the model is ready, e.g. to switch translation to local + Google */
		std::function<void()> installed;
	};

	LocalAi(Hooks hooks, QObject *parent);
	~LocalAi();

	QString state() const { return current_state; }
	double progress() const { return current_progress; }
	QString status() const { return status_text; }
	QString gpu() const;

	Q_INVOKABLE void refresh();
	Q_INVOKABLE void install();
	Q_INVOKABLE void cancel();
	Q_INVOKABLE void uninstall();

signals:
	void changed();

private:
	struct Entry
	{
		QString name;
		quint64 local_offset = 0, compressed = 0, size = 0;
		quint16 method = 0;
	};

	void setState(const QString &state, const QString &status, double progress = -1);
	void fail(const QString &message);
	QString dir() const;
	bool runtimeReady() const;
	bool modelReady() const;

	// Step 1: Ollama files out of the release zip
	void fetchTail();
	void fetchCentralDirectory(quint64 offset, quint64 size);
	void parseCentralDirectory(const QByteArray &cd);
	void nextEntry();
	void fetchEntryData(quint64 data_offset);
	void finishEntry();
	QNetworkReply *getRange(quint64 from, qint64 to);   // to < 0: until the end

	// Step 2 and 3: a running Ollama and the model pull
	void startServer();
	void pullModel();
	void stopOwnServer();

	Hooks hooks;
	QNetworkAccessManager *net;
	QPointer<QNetworkReply> reply;
	QProcess *server = nullptr;
	QString current_state, status_text;
	double current_progress = -1;
	bool cancelled = false;

	quint64 zip_size = 0;
	QString gpu_dir;
	QList<Entry> entries;
	int entry_index = 0;
	quint64 bytes_total = 0, bytes_done = 0;
	// The entry being written: inflate state survives a dropped connection, the next range
	// request continues at the first compressed byte not yet received
	QFile *out = nullptr;
	z_stream_s *zs = nullptr;
	quint64 entry_data_offset = 0, entry_received = 0;
	int entry_retries = 0;
	QByteArray pull_buffer;
};

#endif // OVIORA_LOCALAI_H
