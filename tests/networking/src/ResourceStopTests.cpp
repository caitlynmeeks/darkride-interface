//
//  ResourceStopTests.cpp
//
//  Created by servers on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ResourceStopTests.h"

#include <DependencyManager.h>
#include <ResourceCache.h>

QTEST_MAIN(ResourceStopTests)

namespace {

// Reaches the calls a Resource makes, which ResourceCache keeps protected.
class StopTestCache : public ResourceCache {
public:
    using ResourceCache::attemptHighestPriorityRequest;
    using ResourceCache::attemptRequest;
    using ResourceCache::requestCompleted;

protected:
    QSharedPointer<Resource> createResource(const QUrl&) override { return {}; }
    QSharedPointer<Resource> createResourceCopy(const QSharedPointer<Resource>&) override { return {}; }
};

} // namespace

void ResourceStopTests::callsAfterSharedItemsAreGone() {
    QVERIFY(!DependencyManager::isSet<ResourceCacheSharedItems>());

    QSharedPointer<Resource> resource(new Resource(QUrl("http://127.0.0.1/stop-test")));
    StopTestCache::requestCompleted(resource);
    QVERIFY(!StopTestCache::attemptRequest(resource));
    QVERIFY(!StopTestCache::attemptHighestPriorityRequest());
}

void ResourceStopTests::callsWhileSharedItemsLive() {
    DependencyManager::set<ResourceCacheSharedItems>();
    ResourceCache::setRequestLimit(0); // everything queues, so no request goes out

    QSharedPointer<Resource> resource(new Resource(QUrl("http://127.0.0.1/stop-test")));
    QVERIFY(!StopTestCache::attemptRequest(resource));
    QCOMPARE(ResourceCache::getPendingRequestCount(), (uint32_t)1);

    StopTestCache::requestCompleted(resource);
    QCOMPARE(ResourceCache::getPendingRequestCount(), (uint32_t)1);
    QCOMPARE(ResourceCache::getLoadingRequestCount(), (uint32_t)0);

    DependencyManager::destroy<ResourceCacheSharedItems>();
}
