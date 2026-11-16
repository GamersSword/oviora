// Oviora: PaddleOCR PP-OCRv5 (detection + recognition) on ONNX Runtime
#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <memory>

class PaddleOcr
{
public:
    struct Line
    {
        QString text;
        float score = 0;  // mean recognition confidence 0..1
        QRect box;        // in input image pixels
    };

    PaddleOcr();
    ~PaddleOcr();

    // dir holds onnxruntime.dll, det.onnx, rec.onnx and dict.txt
    bool load(const QString &dir, QString *error);
    bool loaded() const;

    // rgb: Format_RGB888. Returns text lines sorted top to bottom.
    QVector<Line> run(const QImage &rgb);
    // The two steps of run() on their own, so a caller can skip reading boxes it would drop anyway
    // scale: the detector's input size against the image (recognition always reads full size)
    QVector<QRect> detect(const QImage &rgb, double scale = 1.0);
    // Recognition of one detected box. The same box holding the same bright (text) pixels as in a
    // recent read gives that read's answer again without running the model (*reused = true).
    Line recognize(const QImage &rgb, const QRect &box, bool *reused = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
