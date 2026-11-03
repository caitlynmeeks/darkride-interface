//
//  GzipTests.h
//  tests/shared/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_GzipTests_h
#define hifi_GzipTests_h

#include <QtCore/QObject>

class GzipTests : public QObject {
    Q_OBJECT
private slots:
    void testRoundTrip();
    void testEmpty();
    void testLimitExactly();
    void testOverLimit();
    void testFarOverLimit();
    void testBrokenStreams();
    void testBadLimits();
};

#endif // hifi_GzipTests_h
