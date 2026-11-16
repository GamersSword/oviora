// Oviora: text rules of the in-game subtitle translation, ported from the old Oviora
// (subtitletranslator.cpp, SUBTITLE_TEXT_HELPERS). OCR also reads licence plates, signs and HUD
// numbers in the region; a read must look like a line of dialogue before it is translated.
#pragma once

#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace oviora::subtitle {

QString normalizeForCompare(const QString &s);
int levenshtein(const QString &a, const QString &b);
// How alike two OCR reads are, 0..1 (letters and digits only)
double textSimilarity(const QString &a, const QString &b);
// OCR jitter between frames of the same subtitle should not trigger a new translation
inline bool textsSimilar(const QString &a, const QString &b) { return textSimilarity(a, b) >= 0.88; }
QString cleanOcrText(const QString &raw);

// Words of an English sentence as written (letters, inner apostrophes)
QStringList sentenceWords(const QString &sentence);
// Extra English words (from the loaded dictionaries)
void setExtraWords(const QSet<QString> &words);

// Why a single OCR row is not part of a subtitle (empty: it may be)
QString rowNoise(const QString &row);
// Why a whole read is not a subtitle line (empty: it is one)
QString textReason(const QString &text);
// Why a read that is shown (textReason passed) is still not clean enough to share with every
// player through the shared translation memory (empty: it is). Stricter than textReason: no
// stray HUD token ("x100", lone letters, symbols), every lower case word in the English word list.
QString submitReason(const QString &text);
// Rows of one subtitle have about the same text height
QVector<bool> heightKeep(const QVector<int> &heights, const QVector<int> &letters);
// Two reads at about the same place (normalized frame rects)
bool samePlace(const QRectF &a, int aLines, const QRectF &b, int bLines);

} // namespace oviora::subtitle
