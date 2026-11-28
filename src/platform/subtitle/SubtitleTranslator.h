// Oviora: in-game subtitle translation, ported from the old Oviora (subtitletranslator.cpp).
// A few times a second the engine copies the subtitle region of the processed PS5 frames to the
// CPU (engine/SubtitleTap.h); PaddleOCR reads it on a worker thread, the line is checked to look
// like dialogue, looked up in the dictionaries, this run's cache and the shared translation memory
// on the Oviora server, else translated (Google / DeepL / MyMemory / local model), and the Turkish
// is drawn over the original on a layered child of the video window. Only the game's text is sent.
// QML: the context property "subtitles".
#pragma once

#include <QAtomicInteger>
#include <QHash>
#include <QMultiHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQmlPropertyMap>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QVariantList>

#include "GameGlossary.h"

#include <windows.h>

class QThread;
class QTimer;
class OvioraServer;
class LocalAi;
class TranslationMemory;

namespace oviora::engine { class EngineController; }

namespace oviora::subtitle {

// Snapshot of the settings the worker needs for one sample
struct Params
{
    QRectF region;              // normalized (0..1) rect inside the video frame
    int threshold = 180;        // brightness threshold (0..255) for text pixels
    bool requireOutline = true; // text pixels need a dark outline/shadow nearby
    bool centeredOnly = true;   // only accept horizontally centered lines
    bool debugDump = false;     // save region images + OCR log under logs/altyazi
    int engine = 0;             // 0 Google, 1 DeepL, 2 local model, 3 MyMemory (fallback), 4 local + Google
    QString deeplKey, llmUrl, llmModel, llmKey;
    QString sourceLang, targetLang;
    std::shared_ptr<const Glossary> glossary; // the running game's term lists (runtime/altyazi/terim), may be null
};

class Worker;
class Overlay;

// --test-terms: sample lines through the term lists of titleId and Google, logged; 0 = all answered
int runTermsTest(const QString &titleId);

class SubtitleTranslator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *config READ config CONSTANT)
    Q_PROPERTY(QObject *localAi READ localAiObject CONSTANT)
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    Q_PROPERTY(QString sourceText READ sourceText NOTIFY textChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // The OCR files are in the package (runtime/altyazi/ocr)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY overlayChanged)
    Q_PROPERTY(bool selecting READ selecting WRITE setSelecting NOTIFY selectingChanged)
    // The PS5 title id of the game being played ("" = unknown, lines go under "genel")
    Q_PROPERTY(QString game READ game WRITE setGame NOTIFY gameChanged)
    // The last 40 lines shown in this stream, newest first: [{en, tr, at (ms)}] (Ctrl+O > Altyazı > Son satırlar)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)

public:
    SubtitleTranslator(engine::EngineController &engine, HWND video, const QString &configPath, QObject *parent = nullptr);
    ~SubtitleTranslator() override;

    QObject *config() const { return cfg; }
    QObject *localAiObject() const;
    QString text() const { return translatedText; }
    QString sourceText() const { return displayedSource; }
    QString status() const { return statusText; }
    bool available() const { return ocrAvailable; }
    bool streaming() const { return streamingFlag; }
    bool selecting() const { return selectingFlag; }
    void setSelecting(bool on);
    QString game() const { return gameId; }
    QVariantList history() const { return lineHistory; }
    void setGame(const QString &titleId);
    bool enabled() const { return cfg->value(QStringLiteral("enabled")).toBool(); }

    // The shared translation memory on the Oviora server
    void setServer(OvioraServer *server);
    // Test runs (--altyazi ...): on for this run only (not saved), region images, a fake line
    void setTestMode(bool on, bool dump, bool fakeLine);

    // Region selection (Ctrl+B): fractions of the video window, as the QML mouse area reports them
    Q_INVOKABLE void dragSelection(qreal x0, qreal y0, qreal x1, qreal y1);
    Q_INVOKABLE void selectRegion(qreal x0, qreal y0, qreal x1, qreal y1);
    // Ctrl+<key> while streaming (T, B, R, D, E; region keys while the region is shown)
    Q_INVOKABLE bool handleKey(int key, int modifiers);
    Q_INVOKABLE void resetDefaults();
    Q_INVOKABLE void clearCache();
    Q_INVOKABLE bool sharedMemoryActive() const;
    // "Düzelt" (Ctrl+E): asks to correct the line on screen; QML answers with the dialog
    Q_INVOKABLE void openCorrection();
    // "Düzelt" for an earlier line (from history)
    Q_INVOKABLE void correctLine(const QString &english, const QString &translation);
    Q_INVOKABLE void loadVariants(const QString &english);
    Q_INVOKABLE void correct(const QString &english, const QString &translation);
    Q_INVOKABLE void vote(double id, int value, const QString &english);

signals:
    void textChanged();
    void statusChanged();
    void overlayChanged();
    void selectingChanged();
    void availableChanged();
    void gameChanged();
    void historyChanged();
    void correctionRequested(const QString &english, const QString &translation);
    void variantsLoaded(const QString &english, double shown, const QVariantList &list, const QString &error);

private:
    struct PendingTranslation
    {
        quint64 seq = 0;
        QRectF box;
        int lines = 0;
        QVariantList rows;
        qint64 startedMs = 0;
        // Asked at a line's first read, before it was confirmed: only cached when it comes back,
        // shown only if the line was confirmed meanwhile
        bool early = false;
    };

    void loadConfig();
    void saveConfig();
    void requestEarly(const QString &source, const QRectF &box, int lines, const QVariantList &rows);
    void applyConfig();
    void onConfigValueChanged(const QString &key, const QVariant &value);
    void setValue(const QString &key, const QVariant &value);
    Params snapshotParams() const;
    void setStatus(const QString &status);
    void setDisplay(const QString &source, const QString &translation, const QRectF &box = QRectF(), int lines = 0,
                    const QVariantList &rows = QVariantList());
    void requestTranslation(const QString &source);
    void showTranslation(quint64 seq, const QString &source, const QString &translation, const QRectF &box, int lines,
                         const QVariantList &rows);
    void requestClear();
    void warmConnection();
    void setRegion(const QRectF &r);
    void pollStream();
    void updateTap();
    void onOcrResult(const QString &text, int frameW, int frameH, const QRectF &box, int lines, const QVariantList &rows,
                     qint64 sampleMs, double score, bool fresh);
    // Shared memory quality gate (see SubtitleTranslator.cpp offerToMemory)
    void noteCleanRead(const QString &text, double score);
    void offerToMemory(const QString &source, const QString &translation, const QString &engine);
    void onTranslated(const QString &source, const QString &translation, const QString &error);
    void onSharedFound(const QString &sha, const QString &translation);
    void loadDictionaries();
    void reloadGlossary();
    QString findInDictionary(const QString &text) const;
    void refreshOverlay(bool force = false);
    QRectF regionFromFractions(qreal x0, qreal y0, qreal x1, qreal y1) const;

    engine::EngineController &engine;
    HWND videoWindow = nullptr;
    QString configFile;
    QQmlPropertyMap *cfg = nullptr;
    QTimer *saveTimer = nullptr;
    LocalAi *localAi = nullptr;
    TranslationMemory *tm = nullptr;
    QString tmVariantsFor;
    QThread *workerThread = nullptr;
    Worker *worker = nullptr;
    Overlay *overlay = nullptr;
    QAtomicInteger<int> activeFlag = 0;   // enabled and streaming: the worker reads samples

    mutable QMutex paramsMutex;
    Params params;
    QSize frameSize;
    bool ocrAvailable = false;
    bool streamingFlag = false;
    bool selectingFlag = false;
    bool testOn = false, testFake = false;
    QString gameId;
    QRectF dragRect;   // the selection being drawn (normalized frame), invalid when none

    QString ocrText;          // last OCR result (source language)
    QString displayedSource;  // currently displayed source
    QString translatedText;   // currently displayed translation
    QVariantList lineHistory; // the last 40 lines shown, newest first
    QRectF ocrBox;
    int ocrLines = 0;
    QVariantList ocrRows;
    QRectF displayBox;
    int displayLines = 0;
    QVariantList displayRows;
    QString statusText;
    int emptyStreak = 0;
    // Reading pace: the set interval while a line is up or a new one is being confirmed; twice that
    // (at least 300 ms) after 2 s without any text, so a busy scene with no subtitle costs less
    qint64 lastTextMs = 0;
    bool relaxedTap = false;
    void paceTap(bool textNow);
    // A new read waits for a second, alike one at about the same place before it is shown
    QString candidateText;
    QRectF candidateBox;
    int candidateLines = 0;
    int candidateHits = 0;
    qint64 candidateSinceMs = 0;  // sample time of the candidate's first read
    qint64 lineSinceMs = 0;       // ... of the line being translated / shown (latency in the log)
    QHash<QString, QString> cache;
    QHash<QString, PendingTranslation> pending;
    // Shared memory quality gate: normalized English read clean twice in a row, the translations
    // waiting for that second clean read, and the last fresh OCR read
    struct SubmitWaiting
    {
        QString source, translation, engine;
        qint64 sinceMs = 0;
    };
    QSet<QString> cleanReads;
    QHash<QString, SubmitWaiting> submitWaiting;
    QHash<QString, double> bestPairScore;
    QString lastFreshNorm;
    double lastFreshScore = 0;
    quint64 nextSeq = 1;
    quint64 displayedSeq = 0;
    qint64 displayedAtMs = 0;
    qint64 sourceSeenMs = 0;  // last time OCR saw the English of the displayed translation
    QTimer *clearTimer = nullptr;
    QTimer *warmTimer = nullptr;
    QTimer *staleTimer = nullptr;
    QTimer *streamTimer = nullptr;
    QTimer *overlayTimer = nullptr;

    // Ready-made translations (e.g. a game's community translation), checked before any translator
    QHash<QString, QString> dict;            // normalized English -> Turkish
    QStringList dictKeys;
    QMultiHash<QString, int> dictByPrefix;   // first 8 normalized chars -> index into dictKeys
    QMultiHash<QString, int> dictBySuffix;   // last 8 normalized chars
    bool dictRequested = false;              // read once, the first time the translation runs
};

} // namespace oviora::subtitle
