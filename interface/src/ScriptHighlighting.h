//
//  ScriptHighlighting.h
//  interface/src
//
//  Created by Thijs Wenker on 4/15/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ScriptHighlighting_h
#define hifi_ScriptHighlighting_h

#include <QSyntaxHighlighter>
#include <QtCore/QRegularExpression>

/// A pattern searched in a line of script text: where it matches from an offset and how long the match was
/// (QRegExp's indexIn and matchedLength, which Qt 6 no longer has, over a QRegularExpression)
class HighlightPattern {
public:
    HighlightPattern() {}
    // Unicode letters count for \b and the classes, as QRegExp counted them
    explicit HighlightPattern(const QString& pattern) : _regex(pattern, QRegularExpression::UseUnicodePropertiesOption) {}
    int indexIn(const QString& text, int from = 0) {
        const QRegularExpressionMatch match = _regex.match(text, from);
        _matchedLength = match.hasMatch() ? match.capturedLength() : -1;
        return match.hasMatch() ? match.capturedStart() : -1;
    }
    int matchedLength() const { return _matchedLength; }
private:
    QRegularExpression _regex;
    int _matchedLength { -1 };
};

class ScriptHighlighting : public QSyntaxHighlighter {
    Q_OBJECT

public:
    ScriptHighlighting(QTextDocument* parent = NULL);

    enum BlockState {
        BlockStateClean,
        BlockStateInMultiComment
    };

protected:
    void highlightBlock(const QString& text) override;
    void highlightKeywords(const QString& text);
    void formatComments(const QString& text);
    void formatQuotedText(const QString& text);
    void formatNumbers(const QString& text);
    void formatTrueFalse(const QString& text);

private:
    HighlightPattern _alphacharRegex;
    HighlightPattern _keywordRegex;
    HighlightPattern _quotedTextRegex;
    HighlightPattern _multiLineCommentBegin;
    HighlightPattern _multiLineCommentEnd;
    HighlightPattern _numberRegex;
    HighlightPattern _singleLineComment;
    HighlightPattern _truefalseRegex;
};

#endif // hifi_ScriptHighlighting_h
