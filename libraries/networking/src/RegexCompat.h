//
//  RegexCompat.h
//  libraries/networking/src
//
//  Created by servers on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  Regular expressions with QRegExp's matching rules, built on QRegularExpression (Qt 6 has no QRegExp; the Qt 6
//  move, team/editor-ui/QT6-PLAN.md). Many of these patterns decide what a server accepts (asset paths and hashes,
//  the domain server's web pages and its session cookie), so a pattern must match exactly what it matched before:
//  - '.' also matches a newline, and \w \d \s \b are Unicode, as in QRegExp;
//  - '$' (outside a character class) is the end of the text only; QRegularExpression's '$' would also match just
//    before a final newline;
//  - exact() is QRegExp::exactMatch: the whole text, nothing before or after.
//  tests/networking RegexCompatTests compares them with QRegExp itself on the Qt 5 build, for every pattern the
//  servers use.
//

#ifndef hifi_RegexCompat_h
#define hifi_RegexCompat_h

#include <QtCore/QRegularExpression>
#include <QtCore/QString>

namespace regex_compat {

/// A QRegExp pattern as QRegularExpression reads it the same way: each '$' outside a character class becomes \z.
inline QString pattern(const QString& qregexpPattern) {
    QString out;
    out.reserve(qregexpPattern.size() + 4);
    bool inClass = false;
    for (int i = 0; i < qregexpPattern.size(); i++) {
        const QChar c = qregexpPattern[i];
        if (c == '\\' && i + 1 < qregexpPattern.size()) {
            out += c;
            out += qregexpPattern[++i];
            continue;
        }
        if (inClass) {
            if (c == ']') {
                inClass = false;
            }
            out += c;
            continue;
        }
        if (c == '[') {
            inClass = true;
            out += c;
            // a ']' right after '[' or '[^' is a literal in both
            if (i + 1 < qregexpPattern.size() && qregexpPattern[i + 1] == '^') {
                out += qregexpPattern[++i];
            }
            if (i + 1 < qregexpPattern.size() && qregexpPattern[i + 1] == ']') {
                out += qregexpPattern[++i];
            }
            continue;
        }
        if (c == '$') {
            out += QStringLiteral("\\z");
            continue;
        }
        out += c;
    }
    return out;
}

inline QRegularExpression::PatternOptions options(Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive) {
    QRegularExpression::PatternOptions result =
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::UseUnicodePropertiesOption;
    if (caseSensitivity == Qt::CaseInsensitive) {
        result |= QRegularExpression::CaseInsensitiveOption;
    }
    return result;
}

/// For QRegExp::indexIn, cap, matchedLength and QString's indexOf/replace with a QRegExp: match() finds the first match.
inline QRegularExpression search(const QString& qregexpPattern, Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive) {
    return QRegularExpression(pattern(qregexpPattern), options(caseSensitivity));
}

/// For QRegExp::exactMatch: match(text).hasMatch() only when the whole text matches.
inline QRegularExpression exact(const QString& qregexpPattern, Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive) {
    return QRegularExpression(QRegularExpression::anchoredPattern(pattern(qregexpPattern)), options(caseSensitivity));
}

inline bool exactMatch(const QString& qregexpPattern, const QString& text,
                       Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive) {
    return exact(qregexpPattern, caseSensitivity).match(text).hasMatch();
}

}

#endif // hifi_RegexCompat_h
