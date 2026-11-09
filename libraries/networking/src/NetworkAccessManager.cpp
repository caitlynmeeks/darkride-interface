//
//  NetworkAccessManager.cpp
//  libraries/networking/src
//
//  Created by Clement on 7/1/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "NetworkAccessManager.h"

#include <QHash>
#include <QMutex>
#include <QThreadStorage>
#include <QtNetwork/QNetworkDiskCache>
#include <QtNetwork/QNetworkProxy>

#include "AtpReply.h"
#include "ResourceManager.h"

QThreadStorage<QNetworkAccessManager*> networkAccessManagers;

QNetworkAccessManager& NetworkAccessManager::getInstance() {
    if (!networkAccessManagers.hasLocalData()) {
        QNetworkAccessManager* networkAccessManager = new QNetworkAccessManager();
        // a request follows a redirect only when it says so (RedirectPolicyAttribute), as Qt 5 does by default: Qt 6's
        // default is to follow them (NoLessSafeRedirectPolicy), which would change what signed-in requests do
        networkAccessManager->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
        auto resourceManager = DependencyManager::get<ResourceManager>();
        if (resourceManager) {
            // this manager's own lock in front of the one disk cache that every thread's manager shares (the manager
            // owns the LockedNetworkCache; handing it the disk cache itself let several threads into that at once)
            networkAccessManager->setCache(new LockedNetworkCache(resourceManager->networkDiskCache));
        }
        networkAccessManagers.setLocalData(networkAccessManager);
    }

    return *networkAccessManagers.localData();
}

// one lock for the disk cache: a process has one (ResourceManager's, or AssetClient's own where there is no
// ResourceManager), so one mutex serves every LockedNetworkCache
static QMutex& diskCacheMutex() {
    static QMutex mutex;
    return mutex;
}

// the writes in progress (prepare() done, insert() not yet), by address: which LockedNetworkCache and which device;
// only under diskCacheMutex
struct WriteInProgress {
    const LockedNetworkCache* cache;
    QIODevice* device;
};
static QHash<QUrl, WriteInProgress>& writesInProgress() {
    static QHash<QUrl, WriteInProgress> writes;
    return writes;
}

LockedNetworkCache::LockedNetworkCache(QNetworkDiskCache* diskCache, QObject* parent) :
    QAbstractNetworkCache(parent),
    _diskCache(diskCache)
{
}

LockedNetworkCache::~LockedNetworkCache() {
    QMutexLocker locker(&diskCacheMutex());
    auto& writes = writesInProgress();
    for (auto it = writes.begin(); it != writes.end();) {
        it = it->cache == this ? writes.erase(it) : std::next(it);
    }
}

QNetworkCacheMetaData LockedNetworkCache::metaData(const QUrl& url) {
    QMutexLocker locker(&diskCacheMutex());
    return _diskCache->metaData(url);
}

void LockedNetworkCache::updateMetaData(const QNetworkCacheMetaData& metaData) {
    QMutexLocker locker(&diskCacheMutex());
    _diskCache->updateMetaData(metaData);
}

QIODevice* LockedNetworkCache::data(const QUrl& url) {
    // the disk cache reads the item through the one item it keeps open and hands back a copy the caller owns
    QMutexLocker locker(&diskCacheMutex());
    return _diskCache->data(url);
}

bool LockedNetworkCache::remove(const QUrl& url) {
    QMutexLocker locker(&diskCacheMutex());
    auto& writes = writesInProgress();
    auto write = writes.find(url);
    if (write != writes.end()) {
        if (write->cache != this) {
            // another thread is writing this address: the disk cache's remove() would cancel that write and free the
            // device under it; its insert() replaces the stored copy anyway
            return false;
        }
        // our own write in progress: a remove() is how a manager cancels it, and the only write for this address is ours
        writes.erase(write);
    }
    return _diskCache->remove(url);
}

qint64 LockedNetworkCache::cacheSize() const {
    // the first call counts the files on disk and can remove the oldest
    QMutexLocker locker(&diskCacheMutex());
    return _diskCache->cacheSize();
}

QIODevice* LockedNetworkCache::prepare(const QNetworkCacheMetaData& metaData) {
    // the device belongs to the disk cache until it is handed back to insert() or remove(); only its caller writes to it,
    // outside the lock. One write per address at a time: a second one (another thread loading the same address) just
    // isn't cached, as when the disk cache declines an item
    QMutexLocker locker(&diskCacheMutex());
    auto& writes = writesInProgress();
    if (writes.contains(metaData.url())) {
        return nullptr;
    }
    QIODevice* device = _diskCache->prepare(metaData);
    if (device) {
        writes.insert(metaData.url(), { this, device });
    }
    return device;
}

void LockedNetworkCache::insert(QIODevice* device) {
    // can remove the oldest items when the cache is past its size
    QMutexLocker locker(&diskCacheMutex());
    auto& writes = writesInProgress();
    for (auto it = writes.begin(); it != writes.end(); ++it) {
        if (it->device == device) {
            writes.erase(it);
            break;
        }
    }
    _diskCache->insert(device);
}

void LockedNetworkCache::clear() {
    QMutexLocker locker(&diskCacheMutex());
    _diskCache->clear();
}

QString LockedNetworkCache::cacheDirectory() const {
    QMutexLocker locker(&diskCacheMutex());
    return _diskCache->cacheDirectory();
}

void LockedNetworkCache::setCacheDirectory(const QString& cacheDirectory) {
    QMutexLocker locker(&diskCacheMutex());
    _diskCache->setCacheDirectory(cacheDirectory);
}

qint64 LockedNetworkCache::maximumCacheSize() const {
    QMutexLocker locker(&diskCacheMutex());
    return _diskCache->maximumCacheSize();
}

void LockedNetworkCache::setMaximumCacheSize(qint64 size) {
    QMutexLocker locker(&diskCacheMutex());
    _diskCache->setMaximumCacheSize(size);
}

QNetworkReply* NetworkAccessManager::createRequest(Operation operation, const QNetworkRequest& request, QIODevice* device) {
    if (request.url().scheme() == "atp" && operation == GetOperation) {
        return new AtpReply(request.url());
        //auto url = request.url().toString();
        //return QNetworkAccessManager::createRequest(operation, request, device);
    } else {
        return QNetworkAccessManager::createRequest(operation, request, device);
    }
}
