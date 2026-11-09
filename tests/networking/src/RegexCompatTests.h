//
//  RegexCompatTests.h
//  tests/networking/src
//
//  regex_compat (RegexCompat.h) against QRegExp itself, for every pattern the servers use: the same matches, positions,
//  lengths and captures on a corpus of tricky texts. Runs on the Qt 5 build, where QRegExp still exists (the Qt 6
//  move).
//
//  Created by servers on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_RegexCompatTests_h
#define hifi_RegexCompatTests_h

#pragma once

#include <QtTest/QtTest>

class RegexCompatTests : public QObject {
    Q_OBJECT
private slots:
    void dollarIsTheEndOnly();
    void exactMatchesAsQRegExp();
    void searchesAsQRegExp();
};

#endif // hifi_RegexCompatTests_h
