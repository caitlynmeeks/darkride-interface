//
//  LockedNetworkCacheTests.h
//  tests/networking/src
//
//  Created by servers on 2026-11-09.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_LockedNetworkCacheTests_h
#define hifi_LockedNetworkCacheTests_h

#include <QtTest/QtTest>

class LockedNetworkCacheTests : public QObject {
    Q_OBJECT
private slots:
    void oneWriteInProgressPerAddress();
    void aRemoveNeverCancelsAnotherCachesWrite();
    void manyThreadsShareOneDiskCache();
    void unlockedSharingRaces();
};

#endif // hifi_LockedNetworkCacheTests_h
