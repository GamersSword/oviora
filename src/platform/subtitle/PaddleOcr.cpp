// Oviora: PaddleOCR PP-OCRv5 (detection + recognition) on ONNX Runtime
//
// onnxruntime.dll is loaded at runtime from the model folder by full path, so the build needs only
// the headers and Windows' own (older) System32\onnxruntime.dll is never picked up by accident.
// Pre/post-processing follows PaddleX: det = BGR, ImageNet mean/std, sides rounded to 32,
// DB postprocess (thresh 0.3, box_thresh 0.6, unclip 1.5); rec = BGR, height 48, (x/255-0.5)/0.5,
// CTC greedy decode with ["blank"] + dict + [" "].

#include "PaddleOcr.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(ovioraPaddleOcr, "oviora.paddleocr", QtInfoMsg);

static constexpr float det_thresh = 0.3f;
static constexpr float det_box_thresh = 0.6f;
static constexpr float det_unclip_ratio = 1.5f;
static constexpr int rec_height = 48;
static constexpr int rec_min_width = 320;
static constexpr int rec_max_width = 3200;

struct PaddleOcr::Impl
{
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> det;
    std::unique_ptr<Ort::Session> rec;
    std::string det_in, det_out, rec_in, rec_out;
    QStringList chars;  // CTC classes, [0] = blank
    bool space_checked = false;

    QVector<QRect> detect(const QImage &rgb, double scale);
    PaddleOcr::Line recognize(const QImage &rgb, const QRect &box);

    // Recent recognitions: box (input pixels), its bright-pixel mask, the answer
    struct Recent
    {
        QRect box;
        std::vector<uint8_t> mask;
        PaddleOcr::Line line;
        qint64 at = 0;
    };
    std::vector<Recent> recent;
    static std::vector<uint8_t> brightMask(const QImage &rgb, const QRect &box);
    const Recent *findRecent(const QImage &rgb, const QRect &box, std::vector<uint8_t> *mask) const;
};

PaddleOcr::PaddleOcr() : d(new Impl) {}

PaddleOcr::~PaddleOcr()
{
    // Sessions before the env; the dll itself stays loaded for the process lifetime
    d->det.reset();
    d->rec.reset();
    d->env.reset();
}

bool PaddleOcr::loaded() const
{
    return d->det && d->rec;
}

bool PaddleOcr::load(const QString &dir, QString *error)
{
#ifdef Q_OS_WIN
    static const OrtApi *api = nullptr;
    if (!api) {
        const QString dll = QDir::toNativeSeparators(dir + QStringLiteral("/onnxruntime.dll"));
        HMODULE lib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(dll.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib) {
            *error = QStringLiteral("onnxruntime.dll yüklenemedi: %1").arg(dll);
            return false;
        }
        using GetApiBaseFn = const OrtApiBase *(ORT_API_CALL *)();
        auto get_api_base = reinterpret_cast<GetApiBaseFn>(reinterpret_cast<void *>(GetProcAddress(lib, "OrtGetApiBase")));
        if (!get_api_base) {
            *error = QStringLiteral("onnxruntime.dll içinde OrtGetApiBase yok");
            return false;
        }
        api = get_api_base()->GetApi(ORT_API_VERSION);
        if (!api) {
            *error = QStringLiteral("onnxruntime.dll sürümü uyumsuz (API %1 gerekli)").arg(ORT_API_VERSION);
            return false;
        }
        Ort::InitApi(api);
    }
#else
    *error = QStringLiteral("PaddleOCR yalnızca Windows'ta destekleniyor");
    return false;
#endif

    QFile dict_file(dir + QStringLiteral("/dict.txt"));
    if (!dict_file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("dict.txt bulunamadı");
        return false;
    }
    d->chars = { QStringLiteral("blank") };
    for (QByteArray line : dict_file.readAll().split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.isEmpty())
            d->chars << QString::fromUtf8(line);
    }

    try {
        d->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "oviora-ocr");
        Ort::SessionOptions options;
        // Few threads and no busy-spinning: OCR runs a few times a second next to the stream
        // decoder; spinning worker threads made latency jump between 150 ms and 1.5 s, while
        // 4 non-spinning threads measured a steady 140-310 ms on a 1536x184 strip
        options.SetIntraOpNumThreads(qEnvironmentVariableIntValue("OVIORA_OCR_THREADS") > 0
                                         ? qEnvironmentVariableIntValue("OVIORA_OCR_THREADS") : 4);
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const std::wstring det_path = QDir::toNativeSeparators(dir + QStringLiteral("/det.onnx")).toStdWString();
        const std::wstring rec_path = QDir::toNativeSeparators(dir + QStringLiteral("/rec.onnx")).toStdWString();
        d->det = std::make_unique<Ort::Session>(*d->env, det_path.c_str(), options);
        d->rec = std::make_unique<Ort::Session>(*d->env, rec_path.c_str(), options);

        Ort::AllocatorWithDefaultOptions alloc;
        d->det_in = d->det->GetInputNameAllocated(0, alloc).get();
        d->det_out = d->det->GetOutputNameAllocated(0, alloc).get();
        d->rec_in = d->rec->GetInputNameAllocated(0, alloc).get();
        d->rec_out = d->rec->GetOutputNameAllocated(0, alloc).get();
    } catch (const Ort::Exception &e) {
        d->det.reset();
        d->rec.reset();
        *error = QStringLiteral("OCR modeli yüklenemedi: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
    qCInfo(ovioraPaddleOcr) << "PaddleOCR loaded," << d->chars.size() << "classes";
    return true;
}

QVector<QRect> PaddleOcr::Impl::detect(const QImage &rgb, double scale)
{
    const int W = rgb.width(), H = rgb.height();
    double ratio = std::clamp(scale, 0.3, 1.0);
    if (qMin(W, H) * ratio < 64)
        ratio = 64.0 / qMin(W, H);
    int rw = int(W * ratio), rh = int(H * ratio);
    if (qMax(rw, rh) > 4000) {
        const double s = 4000.0 / qMax(rw, rh);
        rw = int(rw * s);
        rh = int(rh * s);
    }
    rw = qMax(int(std::round(rw / 32.0)) * 32, 32);
    rh = qMax(int(std::round(rh / 32.0)) * 32, 32);

    const QImage img = rgb.scaled(rw, rh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB888);
    const size_t plane = size_t(rw) * rh;
    std::vector<float> input(3 * plane);
    // BGR channel order with ImageNet mean/std listed in that same order
    static const float mean[3] = { 0.485f, 0.456f, 0.406f };
    static const float stdv[3] = { 0.229f, 0.224f, 0.225f };
    for (int y = 0; y < rh; y++) {
        const uchar *p = img.constScanLine(y);
        for (int x = 0; x < rw; x++) {
            const float bgr[3] = { p[3 * x + 2] / 255.f, p[3 * x + 1] / 255.f, p[3 * x] / 255.f };
            for (int c = 0; c < 3; c++)
                input[c * plane + size_t(y) * rw + x] = (bgr[c] - mean[c]) / stdv[c];
        }
    }

    const int64_t shape[4] = { 1, 3, rh, rw };
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape, 4);
    const char *in_names[] = { det_in.c_str() };
    const char *out_names[] = { det_out.c_str() };
    auto outputs = det->Run(Ort::RunOptions{ nullptr }, in_names, &tensor, 1, out_names, 1);
    const std::vector<int64_t> oshape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    if (oshape.size() != 4)
        return {};
    const int ph = int(oshape[2]), pw = int(oshape[3]);
    const float *prob = outputs[0].GetTensorData<float>();

    // Connected components of the probability map; subtitles are horizontal, so an axis-aligned
    // box stands in for PaddleOCR's minimum-area rectangle
    QVector<QRect> boxes;
    std::vector<uint8_t> seen(size_t(pw) * ph, 0);
    std::vector<int> stack;
    for (int start = 0; start < pw * ph; start++) {
        if (seen[start] || prob[start] <= det_thresh)
            continue;
        int minx = pw, miny = ph, maxx = -1, maxy = -1, count = 0;
        double sum = 0;
        stack.clear();
        stack.push_back(start);
        seen[start] = 1;
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            const int cx = c % pw, cy = c / pw;
            minx = qMin(minx, cx);
            maxx = qMax(maxx, cx);
            miny = qMin(miny, cy);
            maxy = qMax(maxy, cy);
            sum += prob[c];
            count++;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= pw || ny >= ph)
                        continue;
                    const int q = ny * pw + nx;
                    if (!seen[q] && prob[q] > det_thresh) {
                        seen[q] = 1;
                        stack.push_back(q);
                    }
                }
            }
        }
        const int bw = maxx - minx + 1, bh = maxy - miny + 1;
        if (qMin(bw, bh) < 3 || sum / count < det_box_thresh)
            continue;
        // Unclip: DB predicts a shrunk text kernel, grow it back by area * ratio / perimeter
        const double dist = double(bw) * bh * det_unclip_ratio / (2.0 * (bw + bh));
        const double ex0 = minx - dist, ey0 = miny - dist, ex1 = maxx + 1 + dist, ey1 = maxy + 1 + dist;
        if (qMin(ex1 - ex0, ey1 - ey0) < 5)
            continue;
        const double sx = double(W) / pw, sy = double(H) / ph;
        QRect r(QPoint(qBound(0, int(ex0 * sx), W - 1), qBound(0, int(ey0 * sy), H - 1)),
                QPoint(qBound(0, int(std::ceil(ex1 * sx)) - 1, W - 1), qBound(0, int(std::ceil(ey1 * sy)) - 1, H - 1)));
        if (r.width() >= 4 && r.height() >= 4)
            boxes << r;
    }
    return boxes;
}

PaddleOcr::Line PaddleOcr::Impl::recognize(const QImage &rgb, const QRect &box)
{
    Line line;
    line.box = box;
    const QImage crop = rgb.copy(box);
    const int cw = qMin(rec_max_width, int(std::ceil(double(rec_height) * crop.width() / crop.height())));
    const int W = qMax(rec_min_width, cw);
    const QImage img = crop.scaled(cw, rec_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB888);

    const size_t plane = size_t(W) * rec_height;
    std::vector<float> input(3 * plane, 0.f);  // right side stays zero-padded
    for (int y = 0; y < rec_height; y++) {
        const uchar *p = img.constScanLine(y);
        for (int x = 0; x < cw; x++) {
            input[0 * plane + size_t(y) * W + x] = (p[3 * x + 2] / 255.f - 0.5f) / 0.5f;
            input[1 * plane + size_t(y) * W + x] = (p[3 * x + 1] / 255.f - 0.5f) / 0.5f;
            input[2 * plane + size_t(y) * W + x] = (p[3 * x + 0] / 255.f - 0.5f) / 0.5f;
        }
    }

    const int64_t shape[4] = { 1, 3, rec_height, W };
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape, 4);
    const char *in_names[] = { rec_in.c_str() };
    const char *out_names[] = { rec_out.c_str() };
    auto outputs = rec->Run(Ort::RunOptions{ nullptr }, in_names, &tensor, 1, out_names, 1);
    const std::vector<int64_t> oshape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    if (oshape.size() != 3)
        return line;
    const int T = int(oshape[1]), C = int(oshape[2]);

    // PaddleX appends a space class after the dictionary; confirm against the real class count
    if (!space_checked) {
        if (C == chars.size() + 1)
            chars << QStringLiteral(" ");
        if (C != chars.size())
            qCWarning(ovioraPaddleOcr) << "rec classes" << C << "!= dict" << chars.size();
        space_checked = true;
    }

    const float *out = outputs[0].GetTensorData<float>();
    int prev = -1;
    double conf_sum = 0;
    int conf_n = 0;
    for (int t = 0; t < T; t++) {
        const float *row = out + size_t(t) * C;
        const int best = int(std::max_element(row, row + C) - row);
        if (best != 0 && best != prev && best < chars.size()) {
            line.text += chars.at(best);
            conf_sum += row[best];
            conf_n++;
        }
        prev = best;
    }
    line.text = line.text.trimmed();
    line.score = conf_n ? float(conf_sum / conf_n) : 0.f;
    return line;
}

// Bright pixels of a box (subtitle letters are bright): 1 per pixel, row by row
std::vector<uint8_t> PaddleOcr::Impl::brightMask(const QImage &rgb, const QRect &box)
{
    const QImage crop = rgb.copy(box).convertToFormat(QImage::Format_RGB888);
    std::vector<uint8_t> mask(size_t(box.width()) * box.height());
    for (int y = 0; y < box.height(); y++) {
        const uchar *p = crop.constScanLine(y);
        uint8_t *m = mask.data() + size_t(y) * box.width();
        for (int x = 0; x < box.width(); x++)
            m[x] = (p[3 * x] * 77 + p[3 * x + 1] * 150 + p[3 * x + 2] * 29) >= 170 * 256;
    }
    return mask;
}

// A recent read of (nearly) the same box whose bright pixels match: the same text. Background
// changes behind a subtitle that light up pixels make it read again, never the other way round.
const PaddleOcr::Impl::Recent *PaddleOcr::Impl::findRecent(const QImage &rgb, const QRect &box, std::vector<uint8_t> *mask) const
{
    *mask = brightMask(rgb, box);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Recent &r : recent) {
        if (now - r.at > 5000 || qAbs(r.box.left() - box.left()) > 2 || qAbs(r.box.top() - box.top()) > 2
            || qAbs(r.box.right() - box.right()) > 2 || qAbs(r.box.bottom() - box.bottom()) > 2)
            continue;
        const QRect common = r.box & box;
        qint64 differ = 0, bright = 0;
        for (int y = common.top(); y <= common.bottom(); y++) {
            const uint8_t *a = mask->data() + size_t(y - box.y()) * box.width() + (common.x() - box.x());
            const uint8_t *b = r.mask.data() + size_t(y - r.box.y()) * r.box.width() + (common.x() - r.box.x());
            for (int x = 0; x < common.width(); x++) {
                differ += a[x] != b[x];
                bright += a[x];
            }
        }
        // Up to 1.5 % of the box may differ (anti-aliased letter edges over a changing picture)
        if (bright > 0 && differ * 1000 <= qint64(common.width()) * common.height() * 15)
            return &r;
    }
    return nullptr;
}

QVector<QRect> PaddleOcr::detect(const QImage &rgb, double scale)
{
    if (!loaded() || rgb.isNull())
        return {};
    try {
        return d->detect(rgb, scale);
    } catch (const Ort::Exception &e) {
        qCWarning(ovioraPaddleOcr) << "OCR detection failed:" << e.what();
        return {};
    }
}

PaddleOcr::Line PaddleOcr::recognize(const QImage &rgb, const QRect &box, bool *reused)
{
    if (reused)
        *reused = false;
    Line line;
    line.box = box;
    if (!loaded() || rgb.isNull() || !rgb.rect().contains(box))
        return line;
    std::vector<uint8_t> mask;
    if (const Impl::Recent *r = d->findRecent(rgb, box, &mask)) {
        line = r->line;
        line.box = box;
        if (reused)
            *reused = true;
        return line;
    }
    try {
        line = d->recognize(rgb, box);
    } catch (const Ort::Exception &e) {
        qCWarning(ovioraPaddleOcr) << "OCR recognition failed:" << e.what();
        return line;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::erase_if(d->recent, [now](const Impl::Recent &r) { return now - r.at > 5000; });
    if (d->recent.size() >= 16)
        d->recent.erase(d->recent.begin());
    d->recent.push_back({ box, std::move(mask), line, now });
    return line;
}

QVector<PaddleOcr::Line> PaddleOcr::run(const QImage &rgb)
{
    QVector<Line> lines;
    for (const QRect &b : detect(rgb)) {
        Line l = recognize(rgb, b);
        if (!l.text.isEmpty())
            lines << l;
    }
    std::sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) {
        return a.box.center().y() < b.box.center().y();
    });
    return lines;
}
