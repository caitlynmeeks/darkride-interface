//
//  ResourceStopTests.h
//
//  Created by servers on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ResourceStopTests_h
#define hifi_ResourceStopTests_h

#include <QtTest/QtTest>

// A reply can still be delivered after an assignment has destroyed ResourceCacheSharedItems on its way out.
class ResourceStopTests : public QObject {
    Q_OBJECT
private slots:
    void callsAfterSharedItemsAreGone();
    void callsWhileSharedItemsLive();
};

#endif // hifi_ResourceStopTests_h
