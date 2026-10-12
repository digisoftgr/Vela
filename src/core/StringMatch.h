#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>

// Returns true if every character of pattern appears in text in order
// (subsequence / fuzzy match). Empty pattern always matches.
//
// Example: "cb" matches "CyberpunkGame" (C...b) but NOT "bc" (no 'c' after 'b').
inline bool subsequenceMatch(const QString &pattern, const QString &text,
                             Qt::CaseSensitivity cs = Qt::CaseInsensitive)
{
    if (pattern.isEmpty())
        return true;

    if (cs == Qt::CaseSensitive) {
        int pi = 0;
        for (int ti = 0; ti < text.size() && pi < pattern.size(); ++ti) {
            if (text[ti] == pattern[pi])
                ++pi;
        }
        return pi == pattern.size();
    }

    // Pre-lowercase the pattern once so the inner loop only calls toLower()
    // on text characters, not on the same pattern characters repeatedly.
    const QString lp = pattern.toLower();
    int pi = 0;
    for (int ti = 0; ti < text.size() && pi < lp.size(); ++ti) {
        if (text[ti].toLower() == lp[pi])
            ++pi;
    }
    return pi == lp.size();
}

// Case-insensitive subsequence match against a pattern the caller has already
// lowercased. For per-row loops (the filter proxy): subsequenceMatch() lowers
// the pattern on every call, which is one allocation per row.
inline bool subsequenceMatchLowered(const QString &lowerPattern, const QString &text)
{
    int pi = 0;
    for (int ti = 0; ti < text.size() && pi < lowerPattern.size(); ++ti) {
        if (text[ti].toLower() == lowerPattern[pi])
            ++pi;
    }
    return pi == lowerPattern.size();
}

// Returns the character positions in text that were matched by pattern.
// Returns an empty list if the pattern does not match at all.
// Useful for highlighting the matched characters in the UI.
inline QList<int> matchedPositions(const QString &pattern, const QString &text,
                                   Qt::CaseSensitivity cs = Qt::CaseInsensitive)
{
    if (pattern.isEmpty())
        return {};

    QList<int> positions;
    positions.reserve(pattern.size());

    if (cs == Qt::CaseSensitive) {
        int pi = 0;
        for (int ti = 0; ti < text.size() && pi < pattern.size(); ++ti) {
            if (text[ti] == pattern[pi]) {
                positions.append(ti);
                ++pi;
            }
        }
        if (pi != pattern.size())
            return {};
        return positions;
    }

    const QString lp = pattern.toLower();
    int pi = 0;
    for (int ti = 0; ti < text.size() && pi < lp.size(); ++ti) {
        if (text[ti].toLower() == lp[pi]) {
            positions.append(ti);
            ++pi;
        }
    }
    if (pi != lp.size())
        return {};
    return positions;
}

// Same rule the filter proxy and the search engine use to decide a pattern is
// a glob rather than a subsequence query.
inline bool isGlobPattern(const QString &pattern)
{
    return pattern.contains(QLatin1Char('*')) || pattern.contains(QLatin1Char('?'));
}

// Regex for highlighting a glob match: accepts what
// QRegularExpression::wildcardToRegularExpression(Unanchored) accepts, but
// every literal run and every [...] class is its own capture group, so the
// characters the pattern actually named can be told apart from what * and ?
// swallowed. ("*.txt" highlights ".txt", not the whole name.)
inline QRegularExpression globHighlightRegex(const QString &pattern,
                                             Qt::CaseSensitivity cs = Qt::CaseInsensitive)
{
    QString re;
    QString literal;
    const auto flushLiteral = [&]() {
        if (!literal.isEmpty()) {
            re += QLatin1Char('(') + QRegularExpression::escape(literal) + QLatin1Char(')');
            literal.clear();
        }
    };

    for (int i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('*')) {
            flushLiteral();
            re += QLatin1String(".*");
        } else if (c == QLatin1Char('?')) {
            flushLiteral();
            re += QLatin1Char('.');
        } else if (c == QLatin1Char('[')) {
            // A ']' right after '[' or '[!' is a member, not the terminator.
            int j = i + 1;
            if (j < pattern.size() && pattern.at(j) == QLatin1Char('!'))
                ++j;
            if (j < pattern.size() && pattern.at(j) == QLatin1Char(']'))
                ++j;
            while (j < pattern.size() && pattern.at(j) != QLatin1Char(']'))
                ++j;
            if (j >= pattern.size()) { // unterminated: a literal '['
                literal += c;
                continue;
            }
            flushLiteral();
            QString cls;
            int k = i + 1;
            if (pattern.at(k) == QLatin1Char('!')) {
                cls += QLatin1Char('^');
                ++k;
            }
            for (; k < j; ++k) {
                const QChar m = pattern.at(k);
                if (m == QLatin1Char('\\') || m == QLatin1Char('[') || m == QLatin1Char(']')
                    || (m == QLatin1Char('^') && cls.isEmpty()))
                    cls += QLatin1Char('\\');
                cls += m;
            }
            re += QLatin1String("([") + cls + QLatin1String("])");
            i = j;
        } else {
            literal += c;
        }
    }
    flushLiteral();

    return QRegularExpression(re, cs == Qt::CaseSensitive
                                      ? QRegularExpression::NoPatternOption
                                      : QRegularExpression::CaseInsensitiveOption);
}

// Positions in `text` covered by the capture groups of a globHighlightRegex().
// Empty when the text does not match.
inline QList<int> globMatchedPositions(const QRegularExpression &re, const QString &text)
{
    QList<int> positions;
    if (!re.isValid())
        return positions;
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return positions;
    for (int g = 1; g <= m.lastCapturedIndex(); ++g) {
        const qsizetype start = m.capturedStart(g);
        if (start < 0)
            continue;
        for (qsizetype p = start; p < m.capturedEnd(g); ++p)
            positions.append(int(p));
    }
    return positions;
}
