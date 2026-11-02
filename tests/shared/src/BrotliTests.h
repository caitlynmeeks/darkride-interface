//
//  BrotliTests.h
//  tests/shared/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_BrotliTests_h
#define hifi_BrotliTests_h

#include <QtCore/QObject>

class BrotliTests : public QObject {
    Q_OBJECT
private slots:
    void testText();
    void testLimitExactly();
    void testOverLimit();
    void testFarOverLimit();
    void testManyGrowSteps();
    void testLargeWindowRefused();
    void testBrokenStreams();
    void testBadLimits();
};

#endif // hifi_BrotliTests_h
