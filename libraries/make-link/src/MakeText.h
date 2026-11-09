//
//  MakeText.h
//  libraries/make-link/src
//
//  Words as plain text, for Make's panel and its channel to Claude Code: the one rule both walls use (the dock's
//  clean() and the link's channel event; security C2/C5, S1 11-07). By code point, never by UTF-16 unit, so a tag
//  character (a surrogate pair) is seen whole and a cut never splits a pair.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeText_h
#define hifi_MakeText_h

#include <QtCore/QString>

class MakeText {
public:
    /// `text` with line breaks kept, a tab as a space, and these dropped: control characters (C0, DEL, C1), the bidi
    /// overrides, isolates and marks (U+202A-202E, U+2066-2069, U+200E, U+200F, U+061C), the line and paragraph
    /// separators (U+2028, U+2029), zero-width characters (U+200B-200D, U+2060, U+FEFF) and the tag characters
    /// (U+E0000-E007F: invisible, and able to carry hidden text). Then at most `maxCodePoints` code points when that
    /// is more than 0.
    static QString plain(const QString& text, int maxCodePoints = 0);
};

#endif // hifi_MakeText_h
