#include "SubtitleText.h"

#include <QFile>
#include <QMutex>
#include <QRegularExpression>
#include <QtMath>

#include <utility>

namespace oviora::subtitle {

QString normalizeForCompare(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber())
            out.append(c.toLower());
    }
    return out;
}

int levenshtein(const QString &a, const QString &b)
{
    const int n = a.size(), m = b.size();
    QVector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; j++)
        prev[j] = j;
    for (int i = 1; i <= n; i++) {
        cur[0] = i;
        for (int j = 1; j <= m; j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = qMin(qMin(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

double textSimilarity(const QString &a, const QString &b)
{
    const QString na = normalizeForCompare(a);
    const QString nb = normalizeForCompare(b);
    if (na == nb)
        return 1.0;
    if (na.isEmpty() || nb.isEmpty())
        return 0.0;
    const int longest = qMax(na.size(), nb.size());
    if (longest > 400)
        return 0.0;
    return 1.0 - double(levenshtein(na, nb)) / longest;
}

QString cleanOcrText(const QString &raw)
{
    QString s = raw;
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    static const QRegularExpression junk(QStringLiteral("[|_~`\\\\{}<>]"));
    s.remove(junk);
    s = s.simplified();
    int letters = 0;
    for (const QChar c : s) {
        if (c.isLetter())
            letters++;
    }
    if (letters < 2)
        return QString();
    return s;
}

// English words: a compact list built in (assets/altyazi/ingilizce.txt) plus the words of the
// loaded dictionaries (runtime/altyazi/sozluk/*.json)
static QMutex &wordsMutex()
{
    static QMutex *m = new QMutex;  // never freed: may be used while the app exits
    return *m;
}
static QSet<QString> &extraWords()
{
    static auto *s = new QSet<QString>;
    return *s;
}
static const QSet<QString> &baseWords()
{
    static const QSet<QString> *words = [] {
        auto *s = new QSet<QString>;
        QFile f(QStringLiteral(":/altyazi/ingilizce.txt"));
        if (f.open(QIODevice::ReadOnly)) {
            const QList<QByteArray> lines = f.readAll().split('\n');
            s->reserve(lines.size());
            for (const QByteArray &line : lines) {
                const QString w = QString::fromUtf8(line).trimmed();
                if (!w.isEmpty())
                    s->insert(w);
            }
        }
        return s;
    }();
    return *words;
}

QStringList sentenceWords(const QString &sentence)
{
    QStringList out;
    QString cur;
    for (int i = 0; i <= sentence.size(); i++) {
        const QChar c = i < sentence.size() ? sentence.at(i) : QChar(' ');
        if (c.isLetter() || (c == QLatin1Char('\'') && !cur.isEmpty() && i + 1 < sentence.size() && sentence.at(i + 1).isLetter())) {
            cur.append(c);
        } else if (!cur.isEmpty()) {
            out << cur;
            cur.clear();
        }
    }
    return out;
}

void setExtraWords(const QSet<QString> &words)
{
    QMutexLocker lock(&wordsMutex());
    extraWords() = words;
}

static bool wordInLists(const QString &w)
{
    if (baseWords().contains(w))
        return true;
    QMutexLocker lock(&wordsMutex());
    return extraWords().contains(w);
}

// w: lower case letters (maybe with ' and -)
static bool knownWord(const QString &w)
{
    if (w.isEmpty())
        return false;
    if (w.contains(QLatin1Char('-'))) {
        const QStringList parts = w.split(QLatin1Char('-'), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            return false;
        for (const QString &part : parts) {
            if (!knownWord(part))
                return false;
        }
        return true;
    }
    if (wordInLists(w))
        return true;
    // Simple endings the list may lack
    if (w.endsWith(QLatin1String("'s")) && wordInLists(w.chopped(2)))
        return true;
    if (w.size() > 3 && w.endsWith(QLatin1Char('s')) && wordInLists(w.chopped(1)))
        return true;
    if (w.size() > 4 && w.endsWith(QLatin1String("ed")) && (wordInLists(w.chopped(2)) || wordInLists(w.chopped(1))))
        return true;
    if (w.size() > 5 && w.endsWith(QLatin1String("ing")) && (wordInLists(w.chopped(3)) || wordInLists(w.chopped(3) + QLatin1Char('e'))))
        return true;
    if (w.size() > 4 && w.endsWith(QLatin1String("ly")) && wordInLists(w.chopped(2)))
        return true;
    return false;
}

// One-word lines that are real dialogue ("No!", "Okay.", "Thanks.")
static bool shortReply(const QString &w)
{
    static const QSet<QString> *replies = new QSet<QString> {
        "yes", "no", "ok", "okay", "hey", "hi", "hello", "bye", "goodbye", "what", "why", "how", "who", "where",
        "when", "wait", "stop", "run", "go", "help", "thanks", "sorry", "sure", "fine", "right", "really",
        "please", "come", "look", "listen", "move", "hurry", "later", "now", "here", "there", "shit", "damn",
        "fuck", "man", "dude", "huh", "oh", "ah", "uh", "um", "hmm", "wow", "whoa", "yeah", "yep", "nope",
        "sir", "maybe", "never", "nothing", "again", "great", "good", "nice", "cool", "jesus", "god",
        "alright", "agreed", "exactly", "indeed", "careful", "quiet", "silence", "enough", "seriously",
    };
    return replies->contains(w);
}

namespace {
struct Token
{
    QString text;    // without the punctuation around it
    int letters = 0;
    int digits = 0;
    bool upper = false;   // has upper case letters and no lower case ones
    bool name = false;    // "Lamar": capital first, then lower case
    bool numeric = false; // digits (with an ordinal or unit ending: 1st, 90s, 5pm, 10k, 2x)
    bool mixed = false;   // letters and digits otherwise: plates, codes ("LS-4521", "AK47")
};
}

static QVector<Token> tokens(const QString &text)
{
    static const QRegularExpression numericWord(
        QStringLiteral("^\\d+([.,:/]\\d+)*(st|nd|rd|th|s|am|pm|k|m|x|h)?$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression space(QStringLiteral("\\s+"));
    QVector<Token> out;
    for (const QString &raw : text.split(space, Qt::SkipEmptyParts)) {
        int a = 0, b = raw.size();
        while (a < b && !raw.at(a).isLetterOrNumber())
            a++;
        while (b > a && !raw.at(b - 1).isLetterOrNumber())
            b--;
        if (a >= b)
            continue;
        Token t;
        t.text = raw.mid(a, b - a);
        int lower = 0, upper = 0;
        for (const QChar c : t.text) {
            if (c.isLetter()) {
                t.letters++;
                if (c.isLower())
                    lower++;
                else if (c.isUpper())
                    upper++;
            } else if (c.isDigit()) {
                t.digits++;
            }
        }
        t.upper = upper > 0 && lower == 0;
        t.name = t.letters >= 2 && t.digits == 0 && t.text.at(0).isUpper() && upper == 1 && lower == t.letters - 1;
        if (t.digits > 0)
            (numericWord.match(t.text).hasMatch() ? t.numeric : t.mixed) = true;
        out << t;
    }
    return out;
}

QString rowNoise(const QString &row)
{
    const QVector<Token> list = tokens(row);
    int letters = 0, words = 0, numbers = 0, mixed = 0;
    bool anyLower = false, anyDigit = false;
    for (const Token &t : list) {
        letters += t.letters;
        anyDigit |= t.digits > 0;
        anyLower |= t.letters > 0 && !t.upper;
        if (t.mixed)
            mixed++;
        else if (t.numeric || t.letters == 0)
            numbers++;
        else
            words++;
    }
    if (letters == 0)
        return QStringLiteral("sadece rakam/simge");
    // "34 ABC 123", "LS-4521", "AB12 CDE", "AMMO 30/120"
    if (anyDigit && !anyLower && (mixed > 0 || words <= 2))
        return QStringLiteral("plaka/kod");
    if (mixed > 0 && words < 3)
        return QStringLiteral("harf-rakam karisik");
    if (mixed + numbers > words)
        return QStringLiteral("rakam agirlikli");
    // "WANTED", "MISSION PASSED", "AMMO", "PRESS X"; a shouted reply ("NO!", "OK.") is dialogue
    if (!anyLower && words <= 2 && letters <= 14) {
        const QString last = row.trimmed().right(1);
        const bool sentenceEnd = last == QLatin1String(".") || last == QLatin1String("!") || last == QLatin1String("?");
        if (!(words == 1 && sentenceEnd && shortReply(list.constLast().text.toLower())))
            return QStringLiteral("kisa buyuk harf (HUD)");
    }
    return QString();
}

QString textReason(const QString &text)
{
    const QString noise = rowNoise(text);
    if (!noise.isEmpty())
        return noise;
    const QVector<Token> list = tokens(text);
    QVector<const Token *> words;
    for (const Token &t : list) {
        if (t.letters > 0 && !t.mixed)
            words << &t;
    }
    if (words.isEmpty())
        return QStringLiteral("kelime yok");
    if (words.size() == 1) {
        const QString w = words.first()->text.toLower();
        if (shortReply(w))
            return QString();
        if (words.first()->letters >= 6 && knownWord(w))
            return QString();
        return QStringLiteral("tek kisa/bilinmeyen kelime");
    }
    // Signs and titles: every word capitalised and no sentence punctuation ("Los Santos Customs")
    {
        bool allCapitalised = true;
        for (const Token *t : words)
            allCapitalised &= t->text.at(0).isUpper();
        static const QRegularExpression punctuation(QStringLiteral("[.,!?;:\"'…-]"));
        if (allCapitalised && words.size() <= 4 && !text.contains(punctuation))
            return QStringLiteral("baslik/tabela");
    }
    // Mostly English words; capitalised unknown words count as names (neither for nor against);
    // single letters prove nothing ("a", "e" are common OCR crumbs), except "I"
    int known = 0, considered = 0;
    for (const Token *t : words) {
        const QString w = t->text.toLower();
        if (t->letters < 2 && w != QLatin1String("i"))
            continue;
        if (knownWord(w)) {
            known++;
            considered++;
        } else if (!t->name) {
            considered++;
        }
    }
    if (known == 0)
        return considered == 0 ? QStringLiteral("sadece ozel ad (tabela?)") : QStringLiteral("Ingilizce kelime yok");
    if (known * 2 < considered)
        return QStringLiteral("Ingilizce kelime az (%1/%2)").arg(known).arg(considered);
    return QString();
}

QString submitReason(const QString &text)
{
    const QString base = textReason(text);
    if (!base.isEmpty())
        return base;
    // Characters a subtitle line does not have: OCR crumbs of icons, HUD frames, the minimap
    static const QRegularExpression odd(QStringLiteral("[^\\p{L}\\p{N}\\s.,!?;:'\"\u2019\u2018\u201C\u201D\u2026()\\-]"));
    const auto oddMatch = odd.match(text);
    if (oddMatch.hasMatch())
        return QStringLiteral("garip isaret '%1'").arg(oddMatch.captured(0));
    const QVector<Token> list = tokens(text);
    int known = 0, considered = 0;
    QStringList unknown;
    for (const Token &t : list) {
        // Letters and digits mixed ("x100", "B2"): a HUD counter or code read with the line
        if (t.mixed)
            return QStringLiteral("HUD parcasi '%1'").arg(t.text);
        if (t.letters == 0)
            continue;
        const QString w = t.text.toLower();
        // A lone letter other than "I" / "a" is an OCR crumb ("e Nice job")
        if (t.letters == 1 && w != QLatin1String("i") && w != QLatin1String("a"))
            return QStringLiteral("tek harf kirintisi '%1'").arg(t.text);
        if (t.letters < 2)
            continue;
        if (knownWord(w)) {
            known++;
            considered++;
        } else if (!t.name && !t.upper) {
            // A lower case word the list lacks: a garbled read ("nanaie")
            considered++;
            unknown << t.text;
        }
    }
    // Long lines may hold one rare word; short ones none
    if (unknown.size() > (considered >= 8 ? 1 : 0))
        return QStringLiteral("sozlukte olmayan kelime '%1'").arg(unknown.join(QLatin1String("', '")));
    if (considered == 0 || known * 4 < considered * 3)
        return QStringLiteral("Ingilizce kelime orani dusuk (%1/%2)").arg(known).arg(considered);
    return QString();
}

QVector<bool> heightKeep(const QVector<int> &heights, const QVector<int> &letters)
{
    QVector<bool> keep(heights.size(), true);
    if (heights.size() < 2)
        return keep;
    int ref = 0;
    for (int i = 1; i < heights.size(); i++) {
        if (letters.value(i) > letters.value(ref))
            ref = i;
    }
    const double h = qMax(1, heights.at(ref));
    for (int i = 0; i < heights.size(); i++) {
        const double ratio = heights.at(i) / h;
        keep[i] = ratio >= 0.67 && ratio <= 1.5;
    }
    return keep;
}

bool samePlace(const QRectF &a, int aLines, const QRectF &b, int bLines)
{
    if (!a.isValid() || !b.isValid())
        return true;  // no position known: judged by the text alone
    const double ha = a.height() / qMax(1, aLines), hb = b.height() / qMax(1, bLines);
    const double ratio = ha > 0 && hb > 0 ? ha / hb : 1.0;
    return qAbs(a.center().x() - b.center().x()) <= 0.08 && qAbs(a.center().y() - b.center().y()) <= 0.06
        && ratio >= 0.67 && ratio <= 1.5;
}

} // namespace oviora::subtitle
