//
//  UrlPrefixTests.h
//  tests/networking/src
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_UrlPrefixTests_h
#define hifi_UrlPrefixTests_h

#include <QtTest/QtTest>

// ResourceManager's address prefixes: a prefix maps the addresses under it to its replacement, and an address that
// climbs out of it ("../", however spelled) is left as written.
class UrlPrefixTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void prefixes_data();
    void prefixes();
    void prefixWithoutSlash();
    void localNeverRemote();
};

#endif // hifi_UrlPrefixTests_h
