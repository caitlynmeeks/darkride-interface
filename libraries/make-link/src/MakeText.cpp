//
//  MakeText.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeText.h"

#include <string>

#include <QtCore/QVector>

QString MakeText::plain(const QString& text, int maxCodePoints) {
    // by code point, not by QChar: a tag character is a surrogate pair (an unpaired surrogate comes as U+FFFD)
    const auto points = text.toUcs4();
    std::u32string out;
    out.reserve(static_cast<size_t>(points.size()));
    bool lastWasSelector = false;   // a variation selector right after another one carries nothing visible
    for (const auto point : points) {
        const char32_t u = static_cast<char32_t>(point);
        const bool selector = u >= 0xfe00 && u <= 0xfe0f;
        if (u == '\n') {
            out += u;
        } else if (u == '\t') {
            out += U' ';
        } else if (u < 0x20 || (u >= 0x7f && u < 0xa0)) {
            continue;   // control characters
        } else if ((u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069) || u == 0x200e || u == 0x200f ||
                   u == 0x061c) {
            continue;   // bidi overrides, isolates and marks
        } else if (u == 0x2028 || u == 0x2029) {
            continue;   // line and paragraph separators
        } else if ((u >= 0x200b && u <= 0x200d) || u == 0x2060 || u == 0xfeff || (u >= 0x2061 && u <= 0x2064) ||
                   (u >= 0x206a && u <= 0x206f) || (u >= 0xfff9 && u <= 0xfffb) || u == 0x180e) {
            continue;   // zero-width and invisible format characters, interlinear annotation, the Mongolian vowel separator
        } else if (u >= 0xe0000 && u <= 0xe007f) {
            continue;   // tag characters: invisible, and can carry hidden text (security)
        } else if (u >= 0xe0100 && u <= 0xe01ef) {
            continue;   // variation selectors supplement: invisible, and can carry hidden text (security)
        } else if (selector && lastWasSelector) {
            continue;   // a run of variation selectors: only the first can mean anything, the rest can carry hidden text
        } else {
            out += u;
        }
        lastWasSelector = selector;
        if (maxCodePoints > 0 && out.size() >= static_cast<size_t>(maxCodePoints)) {
            break;
        }
    }
    return QString::fromUcs4(out.data(), static_cast<int>(out.size()));
}
