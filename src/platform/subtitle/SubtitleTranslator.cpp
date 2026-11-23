#include "SubtitleTranslator.h"
#include "OllamaHashes.h"

#include "LocalAi.h"
#include "PaddleOcr.h"
#include "SubtitleText.h"
#include "TranslationMemory.h"
#include "TranslationOverlay.h"

#include "oviora/Log.h"
#include "oviora/engine/EngineController.h"
#include "oviora/platform/OvioraServer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSslConfiguration>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <memory>

#pragma comment(lib, "dxgi.lib")

namespace oviora::subtitle {

static void logInfo(const QString &text) { oviora::log::info("altyazi", text.toStdString()); }
static void logWarn(const QString &text) { oviora::log::warn("altyazi", text.toStdString()); }

static QString appDir() { return QCoreApplication::applicationDirPath(); }
static QString ocrDir() { return appDir() + QStringLiteral("/runtime/altyazi/ocr"); }
static QString dictionaryDir() { return appDir() + QStringLiteral("/runtime/altyazi/sozluk"); }
static QString debugDir() { return appDir() + QStringLiteral("/logs/altyazi"); }

static QVariantMap defaultConfig()
{
    return {
        { "enabled", false },
        { "engine", 0 },
        { "deeplKey", QString() },
        { "llmUrl", QStringLiteral("http://127.0.0.1:11434/api/chat") },
        { "llmModel", QStringLiteral("translategemma:4b") },
        { "llmKey", QString() },
        { "sourceLang", QStringLiteral("en") },
        { "targetLang", QStringLiteral("tr") },
        { "regionX", 0.10 },
        { "regionY", 0.78 },
        { "regionW", 0.80 },
        { "regionH", 0.17 },
        { "threshold", 180 },
        { "requireOutline", true },
        { "centeredOnly", true },
        { "debugDump", false },
        { "useDictionary", true },
        { "sharedMemory", true },
        { "intervalMs", 150 },
        { "fontRel", 1.0 },
        { "boxOpacity", 92 },
        { "coverOriginal", true },
        { "showRegion", false },
        { "showSource", false },
    };
}

// ---------------------------------------------------------------------------
// Worker: its own thread. Takes the engine's region copies, reads them and asks the translators.

class Worker : public QObject
{
    Q_OBJECT

public:
    Worker(engine::EngineController &engine, QAtomicInteger<int> *active, std::function<Params()> params)
        : engine(engine), active(active), params(std::move(params)) {}

    ~Worker() override { stopLocalServer(); }

    void start()
    {
        poller = new QTimer(this);
        poller->setInterval(40);
        connect(poller, &QTimer::timeout, this, &Worker::poll);
        poller->start();
    }

    void reset()
    {
        prevGray = QImage();
        hasLast = false;
    }

    void translate(const QString &source, const Params &p);
    void warm(const Params &p);
    void ensureLocalServer(const Params &p);
    void stopLocalServer();

signals:
    // rows: normalized frame rect of each accepted text row; sampleMs: when the read sample was taken;
    // score: the lowest OCR confidence of the kept rows (0..1); fresh: a new OCR read (false: the
    // last read said again because the picture did not change)
    void ocrResult(const QString &text, int frameW, int frameH, const QRectF &box, int lines, const QVariantList &rows,
                   qint64 sampleMs, double score, bool fresh);
    void translated(const QString &source, const QString &translation, const QString &error);
    void status(const QString &status);

private:
    struct CandidateRow
    {
        QString text;
        QRect box;
        int letters = 0;
        double score = 0;
    };

    void poll();
    void process(const engine::SubtitleSample &sample, const Params &p);
    bool ensurePaddle();
    QString runPaddle(const QImage &rgb, const Params &p, double centerX, double centerTol, QRect *box, int *lineCount,
                      QList<QRect> *rowBoxes, QString *debug, double *score);
    QString keepSubtitleRows(const QVector<CandidateRow> &candidates, QRect *box, int *lineCount, QList<QRect> *rowBoxes,
                             QString *debug, double *score);
    void logReject(const QString &text, const QString &reason);
    void dumpDebug(const QImage &rgb, const QString &log);
    void translateRace(const QString &source, const Params &p, bool google, bool local);
    QNetworkReply *postLocal(const QString &source, const Params &p);
    static QImage toRgb(const engine::SubtitleSample &s);
    static int binarize(const QImage &gray, int threshold, bool requireOutline, QImage *out, int pad);
    static QImage minFilter(const QImage &src, int radius);
    static QRectF mapBox(const QRect &boxImg, const QString &text, const QRect &crop, double scale, int frameW, int frameH);
    static QUrl googleUrl(bool single, const QString &text, const Params &p, bool gtx = false);
    static bool isOllamaNative(const QString &url) { return url.endsWith(QLatin1String("/api/chat")); }
    static QUrl localUrl(const QString &url);

    engine::EngineController &engine;
    QAtomicInteger<int> *active;
    std::function<Params()> params;
    QTimer *poller = nullptr;
    QNetworkAccessManager *nam = nullptr;
    QProcess *localServer = nullptr;
    PaddleOcr paddle;
    bool paddleFailed = false;
    double detectScale = 1.0; // the detector's input size for the current picture
    QImage prevGray;          // the middle band (or the region) at the last read
    qint64 lastReadMs = 0;
    struct LastRead { QString text; QRectF box; int lines = 0; QVariantList rows; double score = 0; } last;
    bool hasLast = false;
    QSet<QString> loggedRejects;
    qint64 lastRejectLogMs = 0;
    // Counters for the session log (one line every 10 s while running)
    struct Counters
    {
        int samples = 0, unchanged = 0, gated = 0, ocr = 0, found = 0, recognized = 0, reused = 0, skipped = 0;
        double ocrMs = 0, busyMs = 0;
    } counters;
    qint64 countersSinceMs = 0;
};

static constexpr int kOutlineDarkMax = 90;

// The region copy as 8-bit RGB. SDR frames are read as they are; HDR frames (PQ or scRGB) are
// brought down to SDR with 203 nits as white, where game subtitles sit
QImage Worker::toRgb(const engine::SubtitleSample &s)
{
    QImage out(int(s.width), int(s.height), QImage::Format_RGB888);
    if (out.isNull() || s.pixels.size() < size_t(s.width) * s.height * s.bytesPerPixel)
        return QImage();
    static const std::array<uint8_t, 4096> srgb = [] {
        std::array<uint8_t, 4096> t{};
        for (int i = 0; i < 4096; i++) {
            const double x = i / 4095.0;
            const double v = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
            t[size_t(i)] = uint8_t(std::clamp(std::lround(v * 255), 0L, 255L));
        }
        return t;
    }();
    auto encode = [](double linear) { return srgb[size_t(std::clamp(linear, 0.0, 1.0) * 4095 + 0.5)]; };
    static const std::array<uint8_t, 1024> pq = [] {
        std::array<uint8_t, 1024> t{};
        const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        for (int i = 0; i < 1024; i++) {
            const double p = std::pow(i / 1023.0, 1 / m2);
            const double nits = 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1 / m1);
            const double x = std::clamp(nits / 203.0, 0.0, 1.0);
            const double v = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
            t[size_t(i)] = uint8_t(std::clamp(std::lround(v * 255), 0L, 255L));
        }
        return t;
    }();
    for (uint32_t y = 0; y < s.height; y++) {
        const uint8_t *in = s.pixels.data() + size_t(y) * s.width * s.bytesPerPixel;
        uchar *o = out.scanLine(int(y));
        for (uint32_t x = 0; x < s.width; x++) {
            uchar r, g, b;
            switch (s.format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                b = in[4 * x]; g = in[4 * x + 1]; r = in[4 * x + 2];
                break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: {
                uint32_t v;
                memcpy(&v, in + 4 * x, 4);
                r = pq[v & 0x3ff]; g = pq[(v >> 10) & 0x3ff]; b = pq[(v >> 20) & 0x3ff];
                break;
            }
            case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                uint16_t h[3];
                memcpy(h, in + 8 * x, 6);
                // scRGB: 1.0 = 80 nits
                const double k = 80.0 / 203.0;
                r = encode(DirectX::PackedVector::XMConvertHalfToFloat(h[0]) * k);
                g = encode(DirectX::PackedVector::XMConvertHalfToFloat(h[1]) * k);
                b = encode(DirectX::PackedVector::XMConvertHalfToFloat(h[2]) * k);
                break;
            }
            default:
                r = in[4 * x]; g = in[4 * x + 1]; b = in[4 * x + 2];
                break;
            }
            o[3 * x] = r;
            o[3 * x + 1] = g;
            o[3 * x + 2] = b;
        }
    }
    return out;
}

void Worker::poll()
{
    if (!active->loadRelaxed())
        return;
    engine::SubtitleSample sample;
    if (!engine.takeSubtitleSample(sample))
        return;
    // Test runs: OVIORA_TEST_SUBTITLE_IMAGE=<png> stands in for the region of every copy, so the
    // whole chain (OCR, filters, translation, overlay) runs on a known subtitle during a stream
    static const QString testImagePath = qEnvironmentVariable("OVIORA_TEST_SUBTITLE_IMAGE");
    if (!testImagePath.isEmpty()) {
        static const QImage testImage = QImage(testImagePath).convertToFormat(QImage::Format_RGBA8888);
        if (!testImage.isNull()) {
            const QImage scaled = testImage.scaled(int(sample.width), int(sample.height), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            sample.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sample.bytesPerPixel = 4;
            sample.pixels.resize(size_t(sample.width) * sample.height * 4);
            for (uint32_t y = 0; y < sample.height; y++)
                memcpy(sample.pixels.data() + size_t(y) * sample.width * 4, scaled.constScanLine(int(y)), size_t(sample.width) * 4);
            static bool told = false;
            if (!told) {
                told = true;
                logInfo(QStringLiteral("test image stands in for the region: ") + testImagePath);
            }
        }
    }
    QElapsedTimer busy;
    busy.start();
    process(sample, params());
    counters.busyMs += busy.nsecsElapsed() / 1e6;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!countersSinceMs)
        countersSinceMs = now;
    if (now - countersSinceMs >= 10000) {
        const double seconds = (now - countersSinceMs) / 1000.0;
        logInfo(QStringLiteral("readings in %1 s: samples=%2 unchanged=%3 no-outlined-text=%4 ocr=%5 (avg %6 ms) text-found=%7 "
                               "rows read=%8 reused=%9 off-centre-skipped=%10 busy=%11 ms/s")
                    .arg(int(seconds)).arg(counters.samples).arg(counters.unchanged).arg(counters.gated)
                    .arg(counters.ocr).arg(counters.ocr ? int(counters.ocrMs / counters.ocr) : 0).arg(counters.found)
                    .arg(counters.recognized).arg(counters.reused).arg(counters.skipped).arg(counters.busyMs / seconds, 0, 'f', 1));
        counters = {};
        countersSinceMs = now;
    }
}

void Worker::process(const engine::SubtitleSample &sample, const Params &p)
{
    counters.samples++;
    if (!ensurePaddle())
        return;
    const int frameW = int(sample.frameWidth), frameH = int(sample.frameHeight);
    const QRect crop(int(sample.x), int(sample.y), int(sample.width), int(sample.height));
    QImage rgb = toRgb(sample);
    if (rgb.isNull())
        return;
    // OCR works on 1080p-sized text: a 4K (upscaled) frame is read at half size
    const double scale = frameH > 1200 ? 1080.0 / frameH : 1.0;
    if (scale < 1.0)
        rgb = rgb.scaled(qMax(1, int(std::lround(rgb.width() * scale))), qMax(1, int(std::lround(rgb.height() * scale))),
                         Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QImage gray = rgb.convertToFormat(QImage::Format_Grayscale8);
    detectScale = frameH * scale >= 1000 ? 0.6 : 1.0;

    // Image x of the region's horizontal centre, for the "subtitles are centred" filter
    const double regionTol = qMin(frameW * 0.08, qMax(crop.width() * 0.15, frameW * 0.03));
    const double centerX = crop.width() * scale / 2.0;
    const double centerTol = regionTol * scale;
    // The cheap checks look at the middle band only when subtitles are centred: every centred
    // line has letters there, while HUD parts at the sides (GTA's minimap turns all the time)
    // would otherwise count as a change and as outlined text on every sample
    QRect band = gray.rect();
    if (p.centeredOnly) {
        const int half = int(std::ceil(centerTol + frameW * 0.06 * scale));
        band = QRect(int(centerX) - half, 0, 2 * half, gray.height()).intersected(gray.rect());
        if (band.width() < 8)
            band = gray.rect();
    }
    const QImage bandGray = band == gray.rect() ? gray : gray.copy(band);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Skip the work when the band looks the same as at the last read...
    if (!prevGray.isNull() && prevGray.size() == bandGray.size()) {
        qint64 diff = 0;
        for (int y = 0; y < bandGray.height(); y += 2) {
            const uchar *a = bandGray.constScanLine(y);
            const uchar *b = prevGray.constScanLine(y);
            for (int x = 0; x < bandGray.width(); x += 2)
                diff += qAbs(int(a[x]) - int(b[x])) > 40;
        }
        // ...and say what that read said, so a line that holds still is confirmed on the next
        // sample (it used to wait for the once-a-second re-read). The region is read again at
        // least every 2 s: a subtitle fading out changes only a little per sample. While the last
        // read had text, every 450 ms: a small line leaving changes too few pixels to count, and the
        // translation stayed up to 2 s after its English was gone
        const qint64 maxReuseMs = hasLast && !last.text.isEmpty() ? 450 : 2000;
        if (diff < qint64(bandGray.width()) * bandGray.height() / 4 / 400 && now - lastReadMs < maxReuseMs) {
            counters.unchanged++;
            if (hasLast)
                emit ocrResult(last.text, frameW, frameH, last.box, last.lines, last.rows, now, last.score, false);
            return;
        }
    }
    prevGray = bandGray;
    lastReadMs = now;
    const qint64 area = qint64(bandGray.width()) * bandGray.height();
    QString debug;
    auto remember = [&](const QString &text, const QRectF &box, int lines, const QVariantList &rows, double score) {
        last = { text, box, lines, rows, score };
        hasLast = true;
        emit ocrResult(text, frameW, frameH, box, lines, rows, now, score, true);
    };

    // Cheap gate before the neural OCR: with no outlined text in the band there is nothing to
    // read, so the CPU-heavy detector only runs while a subtitle is showing
    if (p.requireOutline) {
        QImage bin(bandGray.width(), bandGray.height(), QImage::Format_Grayscale8);
        bin.fill(255);
        const int pixels = binarize(bandGray, p.threshold, true, &bin, 0);
        debug += QStringLiteral("[kapi] kenarlikli piksel=%1 esik=%2\n").arg(pixels).arg(area / 2000);
        if (pixels < area / 2000) {
            counters.gated++;
            if (p.debugDump)
                dumpDebug(rgb, debug + QStringLiteral("SONUC: (kenarlikli yazi yok)"));
            remember(QString(), QRectF(), 0, QVariantList(), 0);
            return;
        }
    }
    QRect boxImg;
    int lines = 0;
    QList<QRect> rowsImg;
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    double score = 0;
    const QString text = runPaddle(rgb, p, centerX, centerTol, &boxImg, &lines, &rowsImg, &debug, &score);
    counters.ocr++;
    counters.ocrMs += double(QDateTime::currentMSecsSinceEpoch() - t0);
    if (!text.isEmpty())
        counters.found++;
    if (p.debugDump)
        dumpDebug(rgb, debug + QStringLiteral("SONUC: ") + text);
    QVariantList rows;
    for (const QRect &r : rowsImg)
        rows << mapBox(r, text, crop, scale, frameW, frameH);
    remember(text, mapBox(boxImg, text, crop, scale, frameW, frameH), lines, rows, text.isEmpty() ? 0 : score);
}

// Text box from OCR image pixels (scaled crop) to normalized frame coordinates
QRectF Worker::mapBox(const QRect &boxImg, const QString &text, const QRect &crop, double scale, int frameW, int frameH)
{
    if (text.isEmpty() || !boxImg.isValid() || frameW <= 0 || frameH <= 0)
        return QRectF();
    const QRectF box(crop.x() + boxImg.x() / scale, crop.y() + boxImg.y() / scale, boxImg.width() / scale, boxImg.height() / scale);
    return QRectF(box.x() / frameW, box.y() / frameH, box.width() / frameW, box.height() / frameH);
}

bool Worker::ensurePaddle()
{
    if (paddle.loaded())
        return true;
    if (paddleFailed)
        return false;
    QString error;
    if (!paddle.load(ocrDir(), &error)) {
        paddleFailed = true;
        logWarn(QStringLiteral("PaddleOCR could not be loaded: ") + error);
        emit status(tr("PaddleOCR açılamadı: %1").arg(error));
        return false;
    }
    logInfo(QStringLiteral("PaddleOCR ready (%1)").arg(ocrDir()));
    emit status(tr("PaddleOCR hazır"));
    return true;
}

// Neural OCR: no binarization needed, the detector finds text over busy backgrounds by itself
QString Worker::runPaddle(const QImage &rgb, const Params &p, double centerX, double centerTol, QRect *box, int *lineCount,
                          QList<QRect> *rowBoxes, QString *debug, double *score)
{
    // Detected boxes grouped into rows first: a row off the centre (HUD, minimap, phone) is dropped
    // below anyway, so it isn't read at all
    // The detector only has to find the text, not read it: 1080p subtitles (~26 px letters) are
    // found as well at 0.6 of the size (same reads on the test lines, ~40 % less detector time).
    // Smaller pictures (720p) keep full size. OVIORA_OCR_DET_SCALE overrides it (tests).
    static const double detScaleOverride = qEnvironmentVariable("OVIORA_OCR_DET_SCALE").toDouble();
    const double detScale = detScaleOverride >= 0.3 && detScaleOverride <= 1.0 ? detScaleOverride : detectScale;
    const QVector<QRect> detected = paddle.detect(rgb, detScale);
    QVector<QRect> rowBoxesDetected;
    QVector<int> rowOf(detected.size(), -1);
    for (int i = 0; i < detected.size(); i++) {
        const QRect &b = detected.at(i);
        for (int r = 0; r < rowBoxesDetected.size(); r++) {
            const int tol = qMin(rowBoxesDetected.at(r).height(), b.height()) / 2;
            if (qAbs(rowBoxesDetected.at(r).center().y() - b.center().y()) <= tol) {
                rowBoxesDetected[r] |= b;
                rowOf[i] = r;
                break;
            }
        }
        if (rowOf.at(i) < 0) {
            rowOf[i] = int(rowBoxesDetected.size());
            rowBoxesDetected << b;
        }
    }
    QVector<PaddleOcr::Line> raw;
    for (int i = 0; i < detected.size(); i++) {
        const QRect &row = rowBoxesDetected.at(rowOf.at(i));
        if (p.centeredOnly && qAbs(row.center().x() - centerX) > centerTol) {
            counters.skipped++;
            debug->append(QStringLiteral("  ortada olmayan satir okunmadi: %1,%2 %3x%4\n").arg(row.x()).arg(row.y()).arg(row.width()).arg(row.height()));
            continue;
        }
        bool reused = false;
        PaddleOcr::Line l = paddle.recognize(rgb, detected.at(i), &reused);
        (reused ? counters.reused : counters.recognized)++;
        if (!l.text.isEmpty())
            raw << l;
    }
    std::sort(raw.begin(), raw.end(), [](const PaddleOcr::Line &a, const PaddleOcr::Line &b) {
        return a.box.center().y() < b.box.center().y();
    });

    // The detector can split one subtitle line into pieces at wide gaps; merge pieces per row
    struct Row
    {
        QVector<PaddleOcr::Line> parts;
        QRect box;
    };
    QVector<Row> rows;
    for (const PaddleOcr::Line &l : raw) {
        bool merged = false;
        for (Row &row : rows) {
            const int tol = qMin(row.box.height(), l.box.height()) / 2;
            if (qAbs(row.box.center().y() - l.box.center().y()) <= tol) {
                row.parts << l;
                row.box |= l.box;
                merged = true;
                break;
            }
        }
        if (!merged)
            rows << Row { { l }, l.box };
    }

    QVector<CandidateRow> candidates;
    for (Row &row : rows) {
        std::sort(row.parts.begin(), row.parts.end(), [](const PaddleOcr::Line &a, const PaddleOcr::Line &b) {
            return a.box.x() < b.box.x();
        });
        QStringList words;
        double scoreSum = 0;
        int weight = 0;
        int letters = 0;
        for (const PaddleOcr::Line &part : row.parts) {
            words << part.text;
            scoreSum += part.score * part.text.size();
            weight += part.text.size();
            for (const QChar c : part.text) {
                if (c.isLetter())
                    letters++;
            }
        }
        const QString lineText = words.join(QLatin1Char(' '));
        const double score = weight ? scoreSum / weight : 0;
        const bool confident = score >= 0.80 && letters >= 3;
        // Subtitles are centered on screen; menus, phone screens and HUD lists are not
        const bool centered = !p.centeredOnly || qAbs(row.box.center().x() - centerX) <= centerTol;
        // Plates, codes on signs, HUD numbers and capital HUD words
        const QString noise = confident && centered ? rowNoise(lineText) : QString();
        debug->append(QStringLiteral("  satir guven=%1 ortali=%2 kabul=%3%5: %4\n")
                          .arg(int(score * 100)).arg(centered ? 1 : 0).arg(confident && centered && noise.isEmpty() ? 1 : 0)
                          .arg(lineText, noise.isEmpty() ? QString() : QStringLiteral(" (") + noise + QLatin1Char(')')));
        if (confident && !centered)
            logReject(lineText, QStringLiteral("ortada degil"));
        else if (!noise.isEmpty())
            logReject(lineText, noise);
        if (confident && centered && noise.isEmpty()) {
            // Detector boxes carry the unclip margin (~1.4x the glyph height); trim it so the
            // overlay sizes its font to the glyphs
            const int trim = row.box.height() * 15 / 100;
            candidates << CandidateRow { lineText, row.box.adjusted(0, trim, 0, -trim), letters, score };
        }
    }
    return keepSubtitleRows(candidates, box, lineCount, rowBoxes, debug, score);
}

// The rows that passed one by one: drop the ones whose text height does not match the dialogue
// line, then the whole read must look like a subtitle
QString Worker::keepSubtitleRows(const QVector<CandidateRow> &candidates, QRect *box, int *lineCount, QList<QRect> *rowBoxes,
                                 QString *debug, double *score)
{
    QVector<int> heights, letters;
    for (const CandidateRow &r : candidates) {
        heights << r.box.height();
        letters << r.letters;
    }
    const QVector<bool> keep = heightKeep(heights, letters);
    QStringList kept;
    QRect textBox;
    QList<QRect> keptBoxes;
    double lowest = 1.0;
    for (int i = 0; i < candidates.size(); i++) {
        if (!keep.at(i)) {
            debug->append(QStringLiteral("  yazi boyu farkli, atildi: %1\n").arg(candidates.at(i).text));
            logReject(candidates.at(i).text, QStringLiteral("yazi boyu farkli"));
            continue;
        }
        kept << candidates.at(i).text;
        lowest = qMin(lowest, candidates.at(i).score);
        textBox |= candidates.at(i).box;
        keptBoxes << candidates.at(i).box;
    }
    const QString text = cleanOcrText(kept.join(QLatin1Char(' ')));
    if (text.isEmpty())
        return QString();
    const QString reason = textReason(text);
    if (!reason.isEmpty()) {
        debug->append(QStringLiteral("  altyaziya benzemiyor (%1): %2\n").arg(reason, text));
        logReject(text, reason);
        return QString();
    }
    *box = textBox;
    *lineCount = kept.size();
    if (score)
        *score = lowest;
    if (rowBoxes)
        *rowBoxes = keptBoxes;
    return text;
}

// Rejected reads go to the session log so the filter can be tuned, but sparingly: each text
// once, at most one line a second
void Worker::logReject(const QString &text, const QString &reason)
{
    const QString key = normalizeForCompare(text).left(60);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (key.isEmpty() || loggedRejects.contains(key) || now - lastRejectLogMs < 1000)
        return;
    if (loggedRejects.size() > 300)
        loggedRejects.clear();
    loggedRejects.insert(key);
    lastRejectLogMs = now;
    logInfo(QStringLiteral("candidate rejected (%1): \"%2\"").arg(reason, text.left(80)));
}

void Worker::dumpDebug(const QImage &rgb, const QString &log)
{
    const QString dir = debugDir();
    QDir().mkpath(dir);
    rgb.save(dir + QStringLiteral("/1_bolge.png"));
    QFile f(dir + QStringLiteral("/ocr_log.txt"));
    if (f.size() > 2 * 1024 * 1024)
        f.remove();
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write(QStringLiteral("=== %1\n%2\n\n").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), log).toUtf8());
}

// Separable min filter (erosion) with a square window of 2 * radius + 1
QImage Worker::minFilter(const QImage &src, int radius)
{
    const int w = src.width(), h = src.height();
    QImage tmp(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; y++) {
        const uchar *in = src.constScanLine(y);
        uchar *out = tmp.scanLine(y);
        for (int x = 0; x < w; x++) {
            uchar m = 255;
            for (int k = qMax(0, x - radius); k <= qMin(w - 1, x + radius); k++)
                m = qMin(m, in[k]);
            out[x] = m;
        }
    }
    QImage dst(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; y++) {
        uchar *out = dst.scanLine(y);
        for (int x = 0; x < w; x++)
            out[x] = 255;
        for (int k = qMax(0, y - radius); k <= qMin(h - 1, y + radius); k++) {
            const uchar *in = tmp.constScanLine(k);
            for (int x = 0; x < w; x++)
                out[x] = qMin(out[x], in[x]);
        }
    }
    return dst;
}

// Writes bright text as black into out (offset by pad), returns the number of text pixels. Most
// game subtitles have a dark outline or shadow, so a letter is a bright island fully enclosed by
// dark: with requireOutline each bright 4-connected component is kept only if nearly all of its
// pixels have dark within a few pixels
int Worker::binarize(const QImage &gray, int threshold, bool requireOutline, QImage *out, int pad)
{
    const int w = gray.width(), h = gray.height();
    const int thr = threshold;
    int count = 0;
    if (!requireOutline) {
        for (int y = 0; y < h; y++) {
            const uchar *in = gray.constScanLine(y);
            uchar *o = out->scanLine(y + pad) + pad;
            for (int x = 0; x < w; x++) {
                if (in[x] >= thr) {
                    o[x] = 0;
                    count++;
                }
            }
        }
        return count;
    }
    const QImage localMin = minFilter(gray, 4);
    QVector<uchar> visited(w * h, 0);
    QVector<int> stack;
    QVector<int> members;
    for (int y0 = 0; y0 < h; y0++) {
        for (int x0 = 0; x0 < w; x0++) {
            const int s = y0 * w + x0;
            if (visited[s] || gray.constScanLine(y0)[x0] < thr)
                continue;
            stack.clear();
            members.clear();
            stack.append(s);
            visited[s] = 1;
            int nearDark = 0;
            while (!stack.isEmpty()) {
                const int c = stack.takeLast();
                members.append(c);
                const int cx = c % w, cy = c / w;
                if (localMin.constScanLine(cy)[cx] <= kOutlineDarkMax)
                    nearDark++;
                auto push = [&](int nx, int ny) {
                    const int q = ny * w + nx;
                    if (!visited[q] && gray.constScanLine(ny)[nx] >= thr) {
                        visited[q] = 1;
                        stack.append(q);
                    }
                };
                if (cx > 0) push(cx - 1, cy);
                if (cx < w - 1) push(cx + 1, cy);
                if (cy > 0) push(cx, cy - 1);
                if (cy < h - 1) push(cx, cy + 1);
            }
            if (nearDark * 10 < members.size() * 9)
                continue;
            for (const int c : members)
                out->scanLine(c / w + pad)[c % w + pad] = 0;
            count += members.size();
        }
    }
    return count;
}

QUrl Worker::googleUrl(bool single, const QString &text, const Params &p, bool gtx)
{
    QUrl url(single ? QStringLiteral("https://translate.googleapis.com/translate_a/single")
                    : QStringLiteral("https://clients5.google.com/translate_a/t"));
    QUrlQuery q;
    q.addQueryItem("client", gtx ? "gtx" : "dict-chrome-ex");
    q.addQueryItem("sl", p.sourceLang.isEmpty() ? QStringLiteral("auto") : p.sourceLang);
    q.addQueryItem("tl", p.targetLang);
    if (single)
        q.addQueryItem("dt", "t");
    q.addQueryItem("q", text);
    url.setQuery(q);
    return url;
}

// "localhost" first tries IPv6 ::1, which Ollama doesn't listen on; measured +2 s per request
QUrl Worker::localUrl(const QString &url)
{
    QUrl u(url);
    if (u.host() == QLatin1String("localhost"))
        u.setHost(QStringLiteral("127.0.0.1"));
    return u;
}

void Worker::translate(const QString &source, const Params &p)
{
    if (!nam)
        nam = new QNetworkAccessManager(this);
    QNetworkReply *reply = nullptr;
    // The game's names and terms go out as tokens and come back as the fixed words
    const ProtectedText prot = p.glossary ? protectTerms(source, *p.glossary, p.targetLang == QLatin1String("tr")) : ProtectedText { source };
    const QString &sent = prot.text;
    switch (p.engine) {
    case 1: { // DeepL
        if (p.deeplKey.isEmpty()) {
            emit translated(source, QString(), tr("DeepL API anahtarı girilmemiş"));
            return;
        }
        const bool freeKey = p.deeplKey.trimmed().endsWith(QLatin1String(":fx"));
        QNetworkRequest req(QUrl(freeKey ? QStringLiteral("https://api-free.deepl.com/v2/translate")
                                         : QStringLiteral("https://api.deepl.com/v2/translate")));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setRawHeader("Authorization", "DeepL-Auth-Key " + p.deeplKey.trimmed().toUtf8());
        req.setTransferTimeout(8000);
        QJsonObject body { { "text", QJsonArray { sent } }, { "target_lang", p.targetLang.toUpper() } };
        if (!p.sourceLang.isEmpty())
            body.insert("source_lang", p.sourceLang.toUpper());
        reply = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        break;
    }
    case 2: // local model only
        translateRace(source, p, false, true);
        return;
    case 4: // local model and Google, first answer wins
        translateRace(source, p, true, true);
        return;
    case 3: { // MyMemory (no key, the fallback when Google failed)
        QUrl url(QStringLiteral("https://api.mymemory.translated.net/get"));
        QUrlQuery q;
        q.addQueryItem("q", sent);
        q.addQueryItem("langpair", (p.sourceLang.isEmpty() ? QStringLiteral("en") : p.sourceLang) + QLatin1Char('|') + p.targetLang);
        url.setQuery(q);
        QNetworkRequest req(url);
        req.setTransferTimeout(8000);
        reply = nam->get(req);
        break;
    }
    default: // Google (no key)
        translateRace(source, p, true, false);
        return;
    }
    const int engineId = p.engine;
    connect(reply, &QNetworkReply::finished, this, [this, reply, source, engineId, prot]() {
        reply->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError) {
            emit translated(source, QString(), tr("Çeviri hatası: %1").arg(reply->errorString()));
            return;
        }
        QString result;
        if (engineId == 3) {
            result = doc.object().value("responseData").toObject().value("translatedText").toString();
        } else if (engineId == 1) {
            const QJsonArray arr = doc.object().value("translations").toArray();
            if (!arr.isEmpty())
                result = arr.at(0).toObject().value("text").toString();
        }
        result = restoreTerms(result.trimmed(), prot);
        if (result.isEmpty())
            emit translated(source, QString(), tr("Çeviri cevabı boş geldi"));
        else
            emit translated(source, result, QString());
    });
}

QNetworkReply *Worker::postLocal(const QString &source, const Params &p)
{
    QNetworkRequest req { localUrl(p.llmUrl) };
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!p.llmKey.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + p.llmKey.toUtf8());
    req.setTransferTimeout(20000);
    const QString lang = p.targetLang == QLatin1String("tr") ? QStringLiteral("Turkish") : p.targetLang;
    QJsonArray messages;
    if (p.llmModel.contains(QLatin1String("translategemma"))) {
        // TranslateGemma's template takes one user turn in exactly this shape (from the model card),
        // including the two blank lines before the text
        messages << QJsonObject { { "role", "user" }, { "content", QStringLiteral(
            "You are a professional English (en) to %1 (%2) translator. Your goal is to accurately convey "
            "the meaning and nuances of the original English text while adhering to %1 grammar, vocabulary, "
            "and cultural sensitivities.\nProduce only the %1 translation, without any additional "
            "explanations or commentary. Please translate the following English text into %1:\n\n\n%3")
            .arg(lang, p.targetLang, source) } };
    } else {
        messages << QJsonObject { { "role", "system" }, { "content", QStringLiteral(
            "You translate video game subtitles into natural, fluent %1. Keep character names unchanged. "
            "Reply with only the translation, nothing else.%2").arg(lang,
            p.glossary ? glossaryHint(source, *p.glossary, p.targetLang == QLatin1String("tr")) : QString()) } };
        messages << QJsonObject { { "role", "user" }, { "content", source } };
    }
    QJsonObject body { { "model", p.llmModel }, { "stream", false }, { "messages", messages } };
    if (isOllamaNative(p.llmUrl)) {
        // Keeps the model in VRAM between lines; a cold load takes seconds
        body.insert("keep_alive", "30m");
        body.insert("options", QJsonObject { { "temperature", 0 }, { "num_predict", 160 } });
    } else {
        body.insert("temperature", 0);
    }
    return nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

// Asks several translators at once and shows the first usable answer. Google has two public
// endpoints; the local model has no network wait at all. MyMemory only when every racer failed.
void Worker::translateRace(const QString &source, const Params &p, bool google, bool local)
{
    struct Race
    {
        bool done = false;
        int left = 0;
        QString lastError;
    };
    auto race = std::make_shared<Race>();
    // Google three ways (a new sentence takes it 0.5-1.5 s, differently each time; the first answer wins)
    race->left = (google ? 3 : 0) + (local ? 1 : 0);
    auto finish = [this, race, source, p, google](const QString &result, const QString &error) {
        if (race->done)
            return;
        if (!result.trimmed().isEmpty()) {
            race->done = true;
            emit translated(source, result.trimmed(), QString());
            return;
        }
        if (!error.isEmpty())
            race->lastError = error;
        if (--race->left > 0)
            return;
        race->done = true;
        if (google) {
            // Every racer failed (e.g. Google's captcha page): fall back to MyMemory
            Params fallback = p;
            fallback.engine = 3;
            translate(source, fallback);
        } else {
            emit translated(source, QString(), tr("Yerel model cevap vermedi: %1").arg(race->lastError));
        }
    };
    if (google) {
        const ProtectedText prot = p.glossary ? protectTerms(source, *p.glossary, p.targetLang == QLatin1String("tr")) : ProtectedText { source };
        for (const int way : { 0, 1, 2 }) {
            const bool single = way != 1;
            QNetworkRequest req(googleUrl(single, prot.text, p, way == 2));
            req.setTransferTimeout(8000);
            QNetworkReply *reply = nam->get(req);
            QElapsedTimer took;
            took.start();
            connect(reply, &QNetworkReply::finished, this, [reply, single, way, finish, took, prot]() {
                reply->deleteLater();
                logInfo(QStringLiteral("google %1 answered in %2 ms (http2=%3, error=%4)").arg(way == 2 ? QStringLiteral("gtx") : single ? QStringLiteral("single") : QStringLiteral("t"))
                            .arg(took.elapsed()).arg(reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool())
                            .arg(int(reply->error())));
                QString result;
                if (reply->error() == QNetworkReply::NoError) {
                    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                    if (single) {
                        for (const QJsonValue &seg : doc.array().at(0).toArray())
                            result += seg.toArray().at(0).toString();
                    } else {
                        // ["text"] or [["text", "detected-lang"]]
                        const QJsonValue first = doc.array().at(0);
                        result = first.isString() ? first.toString() : first.toArray().at(0).toString();
                    }
                }
                finish(restoreTerms(result, prot), reply->errorString());
            });
        }
    }
    if (local) {
        QNetworkReply *reply = postLocal(source, p);
        const bool native = isOllamaNative(p.llmUrl);
        connect(reply, &QNetworkReply::finished, this, [reply, native, finish]() {
            reply->deleteLater();
            QString result;
            if (reply->error() == QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                const QJsonObject message = native
                    ? doc.object().value("message").toObject()
                    : doc.object().value("choices").toArray().at(0).toObject().value("message").toObject();
                result = message.value("content").toString();
                static const QRegularExpression think(QStringLiteral("<think>.*</think>"), QRegularExpression::DotMatchesEverythingOption);
                result.remove(think);
            }
            finish(result, reply->errorString());
        });
    }
}

// The Ollama downloaded next to Oviora (Altyazı → Yerel çeviri yapay zekası) is started when no
// Ollama answers locally, with its model folder; it is stopped again when Oviora quits.
void Worker::ensureLocalServer(const Params &p)
{
    if (localServer || (p.engine != 2 && p.engine != 4))
        return;
    const QUrl url = localUrl(p.llmUrl);
    if (url.host() != QLatin1String("127.0.0.1"))
        return;
    const QString dir = appDir() + QStringLiteral("/ollama");
    const QString exe = dir + QStringLiteral("/ollama.exe");
    // Only the folder Oviora installed and verified (OllamaHashes.h)
    if (!QFileInfo::exists(exe) || !ollamaRuntimeVerified(dir))
        return;
    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, quint16(url.port(11434)));
    if (probe.waitForConnected(400))
        return;  // an installed Ollama (or an earlier start) is already serving
    localServer = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("OLLAMA_MODELS"), QDir::toNativeSeparators(dir + QStringLiteral("/models")));
    env.insert(QStringLiteral("OLLAMA_HOST"), QStringLiteral("127.0.0.1:%1").arg(url.port(11434)));
    // Downloaded for a GPU without CUDA/ROCm: only the Vulkan backend is there
    env.insert(QStringLiteral("OLLAMA_VULKAN"), QStringLiteral("1"));
    localServer->setProcessEnvironment(env);
    localServer->setWorkingDirectory(dir);
    localServer->start(exe, { QStringLiteral("serve") });
    const bool started = localServer->waitForStarted(5000);
    // Tie the server to this process: when Oviora ends in any way, Windows closes the job and the
    // server with it (a killed test run left it holding ~3 GB of VRAM)
    if (started) {
        static HANDLE job = nullptr;
        if (!job) {
            job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job)
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
        if (job) {
            HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(localServer->processId()));
            if (proc) {
                AssignProcessToJobObject(job, proc);
                CloseHandle(proc);
            }
        }
    }
    logInfo(QStringLiteral("local translation server started=%1").arg(started));
    emit status(started ? tr("Yerel çeviri sunucusu başlatıldı") : tr("Yerel çeviri sunucusu başlatılamadı"));
}

void Worker::stopLocalServer()
{
    if (!localServer)
        return;
    localServer->kill();
    localServer->waitForFinished(3000);
    delete localServer;
    localServer = nullptr;
}

// A cold HTTPS request to the translator measured ~1.4 s against ~0.25 s on a warm connection.
// The warm connection must offer HTTP/2 like the requests do: Qt keeps HTTP/1.1 and HTTP/2
// connections apart, and a bare connectToHostEncrypted() (no h2 in ALPN) made a connection the
// translation requests never used, so every first request still paid the TLS handshake.
static void preconnect(QNetworkAccessManager *nam, const QString &host)
{
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setAllowedNextProtocols({ QSslConfiguration::ALPNProtocolHTTP2, QSslConfiguration::NextProtocolHttp1_1 });
    nam->connectToHostEncrypted(host, 443, ssl);
}

void Worker::warm(const Params &p)
{
    if (!nam)
        nam = new QNetworkAccessManager(this);
    switch (p.engine) {
    case 1:
        preconnect(nam, p.deeplKey.trimmed().endsWith(QLatin1String(":fx")) ? QStringLiteral("api-free.deepl.com")
                                                                              : QStringLiteral("api.deepl.com"));
        break;
    case 2:
    case 4:
        if (isOllamaNative(p.llmUrl)) {
            // An empty generate request only loads the model into VRAM and keeps it there
            QUrl url = localUrl(p.llmUrl);
            url.setPath(QStringLiteral("/api/generate"));
            QNetworkRequest req(url);
            req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            req.setTransferTimeout(60000);
            QNetworkReply *reply = nam->post(req, QJsonDocument(QJsonObject { { "model", p.llmModel }, { "keep_alive", "30m" } })
                                                      .toJson(QJsonDocument::Compact));
            connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        }
        if (p.engine == 2)
            break;
        Q_FALLTHROUGH();
    default:
        // Bare connections to both race hosts: no text is sent until a subtitle is read
        preconnect(nam, QStringLiteral("translate.googleapis.com"));
        preconnect(nam, QStringLiteral("clients5.google.com"));
        break;
    }
}

// ---------------------------------------------------------------------------

static QString gpuVendor()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return QStringLiteral("other");
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        if (desc.VendorId == 0x10DE)
            return QStringLiteral("nvidia");
        if (desc.VendorId == 0x1002)
            return QStringLiteral("amd");
    }
    return QStringLiteral("other");
}

SubtitleTranslator::SubtitleTranslator(engine::EngineController &engine, HWND video, const QString &configPath, QObject *parent)
    : QObject(parent), engine(engine), videoWindow(video), configFile(configPath), cfg(new QQmlPropertyMap(this))
{
    // Before the settings are read: applying them drives the overlay timer
    overlayTimer = new QTimer(this);
    overlayTimer->setInterval(150);
    connect(overlayTimer, &QTimer::timeout, this, [this] { refreshOverlay(); });
    loadConfig();
    connect(cfg, &QQmlPropertyMap::valueChanged, this, &SubtitleTranslator::onConfigValueChanged);
    saveTimer = new QTimer(this);
    saveTimer->setSingleShot(true);
    saveTimer->setInterval(400);
    connect(saveTimer, &QTimer::timeout, this, &SubtitleTranslator::saveConfig);

    const QString dir = ocrDir();
    ocrAvailable = QFileInfo::exists(dir + QStringLiteral("/onnxruntime.dll")) && QFileInfo::exists(dir + QStringLiteral("/det.onnx"))
        && QFileInfo::exists(dir + QStringLiteral("/rec.onnx")) && QFileInfo::exists(dir + QStringLiteral("/dict.txt"));
    logInfo(QStringLiteral("ready: ocr files %1 (%2), enabled=%3").arg(ocrAvailable ? QStringLiteral("found") : QStringLiteral("missing"), dir)
                .arg(enabled()));

    workerThread = new QThread(this);
    workerThread->setObjectName(QStringLiteral("subtitles"));
    worker = new Worker(engine, &activeFlag, [this] { return snapshotParams(); });
    worker->moveToThread(workerThread);
    connect(workerThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &Worker::ocrResult, this, &SubtitleTranslator::onOcrResult);
    connect(worker, &Worker::translated, this, &SubtitleTranslator::onTranslated);
    connect(worker, &Worker::status, this, &SubtitleTranslator::setStatus);
    workerThread->start(QThread::LowPriority);
    QMetaObject::invokeMethod(worker, [w = worker] { w->start(); }, Qt::QueuedConnection);

    LocalAi::Hooks hooks;
    hooks.gpu_vendor = [] { return gpuVendor(); };
    hooks.stop_server = [this] {
        QMetaObject::invokeMethod(worker, [w = worker] { w->stopLocalServer(); }, Qt::BlockingQueuedConnection);
    };
    hooks.installed = [this] { setValue(QStringLiteral("engine"), 4); };
    localAi = new LocalAi(std::move(hooks), this);

    clearTimer = new QTimer(this);
    clearTimer->setSingleShot(true);
    warmTimer = new QTimer(this);
    warmTimer->setInterval(10000);
    connect(warmTimer, &QTimer::timeout, this, &SubtitleTranslator::warmConnection);
    // Safety net: the region is re-read at least once a second, so a translation whose English
    // hasn't been seen for 2.5 s belongs to a line that is gone
    staleTimer = new QTimer(this);
    staleTimer->setInterval(500);
    connect(staleTimer, &QTimer::timeout, this, [this] {
        if (testFake && displayedSource == QLatin1String("Test subtitle"))
            return;
        if (!translatedText.isEmpty() && QDateTime::currentMSecsSinceEpoch() - sourceSeenMs > 2500)
            setDisplay(QString(), QString());
    });
    staleTimer->start();
    // Whether a PS5 picture is running, and the picture's size
    streamTimer = new QTimer(this);
    streamTimer->setInterval(250);
    connect(streamTimer, &QTimer::timeout, this, &SubtitleTranslator::pollStream);
    streamTimer->start();

    setStatus(!ocrAvailable ? tr("Okuma dosyaları bulunamadı") : enabled() ? tr("Yayın bekleniyor") : tr("Kapalı"));
}

SubtitleTranslator::~SubtitleTranslator()
{
    activeFlag.storeRelease(0);
    engine.subtitleTap(false, 0, 0, 0, 0, 250);
    if (tm)
        tm->save();
    QMetaObject::invokeMethod(worker, [w = worker] { w->stopLocalServer(); }, Qt::BlockingQueuedConnection);
    workerThread->quit();
    workerThread->wait();
    delete overlay;
}

QObject *SubtitleTranslator::localAiObject() const { return localAi; }

void SubtitleTranslator::setTestMode(bool on, bool dump, bool fakeLine)
{
    testOn = on;
    testFake = fakeLine;
    if (on) {
        // On for this run only: the value is set without being saved
        cfg->insert(QStringLiteral("enabled"), true);
        if (dump)
            cfg->insert(QStringLiteral("debugDump"), true);
        applyConfig();
        logInfo(QStringLiteral("test run: on (not saved) dump=%1 fake=%2").arg(dump).arg(fakeLine));
        setStatus(streamingFlag ? tr("Çalışıyor") : tr("Yayın bekleniyor"));
        emit overlayChanged();
    }
}

void SubtitleTranslator::loadConfig()
{
    QJsonObject saved;
    QFile f(configFile);
    if (f.open(QIODevice::ReadOnly))
        saved = QJsonDocument::fromJson(f.readAll()).object();
    const QVariantMap defaults = defaultConfig();
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        QVariant v = saved.contains(it.key()) ? saved.value(it.key()).toVariant() : it.value();
        if (v.metaType() != it.value().metaType())
            v.convert(it.value().metaType());
        cfg->insert(it.key(), v);
    }
    // The old default reading interval (250 ms) becomes the new one once; a value the user chose stays
    if (!saved.value(QStringLiteral("speed2")).toBool()) {
        if (cfg->value(QStringLiteral("intervalMs")).toInt() == 250)
            cfg->insert(QStringLiteral("intervalMs"), 150);
    }
    applyConfig();
}

void SubtitleTranslator::saveConfig()
{
    QJsonObject all;
    for (const QString &key : defaultConfig().keys()) {
        // A test run's "on" is not kept
        if (testOn && key == QLatin1String("enabled"))
            continue;
        all.insert(key, QJsonValue::fromVariant(cfg->value(key)));
    }
    all.insert(QStringLiteral("speed2"), true);
    if (testOn) {
        QFile f(configFile);
        if (f.open(QIODevice::ReadOnly))
            all.insert(QStringLiteral("enabled"), QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("enabled")).toBool());
    }
    QDir().mkpath(QFileInfo(configFile).absolutePath());
    QSaveFile out(configFile);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(all).toJson(QJsonDocument::Indented));
        if (!out.commit())
            logWarn(QStringLiteral("settings not saved: ") + configFile);
    }
}

void SubtitleTranslator::applyConfig()
{
    QMutexLocker locker(&paramsMutex);
    params.region = QRectF(cfg->value("regionX").toDouble(), cfg->value("regionY").toDouble(), cfg->value("regionW").toDouble(),
                           cfg->value("regionH").toDouble()).intersected(QRectF(0, 0, 1, 1));
    params.threshold = qBound(1, cfg->value("threshold").toInt(), 255);
    params.requireOutline = cfg->value("requireOutline").toBool();
    params.centeredOnly = cfg->value("centeredOnly").toBool();
    params.debugDump = cfg->value("debugDump").toBool();
    params.engine = cfg->value("engine").toInt();
    params.deeplKey = cfg->value("deeplKey").toString();
    params.llmUrl = cfg->value("llmUrl").toString();
    params.llmModel = cfg->value("llmModel").toString();
    params.llmKey = cfg->value("llmKey").toString();
    params.sourceLang = cfg->value("sourceLang").toString();
    params.targetLang = cfg->value("targetLang").toString();
    locker.unlock();
    if (tm)
        tm->setEnabled(cfg->value("sharedMemory").toBool());
    updateTap();
}

Params SubtitleTranslator::snapshotParams() const
{
    QMutexLocker locker(&paramsMutex);
    return params;
}

// Quick while there is text (or a line on screen to clear in time), slower after 2 s of nothing
void SubtitleTranslator::paceTap(bool textNow)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (textNow)
        lastTextMs = now;
    const bool relax = !textNow && now - lastTextMs > 2000;
    if (relax == relaxedTap)
        return;
    relaxedTap = relax;
    // Only the copy interval changes (the overlay is left alone)
    if (activeFlag.loadAcquire()) {
        const Params p = snapshotParams();
        const int interval = qBound(100, cfg->value("intervalMs").toInt(), 5000);
        engine.subtitleTap(true, float(p.region.x()), float(p.region.y()), float(p.region.width()), float(p.region.height()),
                           unsigned(relaxedTap ? qMax(300, interval * 2) : interval));
    }
}

// Engine region copies only while on and a PS5 picture runs
void SubtitleTranslator::updateTap()
{
    const bool active = enabled() && streamingFlag && ocrAvailable;
    activeFlag.storeRelease(active ? 1 : 0);
    const Params p = snapshotParams();
    const int interval = qBound(100, cfg->value("intervalMs").toInt(), 5000);
    engine.subtitleTap(active, float(p.region.x()), float(p.region.y()), float(p.region.width()), float(p.region.height()),
                       unsigned(relaxedTap ? qMax(300, interval * 2) : interval));
    const bool wantOverlay = (enabled() && streamingFlag) || selectingFlag;
    if (wantOverlay && !overlayTimer->isActive())
        overlayTimer->start();
    refreshOverlay(true);
    if (!wantOverlay)
        overlayTimer->stop();
    if (active && !dictRequested)
        loadDictionaries();
}

void SubtitleTranslator::setValue(const QString &key, const QVariant &value)
{
    cfg->insert(key, value);
    onConfigValueChanged(key, value);
}

void SubtitleTranslator::onConfigValueChanged(const QString &key, const QVariant &)
{
    saveTimer->start();
    const bool wasActive = activeFlag.loadRelaxed() != 0;
    applyConfig();
    if (key == QLatin1String("engine") || key == QLatin1String("targetLang") || key == QLatin1String("sourceLang"))
        clearCache();
    if (key.startsWith(QLatin1String("region")) || key == QLatin1String("threshold") || key == QLatin1String("requireOutline")
        || key == QLatin1String("centeredOnly"))
        QMetaObject::invokeMethod(worker, [w = worker] { w->reset(); }, Qt::QueuedConnection);
    if (key == QLatin1String("engine") && streamingFlag && enabled())
        warmConnection();
    if (key == QLatin1String("enabled")) {
        logInfo(QStringLiteral("translation %1").arg(enabled() ? QStringLiteral("on") : QStringLiteral("off")));
        if (!enabled()) {
            setDisplay(QString(), QString());
            ocrText.clear();
            setStatus(ocrAvailable ? tr("Kapalı") : tr("Okuma dosyaları bulunamadı"));
        } else {
            setStatus(!ocrAvailable ? tr("Okuma dosyaları bulunamadı") : streamingFlag ? tr("Çalışıyor") : tr("Yayın bekleniyor"));
            if (streamingFlag && !wasActive)
                warmConnection();
        }
    }
    emit overlayChanged();
}

void SubtitleTranslator::resetDefaults()
{
    const QVariantMap defaults = defaultConfig();
    const bool keepEnabled = enabled();
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        if (it.key() == QLatin1String("deeplKey") || it.key() == QLatin1String("llmKey"))
            continue;
        cfg->insert(it.key(), it.key() == QLatin1String("enabled") ? QVariant(keepEnabled) : it.value());
    }
    onConfigValueChanged(QStringLiteral("engine"), cfg->value("engine"));
}

void SubtitleTranslator::clearCache()
{
    cache.clear();
    ocrText.clear();
    pending.clear();
}

void SubtitleTranslator::setServer(OvioraServer *server)
{
    if (!tm) {
        tm = new TranslationMemory(this);
        connect(tm, &TranslationMemory::found, this, &SubtitleTranslator::onSharedFound);
        connect(tm, &TranslationMemory::variants, this, [this](const QString &sha, qint64 shown, const QVariantList &list, const QString &error) {
            if (TranslationMemory::lineSha(tmVariantsFor) == sha)
                emit variantsLoaded(tmVariantsFor, double(shown), list, error);
        });
    }
    tm->setServer(server);
    tm->setEnabled(cfg->value("sharedMemory").toBool());
    tm->setGame(gameId);
    // Test runs: OVIORA_TEST_TM_LOOKUP=<UTF-8 file, one English line per row> asks the shared memory
    // for those lines once signed in (lookup only, nothing is submitted) and logs what it has
    const QString lookupFile = qEnvironmentVariable("OVIORA_TEST_TM_LOOKUP");
    if (!lookupFile.isEmpty()) {
        QFile f(lookupFile);
        QStringList lines;
        if (f.open(QIODevice::ReadOnly))
            lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        auto *poll = new QTimer(this);
        auto *tries = new int(0);
        connect(tm, &TranslationMemory::found, this, [](const QString &sha, const QString &translation) {
            logInfo(QStringLiteral("test lookup hit %1: \"%2\"").arg(sha.left(12), translation.left(80)));
        });
        connect(poll, &QTimer::timeout, this, [this, poll, tries, lines] {
            if (!tm->active() && ++*tries < 60)
                return;
            poll->stop();
            poll->deleteLater();
            delete tries;
            // OVIORA_TEST_TM_GAME=<title id>: asks under that game instead of the running one
            if (const QString g = qEnvironmentVariable("OVIORA_TEST_TM_GAME"); !g.isEmpty())
                tm->setGame(g);
            logInfo(QStringLiteral("test lookup: %1 lines, memory active=%2, game %3").arg(lines.size()).arg(tm->active()).arg(tm->game()));
            for (const QString &l : lines) {
                const QString line = l.trimmed();
                if (const TranslationMemory::Hit *hit = tm->cached(line))
                    logInfo(QStringLiteral("test lookup kept %1: \"%2\"").arg(TranslationMemory::lineSha(line).left(12), hit->tr.left(80)));
                else
                    tm->lookup(line);
            }
        });
        poll->start(1000);
    }
}

void SubtitleTranslator::setGame(const QString &titleId)
{
    if (titleId == gameId)
        return;
    gameId = titleId;
    if (tm)
        tm->setGame(titleId);
    logInfo(QStringLiteral("game %1").arg(titleId.isEmpty() ? QStringLiteral("unknown (genel)") : titleId));
    if (dictRequested)
        reloadGlossary();
    emit gameChanged();
}

int runTermsTest(const QString &titleId)
{
    const auto g = loadGlossary(appDir() + QStringLiteral("/runtime/altyazi/terim"), titleId);
    logInfo(g ? QStringLiteral("test terms %1: %2").arg(titleId, g->games.join(QStringLiteral(", "))) : QStringLiteral("test terms %1: none").arg(titleId));
    for (const auto &[word, suffix] : std::initializer_list<std::pair<const char *, const char *>> {
             { "Dutch", "yi" }, { "Arthur", "nin" }, { "Molly", "e" }, { "Ekip", "i" }, { "Trevor", "ten" }, { "Saint Denis", "deki" } })
        logInfo(QStringLiteral("test suffix %1 + %2 -> %3").arg(QString::fromUtf8(word), QString::fromUtf8(suffix),
                                                               harmonizeSuffix(QString::fromUtf8(word), QString::fromUtf8(suffix), true)));
    if (!g)
        return 1;
    QStringList lines;
    QFile f(appDir() + QStringLiteral("/runtime/altyazi/terim-test.txt"));
    if (f.open(QIODevice::ReadOnly))
        lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty())
        lines = { QStringLiteral("Franklin, get to Lamar's house and bring Chop with you."),
                  QStringLiteral("Your Wanted Level is too high, lose the cops first."),
                  QStringLiteral("We need a crew for this Heist, call Lester."),
                  QStringLiteral("Tell Dutch we found the money in Saint Denis."),
                  QStringLiteral("Use Dead Eye to mark them, Arthur.") };
    QNetworkAccessManager nam;
    int failed = 0;
    for (QString line : lines) {
        line = line.trimmed();
        const ProtectedText prot = protectTerms(line, *g, true);
        QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
        QUrlQuery q;
        q.addQueryItem("client", "dict-chrome-ex");
        q.addQueryItem("sl", "en");
        q.addQueryItem("tl", "tr");
        q.addQueryItem("dt", "t");
        q.addQueryItem("q", prot.text);
        url.setQuery(q);
        QNetworkRequest req(url);
        req.setTransferTimeout(8000);
        QNetworkReply *reply = nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QString raw;
        for (const QJsonValue &seg : QJsonDocument::fromJson(reply->readAll()).array().at(0).toArray())
            raw += seg.toArray().at(0).toString();
        reply->deleteLater();
        failed += raw.isEmpty() ? 1 : 0;
        logInfo(QStringLiteral("test terms | %1 | sent: %2 | google: %3 | shown: %4").arg(line, prot.text, raw, restoreTerms(raw, prot)));
    }
    return failed ? 1 : 0;
}

// Term lists: runtime/altyazi/terim/*.json, genel.json plus the running game's (GameGlossary.h)
void SubtitleTranslator::reloadGlossary()
{
    std::shared_ptr<const Glossary> g = loadGlossary(appDir() + QStringLiteral("/runtime/altyazi/terim"), gameId);
    logInfo(g ? QStringLiteral("terms: %1 (%2 terms, %3 names)").arg(g->games.join(QStringLiteral(", "))).arg(g->terms.size()).arg(g->keep.size())
              : QStringLiteral("terms: none"));
    QMutexLocker locker(&paramsMutex);
    params.glossary = std::move(g);
}

bool SubtitleTranslator::sharedMemoryActive() const { return tm && tm->active(); }

// The server knows a translation for a line: it replaces the machine one, also the one on screen now
void SubtitleTranslator::onSharedFound(const QString &sha, const QString &translation)
{
    for (auto it = pending.begin(); it != pending.end();) {
        if (TranslationMemory::lineSha(it.key()) == sha) {
            const PendingTranslation p = it.value();
            const QString source = it.key();
            it = pending.erase(it);
            cache.insert(normalizeForCompare(source), translation);
            logInfo(QStringLiteral("shared memory had it: \"%1\"").arg(source.left(80)));
            if (textsSimilar(ocrText, source))
                showTranslation(p.seq, source, translation, p.box, p.lines, p.rows);
        } else {
            ++it;
        }
    }
    if (!displayedSource.isEmpty() && TranslationMemory::lineSha(displayedSource) == sha && translatedText != translation) {
        cache.insert(normalizeForCompare(displayedSource), translation);
        setDisplay(displayedSource, translation, displayBox, displayLines, displayRows);
    }
}

void SubtitleTranslator::openCorrection()
{
    if (displayedSource.isEmpty() || translatedText.isEmpty()) {
        setStatus(tr("Düzeltilecek bir altyazı yok"));
        return;
    }
    tmVariantsFor = displayedSource;
    emit correctionRequested(displayedSource, translatedText);
}

void SubtitleTranslator::loadVariants(const QString &english)
{
    tmVariantsFor = english;
    if (tm)
        tm->loadVariants(english);
    else
        emit variantsLoaded(english, 0, QVariantList(), tr("Ortak çeviri hafızası kapalı."));
}

void SubtitleTranslator::correct(const QString &english, const QString &translation)
{
    tmVariantsFor = english;
    // Shown at once here; the server's answer (the list) follows
    if (!translation.trimmed().isEmpty()) {
        cache.insert(normalizeForCompare(english), translation.trimmed());
        if (displayedSource == english)
            setDisplay(displayedSource, translation.trimmed(), displayBox, displayLines, displayRows);
    }
    if (tm)
        tm->correct(english, translation);
}

void SubtitleTranslator::vote(double id, int value, const QString &english)
{
    tmVariantsFor = english;
    if (tm)
        tm->vote(qint64(id), value, english);
}

void SubtitleTranslator::pollStream()
{
    const auto snap = engine.snapshot();
    const bool live = snap.running && (snap.remotePlay || snap.capture) && !snap.image;
    const QSize size(int(snap.metrics.resolution.output.width), int(snap.metrics.resolution.output.height));
    if (size.isValid() && size != frameSize)
        frameSize = size;
    if (live == streamingFlag)
        return;
    streamingFlag = live;
    logInfo(QStringLiteral("stream %1 (picture %2x%3)").arg(live ? QStringLiteral("running") : QStringLiteral("ended"))
                .arg(frameSize.width()).arg(frameSize.height()));
    if (live) {
        warmConnection();
        warmTimer->start();
        if (testFake && enabled())
            setDisplay(QStringLiteral("Test subtitle"), QStringLiteral("Deneme altyazısı: yer ve boyut kontrolü"),
                       QRectF(0.35, 0.86, 0.30, 0.035), 1);
    } else {
        warmTimer->stop();
        clearTimer->stop();
        if (tm)
            tm->save();
        setSelecting(false);
        setDisplay(QString(), QString());
        ocrText.clear();
        pending.clear();
        emptyStreak = 0;
        candidateHits = 0;
    }
    if (enabled() && ocrAvailable)
        setStatus(live ? tr("Çalışıyor") : tr("Yayın bekleniyor"));
    updateTap();
    emit overlayChanged();
}

void SubtitleTranslator::warmConnection()
{
    if (!enabled() || !streamingFlag)
        return;
    const Params p = snapshotParams();
    QMetaObject::invokeMethod(worker, [w = worker, p] {
        w->ensureLocalServer(p);
        w->warm(p);
    }, Qt::QueuedConnection);
}

void SubtitleTranslator::setSelecting(bool on)
{
    if (on && !streamingFlag)
        on = false;
    if (on == selectingFlag)
        return;
    selectingFlag = on;
    dragRect = QRectF();
    emit selectingChanged();
    updateTap();
}

void SubtitleTranslator::setStatus(const QString &status)
{
    if (status == statusText)
        return;
    statusText = status;
    emit statusChanged();
}

void SubtitleTranslator::setDisplay(const QString &source, const QString &translation, const QRectF &box, int lines,
                                    const QVariantList &rows)
{
    if (source == displayedSource && translation == translatedText && box == displayBox)
        return;
    displayedSource = source;
    translatedText = translation;
    displayBox = box;
    displayLines = lines;
    displayRows = rows;
    emit textChanged();
    refreshOverlay(true);
}

void SubtitleTranslator::onOcrResult(const QString &text, int frameW, int frameH, const QRectF &box, int lines, const QVariantList &rows,
                                     qint64 sampleMs, double score, bool fresh)
{
    Q_UNUSED(frameW);
    Q_UNUSED(frameH);
    if (!enabled())
        return;
    paceTap(!text.isEmpty() || !ocrText.isEmpty());
    if (fresh)
        noteCleanRead(text, score);
    if (text.isEmpty()) {
        candidateHits = 0;
        // Two empty samples in a row, so a single bad read doesn't blink the subtitle
        if (++emptyStreak >= 2 && !ocrText.isEmpty()) {
            ocrText.clear();
            requestClear();
        }
        return;
    }
    if (textsSimilar(text, displayedSource))
        sourceSeenMs = QDateTime::currentMSecsSinceEpoch();
    if (textsSimilar(text, ocrText)) {
        emptyStreak = 0;
        candidateHits = 0;
        return;
    }
    // A new text is taken only once it was read twice in a row, alike and at about the same place:
    // a plate or sign passing through the region rarely holds still, a subtitle does
    if (candidateHits > 0 && textSimilarity(text, candidateText) >= 0.85 && samePlace(box, lines, candidateBox, candidateLines)) {
        candidateHits++;
    } else {
        candidateText = text;
        candidateBox = box;
        candidateLines = lines;
        candidateHits = 1;
        candidateSinceMs = sampleMs;
    }
    if (candidateHits < 2) {
        // The translation starts at the first read: by the time the second read confirms the line it is
        // usually back (from the cache then). Not for lines the dictionaries, the cache or the shared
        // memory already have, nor for crumbs
        if (candidateHits == 1 && text.count(QLatin1Char(' ')) >= 1 && text.size() >= 8)
            requestEarly(text, box, lines, rows);
        if (++emptyStreak >= 2 && !ocrText.isEmpty()) {
            ocrText.clear();
            requestClear();
        }
        return;
    }
    candidateHits = 0;
    emptyStreak = 0;
    // A new line replaced the one on screen: drop its translation at once
    if (!translatedText.isEmpty() && !textsSimilar(text, displayedSource)) {
        clearTimer->stop();
        setDisplay(QString(), QString());
    }
    ocrText = text;
    ocrBox = box;
    ocrLines = lines;
    ocrRows = rows;
    lineSinceMs = candidateSinceMs ? candidateSinceMs : sampleMs;
    logInfo(QStringLiteral("subtitle read: \"%1\" (%2 rows, confirmed %3 ms after its first read)").arg(text.left(120)).arg(lines)
                .arg(QDateTime::currentMSecsSinceEpoch() - lineSinceMs));

    // A human translation of the game's own text beats any machine translation
    // Test runs: OVIORA_TEST_NO_DICTIONARY=1 leaves the dictionaries out (this run only, nothing saved)
    static const bool noDictionary = qEnvironmentVariableIntValue("OVIORA_TEST_NO_DICTIONARY") > 0;
    const QString fromDictionary = cfg->value("useDictionary").toBool() && !noDictionary ? findInDictionary(text) : QString();
    if (!fromDictionary.isEmpty()) {
        logInfo(QStringLiteral("from the dictionary"));
        showTranslation(nextSeq++, text, fromDictionary, box, lines, rows);
        return;
    }
    const auto it = cache.constFind(normalizeForCompare(text));
    if (it != cache.constEnd()) {
        showTranslation(nextSeq++, text, it.value(), box, lines, rows);
        return;
    }
    // Someone already translated (or corrected) this line on the server
    if (tm && tm->active()) {
        if (const TranslationMemory::Hit *hit = tm->cached(text)) {
            logInfo(QStringLiteral("from the shared memory (kept from an earlier answer)"));
            cache.insert(normalizeForCompare(text), hit->tr);
            showTranslation(nextSeq++, text, hit->tr, box, lines, rows);
            return;
        }
        tm->lookup(text);  // answered while the machine translation runs; found() replaces it
    }
    requestTranslation(text);
}

// Dictionaries: runtime/altyazi/sozluk/*.json, {"English game text": "Turkish"}, e.g. built from a
// game's community translation. Keys are matched like the OCR text (letters and digits only).
// Read on the worker thread the first time the translation runs.
void SubtitleTranslator::loadDictionaries()
{
    dictRequested = true;
    reloadGlossary();
    QMetaObject::invokeMethod(worker, [this] {
        auto result = std::make_shared<std::pair<QHash<QString, QString>, QSet<QString>>>();
        QHash<QString, QPair<int, int>> wordCounts;  // lower case uses, all uses
        const QDir dir(dictionaryDir());
        int files = 0;
        for (const QFileInfo &fi : dir.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name)) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
                continue;
            files++;
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
                // Its English words count as English for the subtitle-likeness check; names (nearly
                // always written with a capital) do not
                for (const QString &w : sentenceWords(it.key())) {
                    QPair<int, int> &n = wordCounts[w.toLower()];
                    if (w == w.toLower())
                        n.first++;
                    n.second++;
                }
                const QString key = normalizeForCompare(it.key());
                const QString value = it.value().toString().trimmed();
                if (key.size() < 2 || value.isEmpty() || result->first.contains(key))
                    continue;
                result->first.insert(key, value);
            }
        }
        for (auto it = wordCounts.constBegin(); it != wordCounts.constEnd(); ++it) {
            if (it.value().first > 0 && it.value().first * 3 >= it.value().second)
                result->second.insert(it.key());
        }
        setExtraWords(result->second);
        logInfo(QStringLiteral("dictionaries: %1 files, %2 lines, %3 words").arg(files).arg(result->first.size()).arg(result->second.size()));
        QMetaObject::invokeMethod(this, [this, result] {
            dict = std::move(result->first);
            dictKeys.clear();
            dictByPrefix.clear();
            dictBySuffix.clear();
            for (auto it = dict.constBegin(); it != dict.constEnd(); ++it) {
                // Near-match index: OCR slips rarely hit both ends of a line
                const int idx = dictKeys.size();
                dictKeys << it.key();
                dictByPrefix.insert(it.key().left(8), idx);
                dictBySuffix.insert(it.key().right(8), idx);
            }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

QString SubtitleTranslator::findInDictionary(const QString &text) const
{
    if (dict.isEmpty())
        return QString();
    const QString key = normalizeForCompare(text);
    const auto exact = dict.constFind(key);
    if (exact != dict.constEnd())
        return exact.value();
    if (key.size() < 12)
        return QString();  // short lines: a near match is likely a different line
    QString best;
    double bestScore = 0.9;
    auto consider = [&](int idx) {
        const QString &candidate = dictKeys.at(idx);
        if (qAbs(candidate.size() - key.size()) > key.size() / 8 || candidate.size() > 600)
            return;
        const double score = 1.0 - double(levenshtein(key, candidate)) / qMax(candidate.size(), key.size());
        if (score >= bestScore) {
            bestScore = score;
            best = dict.value(candidate);
        }
    };
    for (auto it = dictByPrefix.constFind(key.left(8)); it != dictByPrefix.constEnd() && it.key() == key.left(8); ++it)
        consider(it.value());
    for (auto it = dictBySuffix.constFind(key.right(8)); it != dictBySuffix.constEnd() && it.key() == key.right(8); ++it)
        consider(it.value());
    return best;
}

// A line read once, not confirmed yet: its translation asked for now (see onOcrResult)
void SubtitleTranslator::requestEarly(const QString &source, const QRectF &box, int lines, const QVariantList &rows)
{
    if (pending.contains(source) || cache.contains(normalizeForCompare(source)))
        return;
    static const bool noDictionary = qEnvironmentVariableIntValue("OVIORA_TEST_NO_DICTIONARY") > 0;
    if (cfg->value("useDictionary").toBool() && !noDictionary && !findInDictionary(source).isEmpty())
        return;
    if (tm && tm->active() && tm->cached(source))
        return;
    PendingTranslation p;
    p.seq = nextSeq++;
    p.box = box;
    p.lines = lines;
    p.rows = rows;
    p.startedMs = QDateTime::currentMSecsSinceEpoch();
    p.early = true;
    pending.insert(source, p);
    const Params params = snapshotParams();
    QMetaObject::invokeMethod(worker, [w = worker, source, params] { w->translate(source, params); }, Qt::QueuedConnection);
}

void SubtitleTranslator::requestTranslation(const QString &source)
{
    // Asked early already: it is now wanted on screen (keeps its sequence, the box of the confirmed read)
    if (const auto it = pending.find(source); it != pending.end()) {
        if (it->early) {
            it->early = false;
            it->box = ocrBox;
            it->lines = ocrLines;
            it->rows = ocrRows;
            logInfo(QStringLiteral("translation requested (asked at the first read %1 ms ago)")
                        .arg(QDateTime::currentMSecsSinceEpoch() - it->startedMs));
        }
        return;
    }
    PendingTranslation p;
    p.seq = nextSeq++;
    p.box = ocrBox;
    p.lines = ocrLines;
    p.rows = ocrRows;
    p.startedMs = QDateTime::currentMSecsSinceEpoch();
    pending.insert(source, p);
    const Params params = snapshotParams();
    logInfo(QStringLiteral("translation requested (engine %1)").arg(params.engine));
    QMetaObject::invokeMethod(worker, [w = worker, source, params] { w->translate(source, params); }, Qt::QueuedConnection);
}

void SubtitleTranslator::onTranslated(const QString &source, const QString &translation, const QString &error)
{
    const auto it = pending.constFind(source);
    if (it == pending.constEnd())
        return;  // cleared meanwhile (region or engine changed, stream ended)
    const PendingTranslation p = it.value();
    pending.erase(it);
    if (!error.isEmpty()) {
        setStatus(error);
        logWarn(QStringLiteral("translation failed: ") + error);
        return;
    }
    if (cache.size() > 5000)
        cache.clear();
    cache.insert(normalizeForCompare(source), translation);
    const qint64 tookMs = QDateTime::currentMSecsSinceEpoch() - p.startedMs;
    // Asked at the first read and the line was not confirmed (yet): kept in the cache only, never offered
    // to the shared memory. If it is confirmed later, the cache answers at once
    if (p.early && !textsSimilar(ocrText, source)) {
        logInfo(QStringLiteral("early translation kept for later (%1 ms)").arg(tookMs));
        return;
    }
    // Offered to the shared memory (sent only if the server has none for this line), but only a
    // clean read of the English: see offerToMemory
    if (tm && tm->active()) {
        const int engineId = snapshotParams().engine;
        offerToMemory(source, translation, engineId == 2 || engineId == 4 ? QStringLiteral("llm") : QStringLiteral("google"));
    }
    logInfo(QStringLiteral("translated in %1 ms: \"%2\"").arg(tookMs).arg(translation.left(120)));
    setStatus(tr("Çalışıyor — son çeviri %1 ms").arg(tookMs));
    // Shown while its English is still on screen. If the line already left and nothing replaced it,
    // it still gets its minimum time on screen; if another line replaced it, it is stale and skipped.
    if (textsSimilar(ocrText, source)) {
        showTranslation(p.seq, source, translation, p.box, p.lines, p.rows);
    } else if (ocrText.isEmpty()) {
        showTranslation(p.seq, source, translation, p.box, p.lines, p.rows);
        requestClear();
    }
}

// The shared memory is read by every player: a garbled read ("x100 This was great.", "nanaie
// contracts") stays there for everyone. A line is offered only when the English was read clean:
// two fresh OCR reads in a row word for word the same, both with high confidence, and the text
// passes submitReason (dictionary words, no HUD crumbs). Otherwise it is still shown here, just
// not shared. A line waiting for its second clean read is offered when that read comes.
static constexpr double kSubmitMinScore = 0.90;

void SubtitleTranslator::noteCleanRead(const QString &text, double score)
{
    const QString norm = text.isEmpty() ? QString() : normalizeForCompare(text);
    if (!norm.isEmpty() && norm == lastFreshNorm) {
        const double pairScore = qMin(score, lastFreshScore);
        if (pairScore >= kSubmitMinScore) {
            if (cleanReads.size() > 2000)
                cleanReads.clear();
            cleanReads.insert(norm);
            const auto it = submitWaiting.find(norm);
            if (it != submitWaiting.end()) {
                const SubmitWaiting w = it.value();
                submitWaiting.erase(it);
                offerToMemory(w.source, w.translation, w.engine);
            }
        } else if (submitWaiting.contains(norm)) {
            bestPairScore.insert(norm, qMax(bestPairScore.value(norm), pairScore));
        }
    }
    lastFreshNorm = norm;
    lastFreshScore = score;
    // Lines no longer on screen that never got their clean second read are let go
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = submitWaiting.begin(); it != submitWaiting.end();) {
        if (now - it.value().sinceMs > 15000) {
            const double best = bestPairScore.value(it.key(), -1);
            logInfo(QStringLiteral("shared memory: not submitted (%1): \"%2\"")
                        .arg(best >= 0 ? QStringLiteral("OCR guveni dusuk, en iyi %1%").arg(int(best * 100))
                                       : QStringLiteral("ikinci temiz okuma yok"),
                             it.value().source.left(100)));
            bestPairScore.remove(it.key());
            it = submitWaiting.erase(it);
        } else {
            ++it;
        }
    }
}

void SubtitleTranslator::offerToMemory(const QString &source, const QString &translation, const QString &engine)
{
    if (!tm || !tm->active())
        return;
    const QString reason = submitReason(source);
    if (!reason.isEmpty()) {
        logInfo(QStringLiteral("shared memory: not submitted (%1): \"%2\"").arg(reason, source.left(100)));
        return;
    }
    const QString norm = normalizeForCompare(source);
    if (!cleanReads.contains(norm)) {
        if (submitWaiting.size() > 50)
            submitWaiting.clear();
        submitWaiting.insert(norm, SubmitWaiting { source, translation, engine, QDateTime::currentMSecsSinceEpoch() });
        return;
    }
    logInfo(QStringLiteral("shared memory: clean read, offered: \"%1\"").arg(source.left(100)));
    tm->offer(source, translation, engine);
}

// Shown if newer than what is on screen, even when the game already moved to the next line,
// which is still translating; out-of-order older answers are ignored
void SubtitleTranslator::showTranslation(quint64 seq, const QString &source, const QString &translation, const QRectF &box,
                                         int lines, const QVariantList &rows)
{
    if (seq < displayedSeq)
        return;
    displayedSeq = seq;
    displayedAtMs = QDateTime::currentMSecsSinceEpoch();
    // From the first sample that held the English to the Turkish on screen
    if (textsSimilar(source, ocrText) && lineSinceMs > 0)
        logInfo(QStringLiteral("shown %1 ms after the line was first read").arg(displayedAtMs - lineSinceMs));
    sourceSeenMs = displayedAtMs;
    clearTimer->stop();
    setDisplay(source, translation, box, lines, rows);
}

// Every translation stays up at least this long, so quick lines can be read
static constexpr qint64 kMinDisplayMs = 1200;

void SubtitleTranslator::requestClear()
{
    const quint64 seq = displayedSeq;
    const qint64 shownFor = QDateTime::currentMSecsSinceEpoch() - displayedAtMs;
    if (shownFor >= kMinDisplayMs) {
        setDisplay(QString(), QString());
        return;
    }
    clearTimer->stop();
    disconnect(clearTimer, &QTimer::timeout, this, nullptr);
    connect(clearTimer, &QTimer::timeout, this, [this, seq] {
        if (displayedSeq == seq)
            setDisplay(QString(), QString());
    });
    clearTimer->start(int(kMinDisplayMs - shownFor));
}

void SubtitleTranslator::setRegion(const QRectF &r)
{
    QRectF n = r;
    n.setWidth(qBound(0.05, n.width(), 1.0));
    n.setHeight(qBound(0.03, n.height(), 1.0));
    n.moveLeft(qBound(0.0, n.left(), 1.0 - n.width()));
    n.moveTop(qBound(0.0, n.top(), 1.0 - n.height()));
    const QPair<const char *, double> values[] = {
        { "regionX", n.x() }, { "regionY", n.y() }, { "regionW", n.width() }, { "regionH", n.height() },
    };
    for (const auto &v : values)
        cfg->insert(QString::fromLatin1(v.first), qRound(v.second * 1000.0) / 1000.0);
    onConfigValueChanged(QStringLiteral("regionX"), cfg->value("regionX"));
}

// Video window fractions (as the QML mouse area over the picture reports them) to the frame
QRectF SubtitleTranslator::regionFromFractions(qreal x0, qreal y0, qreal x1, qreal y1) const
{
    RECT c{};
    if (!videoWindow || !GetClientRect(videoWindow, &c) || c.right < 1 || c.bottom < 1)
        return QRectF();
    const auto view = engine.previewView();
    const float cw = float(c.right), ch = float(c.bottom);
    const float iw = frameSize.width() > 0 ? float(frameSize.width()) : 1920.f;
    const float ih = frameSize.height() > 0 ? float(frameSize.height()) : 1080.f;
    const auto a = view.sourcePoint(float(x0 * cw), float(y0 * ch), cw, ch, iw, ih);
    const auto b = view.sourcePoint(float(x1 * cw), float(y1 * ch), cw, ch, iw, ih);
    const QRectF r = QRectF(QPointF(a.first, a.second), QPointF(b.first, b.second)).normalized();
    return r.intersected(QRectF(0, 0, 1, 1));
}

void SubtitleTranslator::dragSelection(qreal x0, qreal y0, qreal x1, qreal y1)
{
    if (!selectingFlag)
        return;
    dragRect = x0 < 0 ? QRectF() : regionFromFractions(x0, y0, x1, y1);
    refreshOverlay(true);
}

void SubtitleTranslator::selectRegion(qreal x0, qreal y0, qreal x1, qreal y1)
{
    RECT c{};
    if (!videoWindow || !GetClientRect(videoWindow, &c))
        return;
    // Too small to be meant (the old app: 20 x 10 px)
    if (qAbs(x1 - x0) * c.right < 20 || qAbs(y1 - y0) * c.bottom < 10) {
        dragRect = QRectF();
        refreshOverlay(true);
        return;
    }
    const QRectF r = regionFromFractions(x0, y0, x1, y1);
    if (r.width() <= 0 || r.height() <= 0)
        return;
    setRegion(r);
    logInfo(QStringLiteral("region drawn: %1,%2 %3x%4").arg(r.x(), 0, 'f', 3).arg(r.y(), 0, 'f', 3).arg(r.width(), 0, 'f', 3).arg(r.height(), 0, 'f', 3));
    ocrText.clear();
    pending.clear();
    setDisplay(QString(), QString());
    if (!enabled())
        setValue(QStringLiteral("enabled"), true);
    setSelecting(false);
}

bool SubtitleTranslator::handleKey(int key, int modifiers)
{
    if (!(modifiers & Qt::ControlModifier) || !streamingFlag)
        return false;
    if (key == Qt::Key_E) {
        openCorrection();
        return true;
    }
    if (key == Qt::Key_B) {
        setSelecting(!selectingFlag);
        return true;
    }
    auto toggle = [this](const char *name) { setValue(QString::fromLatin1(name), !cfg->value(name).toBool()); };
    switch (key) {
    case Qt::Key_T:
        toggle("enabled");
        return true;
    case Qt::Key_R:
        toggle("showRegion");
        return true;
    case Qt::Key_D:
        toggle("debugDump");
        setStatus(cfg->value("debugDump").toBool() ? tr("Hata kaydı açık (logs/altyazi klasörü)") : tr("Çalışıyor"));
        return true;
    default:
        break;
    }
    // Region editing keys only while the region is shown
    if (!cfg->value("showRegion").toBool())
        return false;
    const bool shift = modifiers & Qt::ShiftModifier;
    const double step = 0.01;
    QRectF r = snapshotParams().region;
    switch (key) {
    case Qt::Key_Left:
        shift ? r.adjust(step, 0, -step, 0) : r.translate(-step, 0);
        break;
    case Qt::Key_Right:
        shift ? r.adjust(-step, 0, step, 0) : r.translate(step, 0);
        break;
    case Qt::Key_Up:
        shift ? r.adjust(0, -step, 0, 0) : r.translate(0, -step);
        break;
    case Qt::Key_Down:
        shift ? r.adjust(0, step, 0, 0) : r.translate(0, step);
        break;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        setValue(QStringLiteral("threshold"), qBound(50, cfg->value("threshold").toInt() + (key == Qt::Key_PageUp ? 10 : -10), 250));
        return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
    case Qt::Key_Minus:
        setValue(QStringLiteral("fontRel"), qBound(0.5, cfg->value("fontRel").toDouble() + (key == Qt::Key_Minus ? -0.1 : 0.1), 2.0));
        return true;
    default:
        return false;
    }
    setRegion(r);
    return true;
}

void SubtitleTranslator::refreshOverlay(bool force)
{
    Q_UNUSED(force);
    const bool want = (enabled() && streamingFlag) || selectingFlag;
    if (!want) {
        if (overlay)
            overlay->hide();
        return;
    }
    if (!overlay)
        overlay = new Overlay(videoWindow);
    OverlayState s;
    s.text = translatedText;
    s.source = displayedSource;
    s.region = snapshotParams().region;
    s.textBox = displayBox;
    s.lines = displayLines;
    s.rows = displayRows;
    s.fontRel = cfg->value("fontRel").toDouble();
    s.boxOpacity = cfg->value("boxOpacity").toInt();
    s.coverOriginal = cfg->value("coverOriginal").toBool();
    s.showSource = cfg->value("showSource").toBool();
    s.showRegion = cfg->value("showRegion").toBool();
    s.selecting = selectingFlag;
    s.drag = dragRect;
    s.selectHint = tr("Altyazının çıktığı yere fareyle kutu çiz\nEsc, sağ tık veya Ctrl+B: vazgeç");
    const bool shown = overlay->update(s, engine.previewView(), frameSize.width(), frameSize.height());
    static bool loggedFirst = false;
    if (shown && !s.text.isEmpty() && !loggedFirst) {
        loggedFirst = true;
        logInfo(QStringLiteral("overlay drawn: \"%1\"").arg(s.text.left(80)));
    }
}

} // namespace oviora::subtitle

#include "SubtitleTranslator.moc"
