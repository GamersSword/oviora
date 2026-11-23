// SPDX-License-Identifier: GPL-3.0-only
// E2E test publisher driving oviora::platform::IzletPublisher from an Annex-B
// H.264 or H.265 file against the mock PHP server (no PS5 or desktop UI).
#include "H264RelayCache.h"
#include "IzletMediaCache.h"
#include "oviora/Log.h"
#include "oviora/platform/IzletPublisher.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestAu {
    std::vector<std::uint8_t> configBytes;
    std::vector<std::uint8_t> frameBytes;
    bool isIdr = false;
};

std::vector<TestAu> splitH265Frames(std::span<const std::uint8_t> stream) {
    struct Nal {
        std::size_t start = 0;
        std::size_t hdr = 0;
        std::size_t end = 0;
        unsigned type = 0;
    };
    std::vector<Nal> nals;
    std::size_t pos = 0;
    while (pos + 4 <= stream.size()) {
        std::size_t prefix = 0;
        if (stream[pos] == 0 && stream[pos + 1] == 0) {
            if (stream[pos + 2] == 1) prefix = 3;
            else if (pos + 4 <= stream.size() && stream[pos + 2] == 0 && stream[pos + 3] == 1) prefix = 4;
        }
        if (!prefix) {
            ++pos;
            continue;
        }
        const std::size_t hdr = pos + prefix;
        if (hdr + 1 >= stream.size()) break;
        if (!nals.empty()) nals.back().end = pos;
        nals.push_back(Nal{pos, hdr, stream.size(), unsigned((stream[hdr] >> 1) & 0x3Fu)});
        pos = hdr + 2;
    }

    std::vector<TestAu> frames;
    std::vector<std::uint8_t> pendingConfig;
    std::vector<std::uint8_t> currentAu;
    bool hasSlice = false;
    bool isIdr = false;

    auto flush = [&]() {
        if (!hasSlice || currentAu.empty()) return;
        frames.push_back(TestAu{pendingConfig, std::move(currentAu), isIdr});
        currentAu.clear();
        pendingConfig.clear();
        hasSlice = false;
        isIdr = false;
    };

    for (const auto& n : nals) {
        const auto slice = stream.subspan(n.start, n.end - n.start);
        if (n.type == 35 /* AUD */) {
            flush();
            continue;
        }
        if (n.type == 32 || n.type == 33 || n.type == 34) {
            if (hasSlice) flush();
            pendingConfig.insert(pendingConfig.end(), slice.begin(), slice.end());
            continue;
        }
        const bool isVcl = (n.type <= 31u);
        if (isVcl && hasSlice) {
            // first_slice_segment_in_pic_flag is MSB of byte after 2-byte NAL header
            const bool firstSlice = (n.hdr + 2 < n.end) && ((stream[n.hdr + 2] & 0x80u) != 0);
            if (firstSlice) flush();
        }
        currentAu.insert(currentAu.end(), slice.begin(), slice.end());
        if (isVcl) {
            hasSlice = true;
            if (n.type == 19 || n.type == 20 || n.type == 21) isIdr = true;
        }
    }
    flush();
    return frames;
}

std::vector<TestAu> loadTestStream(const std::filesystem::path& path, const QString& codec) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (codec.compare(QLatin1String("h265"), Qt::CaseInsensitive) == 0) {
        return splitH265Frames(bytes);
    }
    const auto h264 = oviora::gecit::splitAnnexBFrames(bytes);
    std::vector<TestAu> out;
    out.reserve(h264.size());
    for (const auto& f4 : h264) {
        out.push_back(TestAu{f4.configBytes, f4.frameBytes, f4.isIdr});
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString token;

    QString apiUrl = QStringLiteral("http://127.0.0.1:18081/api.php");
    QString sourcePath;
    QString codec = QStringLiteral("h264");
    int fps = 60;
    int durationMs = 15000;

    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--api") && i + 1 < argc) apiUrl = QString::fromLocal8Bit(argv[++i]);
        else if (a == QLatin1String("--kaynak") && i + 1 < argc) sourcePath = QString::fromLocal8Bit(argv[++i]);
        else if (a == QLatin1String("--token") && i + 1 < argc) token = QString::fromLocal8Bit(argv[++i]);
        else if (a == QLatin1String("--codec") && i + 1 < argc) codec = QString::fromLocal8Bit(argv[++i]).toLower();
        else if (a == QLatin1String("--fps") && i + 1 < argc) fps = std::max(1, QString::fromLocal8Bit(argv[++i]).toInt());
        else if (a == QLatin1String("--sure-ms") && i + 1 < argc) durationMs = std::max(500, QString::fromLocal8Bit(argv[++i]).toInt());
    }

    if (sourcePath.isEmpty()) {
        std::cerr << "Kullanim: oviora_izlet_test_pub --api <url> --kaynak <dosya> [--codec h264|h265] [--fps 60] [--sure-ms 15000]\n";
        return 2;
    }

    const auto frames = loadTestStream(std::filesystem::path(sourcePath.toStdWString()), codec);
    if (frames.empty()) {
        std::cerr << "Video kareleri yuklenemedi\n";
        return 3;
    }

    QNetworkAccessManager net;
    oviora::platform::IzletPublisher publisher;
    QString yid;
    qint64 lastSigId = 0;
    bool connected = false;
    bool stopping = false;
    std::size_t frameIdx = 0;
    std::atomic<bool> idrPending{false};

    auto postApi = [&](QJsonObject body, std::function<void(const QJsonObject&, bool)> done) {
        // --token: the real api.php (a player's account), as the desktop app sends it
        if (!token.isEmpty()) { body[QStringLiteral("token")] = token; body[QStringLiteral("client")] = QStringLiteral("win:test"); }
        QNetworkRequest req{QUrl(apiUrl)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
        req.setTransferTimeout(4000);
        QNetworkReply* reply = net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        QObject::connect(reply, &QNetworkReply::finished, &app, [reply, done = std::move(done)]() {
            const bool ok = (reply->error() == QNetworkReply::NoError &&
                             reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() < 400);
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (done) done(doc.isObject() ? doc.object() : QJsonObject{}, ok);
        });
    };

    QObject::connect(&publisher, &oviora::platform::IzletPublisher::stateChanged, &app, [&](const QString& st) {
        std::cout << "PUB_STATE=" << st.toStdString() << "\n" << std::flush;
        if (st == QLatin1String("connected")) connected = true;
    });
    QObject::connect(&publisher, &oviora::platform::IzletPublisher::idrRequested, &app, [&]() {
        idrPending.store(true);
    });

    auto* pollTimer = new QTimer(&app);
    pollTimer->setInterval(100);
    QObject::connect(pollTimer, &QTimer::timeout, &app, [&]() {
        if (yid.isEmpty() || stopping) return;
        postApi(QJsonObject{{QStringLiteral("a"), QStringLiteral("izlet_poll")},
                            {QStringLiteral("yid"), yid},
                            {QStringLiteral("after"), lastSigId}},
                [&](const QJsonObject& r, bool ok) {
                    if (!ok || stopping) return;
                    lastSigId = std::max<qint64>(lastSigId, r.value(QStringLiteral("last")).toInteger(lastSigId));
                    for (const auto& v : r.value(QStringLiteral("signals")).toArray()) {
                        publisher.onRemoteSignal(v.toObject().value(QStringLiteral("data")).toObject());
                    }
                    if (r.value(QStringLiteral("state")).toString() == QLatin1String("bitti")) {
                        stopping = true;
                        publisher.stop();
                        app.quit();
                    }
                });
    });

    auto* videoTimer = new QTimer(&app);
    videoTimer->setInterval(std::max(5, 1000 / fps));
    QObject::connect(videoTimer, &QTimer::timeout, &app, [&]() {
        if (!connected || stopping) return;
        if (idrPending.exchange(false)) {
            for (std::size_t k = 0; k < frames.size(); ++k) {
                const std::size_t cand = (frameIdx + k) % frames.size();
                if (frames[cand].isIdr) {
                    frameIdx = cand;
                    break;
                }
            }
        }
        const auto& f = frames[frameIdx];
        frameIdx = (frameIdx + 1) % frames.size();
        if (!f.configBytes.empty()) {
            publisher.pushVideo(oviora::remoteplay::SampleKind::CodecConfig, f.configBytes, false, false);
        }
        publisher.pushVideo(oviora::remoteplay::SampleKind::AccessUnit, f.frameBytes, f.isIdr, false);
    });

    auto* audioTimer = new QTimer(&app);
    audioTimer->setInterval(20);
    QObject::connect(audioTimer, &QTimer::timeout, &app, [&]() {
        if (!connected || stopping) return;
        static constexpr std::uint8_t kOpusSilence[3] = {0xFC, 0xFF, 0xFE};
        publisher.pushAudio(kOpusSilence, 960u);
    });

    QTimer::singleShot(durationMs, &app, [&]() {
        if (stopping) return;
        stopping = true;
        videoTimer->stop();
        audioTimer->stop();
        pollTimer->stop();
        publisher.stop();
        if (!yid.isEmpty()) {
            postApi(QJsonObject{{QStringLiteral("a"), QStringLiteral("izlet_stop")}, {QStringLiteral("yid"), yid}},
                    [&](const QJsonObject&, bool) { app.quit(); });
        } else {
            app.quit();
        }
    });

    postApi(QJsonObject{{QStringLiteral("a"), QStringLiteral("izlet_start")},
                        {QStringLiteral("console"), QStringLiteral("TestPS5")},
                        {QStringLiteral("codec"), codec}},
            [&](const QJsonObject& r, bool ok) {
                if (!ok || r.value(QStringLiteral("yid")).toString().isEmpty()) {
                    std::cerr << "izlet_start basarisiz\n";
                    app.exit(4);
                    return;
                }
                yid = r.value(QStringLiteral("yid")).toString();
                std::cout << "PUB_YID=" << yid.toStdString() << "\n" << std::flush;
                const QJsonArray ice = r.value(QStringLiteral("ice")).toArray();
                const bool started = publisher.start(codec, ice, [&](const QJsonObject& sig) {
                    postApi(QJsonObject{{QStringLiteral("a"), QStringLiteral("izlet_signal")},
                                        {QStringLiteral("yid"), yid},
                                        {QStringLiteral("data"), sig}},
                            nullptr);
                });
                if (!started) {
                    std::cerr << "IzletPublisher::start basarisiz\n";
                    app.exit(5);
                    return;
                }
                pollTimer->start();
                videoTimer->start();
                audioTimer->start();
            });

    return app.exec();
}
