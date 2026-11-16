// Oviora: per-game term lists for the subtitle translation, see GameGlossary.h.
#include "GameGlossary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

namespace oviora::subtitle {

namespace {

const QString kToken = QStringLiteral("ZQX");

// Turkish lower case: a capital I is a dotless ı in Turkish words, an i in foreign names
QChar trLower(QChar c, bool foreign)
{
    if (c == QChar(0x0130))
        return QLatin1Char('i');
    if (c == QLatin1Char('I'))
        return foreign ? QLatin1Char('i') : QChar(0x0131);
    return c.toLower();
}

QChar plainVowel(QChar c)
{
    switch (c.unicode()) {
    case 0x00E2: return QLatin1Char('a'); // â
    case 0x00EE: return QLatin1Char('i'); // î
    case 0x00FB: return QLatin1Char('u'); // û
    default: return c;
    }
}

bool isA(QChar c) { return c == QLatin1Char('a') || c == QLatin1Char('e'); }
bool isI(QChar c) { return c == QChar(0x0131) || c == QLatin1Char('i') || c == QLatin1Char('u') || c == QChar(0x00FC); }
bool isVowel(QChar c)
{
    c = plainVowel(c);
    return isA(c) || isI(c) || c == QLatin1Char('o') || c == QChar(0x00F6);
}
bool backVowel(QChar c) { return c == QLatin1Char('a') || c == QChar(0x0131) || c == QLatin1Char('o') || c == QLatin1Char('u'); }
bool roundVowel(QChar c) { return c == QLatin1Char('o') || c == QChar(0x00F6) || c == QLatin1Char('u') || c == QChar(0x00FC); }

// The vowel the suffix follows; a final y of a foreign name reads as i ("Molly")
QChar lastVowel(const QString &word, bool foreign)
{
    // An English -er is said "ır": Lester'ı, Trevor'ı is already back
    if (foreign && word.size() > 3 && word.endsWith(QLatin1String("er"), Qt::CaseInsensitive))
        return QChar(0x0131);
    for (int i = word.size() - 1; i >= 0; i--) {
        const QChar c = trLower(word.at(i), foreign);
        if (isVowel(c))
            return plainVowel(c);
        if (foreign && c == QLatin1Char('y') && i == word.size() - 1 && i > 0)
            return QLatin1Char('i');
    }
    return {};
}

bool endsWithVowel(const QString &word, bool foreign)
{
    const QChar c = trLower(word.back(), foreign);
    if (isVowel(c))
        return true;
    return foreign && c == QLatin1Char('y') && word.size() > 1 && !isVowel(trLower(word.at(word.size() - 2), foreign));
}

int vowelCount(const QString &word)
{
    int n = 0;
    for (const QChar c : word)
        n += isVowel(trLower(c, false)) ? 1 : 0;
    return n;
}

QString capitalized(const QString &s)
{
    if (s.isEmpty())
        return s;
    const QChar c = s.at(0);
    const QChar up = c == QLatin1Char('i') ? QChar(0x0130) : c == QChar(0x0131) ? QLatin1Char('I') : c.toUpper();
    return up + s.mid(1);
}

bool mostlyUpper(const QString &s)
{
    int upper = 0, lower = 0;
    for (const QChar c : s) {
        if (c.isUpper())
            upper++;
        else if (c.isLower())
            lower++;
    }
    return upper > 3 && upper > lower * 2;
}

QRegularExpression wordPattern(const QString &from, bool anyCase)
{
    QRegularExpression::PatternOptions o = QRegularExpression::UseUnicodePropertiesOption;
    if (anyCase)
        o |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(QStringLiteral("(?<![\\p{L}\\p{N}])") + QRegularExpression::escape(from) + QStringLiteral("(?![\\p{L}\\p{N}])"), o);
}

bool startsSentence(const QString &text, int pos)
{
    for (int i = pos - 1; i >= 0; i--) {
        const QChar c = text.at(i);
        if (c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QChar(0x201C) || c == QLatin1Char('-') || c == QChar(0x2014))
            continue;
        return c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?') || c == QChar(0x2026);
    }
    return true;
}

} // namespace

// A compound term already ends in the possessive -(s)I ("Aranma Seviyesi"): a case suffix takes an
// n ("Seviyesini", "Seviyesinde"), a person suffix replaces it ("Seviyeniz"). Changes word/suffix.
static void compoundSuffix(QString &word, QString &suffix, const QString &raw)
{
    if (!word.contains(QLatin1Char(' ')) || word.size() < 3 || !isI(trLower(word.back(), false)))
        return;
    // Person suffixes as the translator wrote them for the token: -(I)m, -(I)mIz, -(I)nIz (+ case)
    static const QRegularExpression person(QStringLiteral("^[y]?[ıiuü]?(m|m[ıiuü]z|n[ıiuü]z)(.*)$"));
    const QRegularExpressionMatch pm = person.match(raw);
    if (pm.hasMatch()) {
        const bool s = word.size() > 2 && word.at(word.size() - 2) == QLatin1Char('s');
        QString stem = word.chopped(s ? 2 : 1);  // Seviyesi -> Seviye, Görevi -> Görev
        word = stem;
        // After a vowel the person suffix starts with its consonant; after a consonant with I
        suffix = harmonizeSuffix(stem, isVowel(trLower(stem.back(), false)) ? pm.captured(1) + pm.captured(2)
                                                                            : QStringLiteral("ı") + pm.captured(1) + pm.captured(2), false);
        return;
    }
    const QChar first = suffix.at(0);
    if (first == QLatin1Char('y') && suffix.size() > 1 && isVowel(suffix.at(1))) {
        suffix[0] = QLatin1Char('n');  // Seviyesiyi -> Seviyesini, Seviyesiye -> Seviyesine (-yle stays)
    } else if (first == QLatin1Char('d') || first == QLatin1Char('t')) {
        suffix = QLatin1Char('n') + QString(suffix).replace(0, 1, QLatin1Char('d'));  // Seviyesinde, Seviyesinden
    }
}

QString harmonizeSuffix(const QString &word, const QString &suffix, bool foreign)
{
    if (suffix.isEmpty() || word.isEmpty() || !word.back().isLetter())
        return suffix;
    const QChar lv = lastVowel(word, foreign);
    if (lv.isNull())
        return suffix;  // "FBI": read letter by letter, the translator's suffix is as good as any
    QString s = suffix.toLower();
    const bool keepKi = s.size() >= 4 && s.endsWith(QLatin1String("ki"));
    const bool endV = endsWithVowel(word, foreign);
    const bool hard = QStringLiteral("fstkçşhp").contains(trLower(word.back(), foreign));

    // The buffer letter the translator chose for the token: y (-yi, -ye), n (-nin), s (-si)
    QChar buffer;
    if (s.size() > 1 && s.at(0) == QLatin1Char('y') && isVowel(s.at(1))) {
        buffer = QLatin1Char('y');
        s.remove(0, 1);
    } else if (s.size() > 2 && s.at(0) == QLatin1Char('n') && isI(s.at(1)) && s.at(2) == QLatin1Char('n')) {
        buffer = QLatin1Char('n');
        s.remove(0, 1);
    } else if (s.size() > 1 && s.at(0) == QLatin1Char('s') && isI(s.at(1)) && (s.size() == 2 || s.at(2) == QLatin1Char('n'))) {
        buffer = QLatin1Char('s');
        s.remove(0, 1);
    }
    if (isVowel(s.at(0))) {
        if (endV) {
            if (buffer.isNull())
                buffer = isI(s.at(0)) && s.size() > 1 && s.at(1) == QLatin1Char('n') && (s.size() == 2 || !isVowel(s.at(2)))
                    ? QLatin1Char('n') : QLatin1Char('y');
            s.prepend(buffer);
        }
    } else if (s.at(0) == QLatin1Char('l') && s.size() > 1 && isA(s.at(1)) && !(s.size() > 2 && s.at(2) == QLatin1Char('r'))) {
        if (endV)
            s.prepend(QLatin1Char('y'));  // -yla / -yle, not the plural -lar
    } else if (s.at(0) == QLatin1Char('d') || s.at(0) == QLatin1Char('t')) {
        s[0] = hard ? QLatin1Char('t') : QLatin1Char('d');
    }
    QChar prev = lv;
    for (int i = 0; i < s.size(); i++) {
        const QChar c = s.at(i);
        if (isA(c)) {
            s[i] = backVowel(prev) ? QLatin1Char('a') : QLatin1Char('e');
        } else if (isI(c)) {
            s[i] = backVowel(prev) ? (roundVowel(prev) ? QLatin1Char('u') : QChar(0x0131))
                                   : (roundVowel(prev) ? QChar(0x00FC) : QLatin1Char('i'));
        } else if (!isVowel(c)) {
            continue;
        }
        prev = s.at(i);
    }
    if (keepKi)
        s.replace(s.size() - 2, 2, QStringLiteral("ki"));  // -daki / -deki: "ki" does not harmonize
    return s;
}

std::shared_ptr<const Glossary> loadGlossary(const QString &dir, const QString &titleId)
{
    auto g = std::make_shared<Glossary>();
    QSet<QString> kept;
    // genel first, so a game's own file overrides its terms
    QFileInfoList files = QDir(dir).entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    std::stable_sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return (a.baseName() == QLatin1String("genel")) > (b.baseName() == QLatin1String("genel"));
    });
    for (const QFileInfo &fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonArray ids = obj.value(QStringLiteral("titleIds")).toArray();
        bool match = ids.isEmpty();
        for (const QJsonValue &id : ids)
            match = match || (!titleId.isEmpty() && id.toString().compare(titleId, Qt::CaseInsensitive) == 0);
        if (!match)
            continue;
        g->games << obj.value(QStringLiteral("game")).toString(fi.baseName());
        const QJsonObject terms = obj.value(QStringLiteral("terms")).toObject();
        for (auto it = terms.constBegin(); it != terms.constEnd(); ++it) {
            const QString from = it.key().trimmed(), to = it.value().toString().trimmed();
            if (from.size() >= 2 && !to.isEmpty())
                g->terms.insert(from, to);
        }
        for (const QJsonValue &v : obj.value(QStringLiteral("keep")).toArray()) {
            const QString name = v.toString().trimmed();
            if (name.size() >= 2 && !kept.contains(name)) {
                kept.insert(name);
                g->keep << name;
            }
        }
    }
    if (g->empty())
        return nullptr;
    for (auto it = g->terms.constBegin(); it != g->terms.constEnd(); ++it) {
        Glossary::Entry e;
        e.from = it.key();
        e.to = it.value();
        e.term = true;
        e.protect = true;
        // Game terms are common nouns in Turkish ("Ekip" -> "Ekibi"), even when the game capitalizes them
        e.properName = false;
        g->entries << e;
    }
    for (const QString &name : std::as_const(g->keep)) {
        if (g->terms.contains(name))
            continue;
        Glossary::Entry e;
        e.from = e.to = name;
        // Every name: the translator also localizes ones that are no English word ("Rhodes" -> "Rodos")
        e.protect = true;
        e.properName = true;
        g->entries << e;
    }
    std::stable_sort(g->entries.begin(), g->entries.end(), [](const Glossary::Entry &a, const Glossary::Entry &b) {
        return a.from.size() > b.from.size();
    });
    for (Glossary::Entry &e : g->entries) {
        // A lower case term also matches at the start of a sentence; names only as written
        e.exact = wordPattern(e.from, e.term && !e.from.at(0).isUpper());
        e.anyCase = wordPattern(e.from, true);
    }
    return g;
}

ProtectedText protectTerms(const QString &source, const Glossary &g, bool turkishTarget)
{
    ProtectedText out;
    out.text = source;
    const bool upper = mostlyUpper(source);  // some games write subtitles in capitals
    for (const Glossary::Entry &e : g.entries) {
        if (!e.protect || (e.term && !turkishTarget) || !out.text.contains(e.from, Qt::CaseInsensitive))
            continue;
        int token = 0;
        int pos = 0;
        QString result;
        auto it = (upper ? e.anyCase : e.exact).globalMatch(out.text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (!token) {
                out.words << e.to;
                out.properName << e.properName;
                out.foreign << !e.term;
                token = int(out.words.size());
            }
            result += out.text.mid(pos, m.capturedStart() - pos) + kToken + QString::number(token);
            pos = int(m.capturedEnd());
        }
        if (token)
            out.text = result + out.text.mid(pos);
    }
    return out;
}

QString restoreTerms(const QString &translated, const ProtectedText &p)
{
    if (!p.changed())
        return translated;
    static const QRegularExpression token(QStringLiteral("ZQX(\\d+)(?:['’`]?([\\p{Ll}]+))?"),
                                          QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    QString out;
    int pos = 0;
    auto it = token.globalMatch(translated);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int n = m.captured(1).toInt();
        out += translated.mid(pos, m.capturedStart() - pos);
        pos = int(m.capturedEnd());
        if (n < 1 || n > p.words.size()) {
            out += m.captured(0);
            continue;
        }
        QString word = p.words.at(n - 1);
        const bool proper = p.properName.at(n - 1), foreign = p.foreign.at(n - 1);
        QString suffix = harmonizeSuffix(word, m.captured(2), foreign);
        if (!proper && !suffix.isEmpty())
            compoundSuffix(word, suffix, m.captured(2).toLower());
        if (!proper && !suffix.isEmpty() && isVowel(suffix.at(0)) && vowelCount(word) > 1 && !word.contains(QLatin1Char(' '))) {
            // Consonant softening of a common noun: kitap -> kitabı, ağaç -> ağacı
            const QChar last = word.back();
            const bool nk = last == QLatin1Char('k') && word.size() > 1 && word.at(word.size() - 2) == QLatin1Char('n');
            if (last == QLatin1Char('p')) word.back() = QLatin1Char('b');
            else if (last == QChar(0x00E7)) word.back() = QLatin1Char('c');
            else if (last == QLatin1Char('t')) word.back() = QLatin1Char('d');
            else if (last == QLatin1Char('k')) word.back() = nk ? QLatin1Char('g') : QChar(0x011F);
        }
        if (!proper && startsSentence(out, int(out.size())))
            word = capitalized(word);
        out += word;
        if (!suffix.isEmpty())
            out += proper ? QLatin1Char('\'') + suffix : suffix;
    }
    out += translated.mid(pos);
    return out;
}

QString glossaryHint(const QString &source, const Glossary &g, bool turkishTarget)
{
    QStringList fixed, names;
    const bool upper = mostlyUpper(source);
    for (const Glossary::Entry &e : g.entries) {
        if (fixed.size() + names.size() >= 15)
            break;
        if ((e.term && !turkishTarget) || !source.contains(e.from, Qt::CaseInsensitive) || !(upper ? e.anyCase : e.exact).match(source).hasMatch())
            continue;
        if (e.term)
            fixed << QStringLiteral("\"%1\" = \"%2\"").arg(e.from, e.to);
        else
            names << e.from;
    }
    QString hint;
    if (!fixed.isEmpty())
        hint += QStringLiteral(" Use these fixed translations: %1.").arg(fixed.join(QStringLiteral("; ")));
    if (!names.isEmpty())
        hint += QStringLiteral(" Keep these names unchanged: %1.").arg(names.join(QStringLiteral(", ")));
    return hint;
}

} // namespace oviora::subtitle
