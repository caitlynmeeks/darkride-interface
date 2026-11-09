//
//  FSTMappingTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_FSTMappingTests_h
#define hifi_FSTMappingTests_h

#include <QtTest/QtTest>

// The .fst mapping: repeated keys at the top level and inside headings, what the readers get from it, what it writes
// back, and what the model cache's key streams.
class FSTMappingTests : public QObject {
    Q_OBJECT
private slots:
    void keepsRepeatedKeys();
    void mergesFaceTrackingShapes();
    void splitsLegacyShapes();
    void writesWhatItReads();
    void predictsTypeFromHeadings();
    void readsHeadingsInEveryForm();
    void cacheKeyFollowsHeadings();
};

#endif // hifi_FSTMappingTests_h
