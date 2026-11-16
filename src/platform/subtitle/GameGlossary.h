// Oviora: per-game term lists for the subtitle translation (runtime/altyazi/terim/*.json).
// A file names the PS5/PS4 title ids it belongs to; "genel.json" (no ids) applies to every game.
//   { "game": "...", "titleIds": ["CUSA03041", ...], "keep": ["Arthur Morgan", ...],
//     "terms": { "Dead Eye": "Ölümcül Nişan", ... } }
// keep: names the translator must not touch; terms: fixed Turkish translations. Before a line goes
// to Google / DeepL / MyMemory the matches are swapped for tokens (ZQX1, ZQX2 ...) the translator
// copies as they are; afterwards the tokens get the real words back, with the Turkish case suffix
// the translator attached rebuilt for the real word (vowel harmony, buffer letters, d/t).
#pragma once

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

namespace oviora::subtitle {

struct Glossary
{
    QStringList games;                  // the loaded files' game names, for the log
    QHash<QString, QString> terms;      // English as written in the game -> Turkish
    QStringList keep;                   // names kept as they are
    bool empty() const { return terms.isEmpty() && keep.isEmpty(); }

    // Both lists as match rules, longest first so "Dutch van der Linde" wins over "Dutch"
    struct Entry
    {
        QString from, to;
        QRegularExpression exact, anyCase;
        bool term = false;      // from terms (else a kept name)
        bool protect = false;   // swapped for a token
        bool properName = false;
    };
    QVector<Entry> entries;
};

// genel.json plus every file listing titleId; null when there is nothing to use
std::shared_ptr<const Glossary> loadGlossary(const QString &dir, const QString &titleId);

struct ProtectedText
{
    QString text;                       // what is sent to the translator
    QVector<QString> words;             // token n (1-based) -> the word that comes back
    QVector<bool> properName;           // the word takes its suffix after an apostrophe
    QVector<bool> foreign;              // a non-Turkish name (a final y reads as a vowel)
    bool changed() const { return !words.isEmpty(); }
};

// turkishTarget: terms are only used when translating into Turkish; keep names always
ProtectedText protectTerms(const QString &source, const Glossary &g, bool turkishTarget);
QString restoreTerms(const QString &translated, const ProtectedText &p);

// For the local model's prompt: the fixed translations and kept names found in the line ("" = none)
QString glossaryHint(const QString &source, const Glossary &g, bool turkishTarget);

// The Turkish suffix the translator wrote after a token, rebuilt for word (exposed for the tests)
QString harmonizeSuffix(const QString &word, const QString &suffix, bool foreign);

} // namespace oviora::subtitle
