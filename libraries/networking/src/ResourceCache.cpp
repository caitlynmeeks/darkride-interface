//
//  ResourceCache.cpp
//  libraries/shared/src
//
//  Created by Andrzej Kapolka on 2/27/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ResourceCache.h"

#include <QtCore/QDateTime>
#include <QtCore/QMutex>
#include <QtCore/QSet>
#include "ResourceRequestObserver.h"
#include "ResourceScriptingInterface.h"

#include <cfloat>
#include <cmath>
#include <assert.h>

#include <QThread>
#include <QTimer>

#include <SharedUtil.h>
#include <shared/QtHelpers.h>
#include <Trace.h>
#include <Profile.h>

#include "NetworkAccessManager.h"
#include "NetworkLogging.h"
#include "NodeList.h"

bool ResourceCacheSharedItems::appendRequest(QWeakPointer<Resource> resource) {
    Lock lock(_mutex);
    if ((uint32_t)_loadingRequests.size() < _requestLimit) {
        _loadingRequests.append(resource);
        return true;
    } else {
        _pendingRequests.append(resource);
        return false;
    }
}

void ResourceCacheSharedItems::setRequestLimit(uint32_t limit) {
    Lock lock(_mutex);
    _requestLimit = limit;
}

uint32_t ResourceCacheSharedItems::getRequestLimit() const {
    Lock lock(_mutex);
    return _requestLimit;
}

QList<QSharedPointer<Resource>> ResourceCacheSharedItems::getPendingRequests() const {
    QList<QSharedPointer<Resource>> result;
    Lock lock(_mutex);

    foreach (QWeakPointer<Resource> resource, _pendingRequests) {
        auto locked = resource.lock();
        if (locked) {
            result.append(locked);
        }
    }

    return result;
}

uint32_t ResourceCacheSharedItems::getPendingRequestsCount() const {
    Lock lock(_mutex);
    return _pendingRequests.size();
}

QList<QSharedPointer<Resource>> ResourceCacheSharedItems::getLoadingRequests() const {
    QList<QSharedPointer<Resource>> result;
    Lock lock(_mutex);

    foreach(QWeakPointer<Resource> resource, _loadingRequests) {
        auto locked = resource.lock();
        if (locked) {
            result.append(locked);
        }
    }

    return result;
}

uint32_t ResourceCacheSharedItems::getLoadingRequestsCount() const {
    Lock lock(_mutex);
    return _loadingRequests.size();
}

void ResourceCacheSharedItems::removeRequest(QWeakPointer<Resource> resource) {
    Lock lock(_mutex);

    // resource can only be removed if it still has a ref-count, as
    // QWeakPointer has no operator== implementation for two weak ptrs, so
    // manually loop in case resource has been freed.
    for (int i = 0; i < _loadingRequests.size();) {
        auto request = _loadingRequests.at(i);
        // Clear our resource and any freed resources
        if (!request || request.toStrongRef().data() == resource.data()) {
            _loadingRequests.removeAt(i);
            continue;
        }
        i++;
    }
}

QSharedPointer<Resource> ResourceCacheSharedItems::getHighestPendingRequest() {
    // look for the highest priority pending request
    int highestIndex = -1;
    float highestPriority = -FLT_MAX;
    QSharedPointer<Resource> highestResource;
    Lock lock(_mutex);

    bool currentHighestIsFile = false;

    for (int i = 0; i < _pendingRequests.size();) {
        // Clear any freed resources
        auto resource = _pendingRequests.at(i).lock();
        if (!resource) {
            _pendingRequests.removeAt(i);
            continue;
        }

        // Check load priority
        float priority = resource->getLoadPriority();
        bool isFile = resource->getURL().scheme() == HIFI_URL_SCHEME_FILE;
        if (priority >= highestPriority && (isFile || !currentHighestIsFile)) {
            highestPriority = priority;
            highestIndex = i;
            highestResource = resource;
            currentHighestIsFile = isFile;
        }
        i++;
    }

    if (highestIndex >= 0) {
        _pendingRequests.takeAt(highestIndex);
    }

    return highestResource;
}

void ResourceCacheSharedItems::clear() {
    Lock lock(_mutex);
    _pendingRequests.clear();
    _loadingRequests.clear();
}

ScriptableResourceCache::ScriptableResourceCache(QSharedPointer<ResourceCache> resourceCache) {
    _resourceCache = resourceCache;
    connect(&(*_resourceCache), &ResourceCache::dirty,
        this, &ScriptableResourceCache::dirty, Qt::DirectConnection);
}

QVariantList ScriptableResourceCache::getResourceList() {
    return _resourceCache->getResourceList();
}

void ScriptableResourceCache::updateTotalSize(const qint64& deltaSize) {
    _resourceCache->updateTotalSize(deltaSize);
}

ScriptableResource* ScriptableResourceCache::prefetch(const QUrl& url, void* extra, size_t extraHash) {
    return _resourceCache->prefetch(url, extra, extraHash);
}


ScriptableResource::ScriptableResource(const QUrl& url) :
    QObject(nullptr),
    _url(url) { }

void ScriptableResource::release() {
    disconnectHelper();
    _resource.reset();
}

bool ScriptableResource::isInScript() const {
    return _resource && _resource->isInScript();
}

void ScriptableResource::setInScript(bool isInScript) {
    if (_resource) {
        _resource->setInScript(isInScript);
    }
}

void ScriptableResource::loadingChanged() {
    setState(LOADING);
}

void ScriptableResource::loadedChanged() {
    setState(LOADED);
}

void ScriptableResource::finished(bool success) {
    disconnectHelper();

    setState(success ? FINISHED : FAILED);
}

void ScriptableResource::disconnectHelper() {
    if (_progressConnection) {
        disconnect(_progressConnection);
    }
    if (_loadingConnection) {
        disconnect(_loadingConnection);
    }
    if (_loadedConnection) {
        disconnect(_loadedConnection);
    }
    if (_finishedConnection) {
        disconnect(_finishedConnection);
    }
}

ScriptableResource* ResourceCache::prefetch(const QUrl& url, void* extra, size_t extraHash) {
    ScriptableResource* result = nullptr;

    if (QThread::currentThread() != thread()) {
        // Must be called in thread to ensure getResource returns a valid pointer
        BLOCKING_INVOKE_METHOD(this, "prefetch",
            Q_RETURN_ARG(ScriptableResource*, result),
            Q_ARG(QUrl, url), Q_ARG(void*, extra), Q_ARG(size_t, extraHash));
        return result;
    }

    result = new ScriptableResource(url);

    auto resource = getResource(url, QUrl(), extra, extraHash);
    result->_resource = resource;
    result->setObjectName(url.toString());

    result->_resource = resource;
    if (resource->isLoaded() || resource->_failedToLoad) {
        result->finished(!resource->_failedToLoad);
    } else {
        result->_progressConnection = connect(
            resource.data(), &Resource::onProgress,
            result, &ScriptableResource::progressChanged);
        result->_loadingConnection = connect(
            resource.data(), &Resource::loading,
            result, &ScriptableResource::loadingChanged);
        result->_loadedConnection = connect(
            resource.data(), &Resource::loaded,
            result, &ScriptableResource::loadedChanged);
        result->_finishedConnection = connect(
            resource.data(), &Resource::finished,
            result, &ScriptableResource::finished);
    }

    return result;
}

ResourceCache::ResourceCache(QObject* parent) : QObject(parent) {
    if (DependencyManager::isSet<NodeList>()) {
        auto nodeList = DependencyManager::get<NodeList>();
        auto& domainHandler = nodeList->getDomainHandler();
        connect(&domainHandler, &DomainHandler::disconnectedFromDomain,
            this, &ResourceCache::clearATPAssets, Qt::DirectConnection);
    }
}

ResourceCache::~ResourceCache() {
    clearUnusedResources();
}

void ResourceCache::clearATPAssets() {
    {
        QWriteLocker locker(&_resourcesLock);
        QList<QUrl> urls = _resources.keys();
        for (auto& url : urls) {
            // If this is an ATP resource
            if (url.scheme() == URL_SCHEME_ATP) {
                auto resourcesWithExtraHash = _resources.take(url);
                for (auto& resource : resourcesWithExtraHash) {
                    if (auto strongRef = resource.lock()) {
                        // Make sure the resource won't reinsert itself
                        strongRef->setCache(nullptr);
                        _totalResourcesSize -= strongRef->getBytes();
                    }
                }
            }
        }
    }
    {
        QWriteLocker locker(&_unusedResourcesLock);
        for (auto& resource : _unusedResources.values()) {
            if (resource->getURL().scheme() == URL_SCHEME_ATP) {
                _unusedResources.remove(resource->getLRUKey());
                _unusedResourcesSize -= resource->getBytes();
            }
        }
    }

    resetResourceCounters();
}

void ResourceCache::refreshAll() {
    // Clear all unused resources so we don't have to reload them
    clearUnusedResources();
    resetUnusedResourceCounter();

    QHash<QUrl, QHash<size_t, QWeakPointer<Resource>>> allResources;
    {
        QReadLocker locker(&_resourcesLock);
        allResources = _resources;
    }

    // Refresh all remaining resources in use
    // FIXME: this will trigger multiple refreshes for the same resource if they have different hashes
    for (auto& resourcesWithExtraHash : allResources) {
        for (auto& resourceWeak : resourcesWithExtraHash) {
            auto resource = resourceWeak.lock();
            if (resource) {
                resource->refresh();
            }
        }
    }
}

QVariantList ResourceCache::getResourceList() {
    QVariantList list;
    if (QThread::currentThread() != thread()) {
        // NOTE: invokeMethod does not allow a const QObject*
        BLOCKING_INVOKE_METHOD(this, "getResourceList",
            Q_RETURN_ARG(QVariantList, list));
    } else {
        QList<QUrl> resources;
        {
            QReadLocker locker(&_resourcesLock);
            resources = _resources.keys();  // a QHash: each key once, as uniqueKeys() gave
        }
        list.reserve(resources.size());
        for (auto& resource : resources) {
            list << resource;
        }
    }

    return list;
}
 
namespace {
QMutex deadHostsMutex;
QHash<QString, qint64> deadHostsUntil;   // host -> when its death expires, ms since the epoch
QSet<QString> answeredHosts;
}

namespace {
    QMutex failuresMutex;
    QVector<ResourceCache::Failure> failureList;   // newest last, one per address
}

QVector<ResourceCache::Failure> ResourceCache::failures() {
    QMutexLocker lock(&failuresMutex);
    return failureList;
}

void ResourceCache::clearFailures() {
    QMutexLocker lock(&failuresMutex);
    failureList.clear();
}

void ResourceCache::recordFailure(const QUrl& url, const QString& resourceType, const QString& reason) {
    if (url.isEmpty()) {
        return;
    }
    // a person's word for the cache's class
    QString type = resourceType == "Model" ? "model"
        : resourceType == "NetworkTexture" ? "texture"
        : resourceType == "NetworkMaterial" ? "material"
        : resourceType == "Animation" ? "animation"
        : resourceType == "NetworkShader" ? "shader"
        : "file";
    Failure failure { url, type, reason, QDateTime::currentMSecsSinceEpoch() };
    {
        QMutexLocker lock(&failuresMutex);
        for (int i = 0; i < failureList.size(); ++i) {
            if (failureList[i].url == url) {
                failureList.remove(i);
                break;
            }
        }
        failureList.push_back(failure);
        if (failureList.size() > MAX_FAILURES) {
            failureList.remove(0, failureList.size() - MAX_FAILURES);
        }
    }
    if (DependencyManager::isSet<ResourceScriptingInterface>()) {
        emit DependencyManager::get<ResourceScriptingInterface>()->resourceFailed(url.toString(), type, reason);
    }
}

void ResourceCache::forgetFailure(const QUrl& url) {
    QMutexLocker lock(&failuresMutex);
    for (int i = 0; i < failureList.size(); ++i) {
        if (failureList[i].url == url) {
            failureList.remove(i);
            return;
        }
    }
}

bool ResourceCache::hasHostAnswered(const QString& host) {
    QMutexLocker lock(&deadHostsMutex);
    return answeredHosts.contains(host);
}

void ResourceCache::markHostAnswered(const QString& host) {
    if (host.isEmpty()) {
        return;
    }
    QMutexLocker lock(&deadHostsMutex);
    answeredHosts.insert(host);
}

bool ResourceCache::isHostDead(const QString& host) {
    if (host.isEmpty()) {
        return false;
    }
    QMutexLocker lock(&deadHostsMutex);
    auto it = deadHostsUntil.find(host);
    return it != deadHostsUntil.end() && QDateTime::currentMSecsSinceEpoch() < it.value();
}

void ResourceCache::markHostDead(const QString& host) {
    if (host.isEmpty()) {
        return;
    }
    QMutexLocker lock(&deadHostsMutex);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!deadHostsUntil.contains(host) || now >= deadHostsUntil.value(host)) {
        qCDebug(networking) << "Host did not resolve or answer; its links fail at once for" << (DEAD_HOST_MSECS / 1000) << "s:" << host;
    }
    deadHostsUntil[host] = now + DEAD_HOST_MSECS;
}

void ResourceCache::setRequestLimit(uint32_t limit) {
    auto sharedItems = DependencyManager::get<ResourceCacheSharedItems>();
    sharedItems->setRequestLimit(limit);

    // Now go fill any new request spots
    while (sharedItems->getLoadingRequestsCount() < limit && sharedItems->getPendingRequestsCount() > 0) {
        attemptHighestPriorityRequest();
    }
}

QSharedPointer<Resource> ResourceCache::getResource(const QUrl& url, const QUrl& fallback, void* extra, size_t extraHash) {
    QSharedPointer<Resource> resource;
    {
        QWriteLocker locker(&_resourcesLock);
        auto& resourcesWithExtraHash = _resources[url];
        auto resourcesWithExtraHashIter = resourcesWithExtraHash.find(extraHash);
        if (resourcesWithExtraHashIter != resourcesWithExtraHash.end()) {
            // We've seen this extra info before
            resource = resourcesWithExtraHashIter.value().lock();
        } else if (resourcesWithExtraHash.size() > 0.0f) {
            auto oldResource = resourcesWithExtraHash.begin().value().lock();
            if (oldResource) {
                // We haven't seen this extra info before, but we've already downloaded the resource.  We need a new copy of this object (with any old hash).
                resource = createResourceCopy(oldResource);
                resource->setExtra(extra);
                resource->setExtraHash(extraHash);
                resource->setSelf(resource);
                resource->setCache(this);
                resource->moveToThread(qApp->thread());
                connect(resource.data(), &Resource::updateSize, this, &ResourceCache::updateTotalSize);
                resourcesWithExtraHash.insert(extraHash, resource);
                resource->ensureLoading();
            }
        }
    }
    if (resource) {
        removeUnusedResource(resource);
    }

    if (!resource && (!url.isValid() || url.isEmpty()) && fallback.isValid()) {
        resource = getResource(fallback, QUrl(), extra, extraHash);
    }

    if (!resource) {
        resource = createResource(url);
        resource->setExtra(extra);
        resource->setExtraHash(extraHash);
        resource->setSelf(resource);
        resource->setCache(this);
        resource->moveToThread(qApp->thread());
        connect(resource.data(), &Resource::updateSize, this, &ResourceCache::updateTotalSize);
        {
            QWriteLocker locker(&_resourcesLock);
            _resources[url].insert(extraHash, resource);
        }
        removeUnusedResource(resource);
        resource->ensureLoading();
    }

    DependencyManager::get<ResourceRequestObserver>()->update(resource->getURL(), -1, "ResourceCache::getResource");
    return resource;
}

void ResourceCache::setUnusedResourceCacheSize(qint64 unusedResourcesMaxSize) {
    _unusedResourcesMaxSize = glm::clamp(unusedResourcesMaxSize, MIN_UNUSED_MAX_SIZE, MAX_UNUSED_MAX_SIZE);
    reserveUnusedResource(0);
    resetUnusedResourceCounter();
}

void ResourceCache::addUnusedResource(const QSharedPointer<Resource>& resource) {
    // If it doesn't fit or its size is unknown, remove it from the cache.
    if (resource->getBytes() == 0 || resource->getBytes() > _unusedResourcesMaxSize) {
        resource->setCache(nullptr);
        removeResource(resource->getURL(), resource->getExtraHash(), resource->getBytes());
        resetTotalResourceCounter();
        return;
    }
    reserveUnusedResource(resource->getBytes());
    
    resource->setLRUKey(++_lastLRUKey);

    {
        QWriteLocker locker(&_unusedResourcesLock);
        _unusedResources.insert(resource->getLRUKey(), resource);
        _unusedResourcesSize += resource->getBytes();
    }

    resetUnusedResourceCounter();
}

void ResourceCache::removeUnusedResource(const QSharedPointer<Resource>& resource) {
    QWriteLocker locker(&_unusedResourcesLock);
    if (_unusedResources.contains(resource->getLRUKey())) {
        _unusedResources.remove(resource->getLRUKey());
        _unusedResourcesSize -= resource->getBytes();

        locker.unlock();
        resetUnusedResourceCounter();
    }
}

void ResourceCache::reserveUnusedResource(qint64 resourceSize) {
    QWriteLocker locker(&_unusedResourcesLock);
    while (!_unusedResources.empty() &&
           _unusedResourcesSize + resourceSize > _unusedResourcesMaxSize) {
        // unload the oldest resource
        QMap<int, QSharedPointer<Resource> >::iterator it = _unusedResources.begin();
        
        it.value()->setCache(nullptr);
        auto size = it.value()->getBytes();

        locker.unlock();
        removeResource(it.value()->getURL(), it.value()->getExtraHash(), size);
        locker.relock();

        _unusedResourcesSize -= size;
        _unusedResources.erase(it);
    }
}

void ResourceCache::clearUnusedResources() {
    // the unused resources may themselves reference resources that will be added to the unused
    // list on destruction, so keep clearing until there are no references left
    QWriteLocker locker(&_unusedResourcesLock);
    while (!_unusedResources.isEmpty()) {
        foreach (const QSharedPointer<Resource>& resource, _unusedResources) {
            resource->setCache(nullptr);
        }
        _unusedResources.clear();
    }
    _unusedResourcesSize = 0;
}

void ResourceCache::resetTotalResourceCounter() {
    {
        QReadLocker locker(&_resourcesLock);
        _numTotalResources = _resources.size();
    }

    emit dirty();
}

void ResourceCache::resetUnusedResourceCounter() {
    {
        QReadLocker locker(&_unusedResourcesLock);
        _numUnusedResources = _unusedResources.size();
    }

    emit dirty();
}

void ResourceCache::resetResourceCounters() {
    resetTotalResourceCounter();
    resetUnusedResourceCounter();

    emit dirty();
}

void ResourceCache::removeResource(const QUrl& url, size_t extraHash, qint64 size) {
    QWriteLocker locker(&_resourcesLock);
    auto& resources = _resources[url];
    resources.remove(extraHash);
    if (resources.size() == 0) {
        _resources.remove(url);
    }
    _totalResourcesSize -= size;
}

void ResourceCache::updateTotalSize(const qint64& deltaSize) {
    _totalResourcesSize += deltaSize;

    // Sanity checks
    assert(_totalResourcesSize >= 0);
    assert(_totalResourcesSize < (1024 * BYTES_PER_GIGABYTES));

    emit dirty();
}

QList<QSharedPointer<Resource>> ResourceCache::getLoadingRequests() {
    return DependencyManager::get<ResourceCacheSharedItems>()->getLoadingRequests();
}

uint32_t ResourceCache::getPendingRequestCount() {
    return DependencyManager::get<ResourceCacheSharedItems>()->getPendingRequestsCount();
}

uint32_t ResourceCache::getLoadingRequestCount() {
    return DependencyManager::get<ResourceCacheSharedItems>()->getLoadingRequestsCount();
}

bool ResourceCache::attemptRequest(QSharedPointer<Resource> resource) {
    Q_ASSERT(!resource.isNull());

    auto sharedItems = DependencyManager::get<ResourceCacheSharedItems>();
    if (!sharedItems) {
        return false; // shutting down, see requestCompleted
    }
    if (sharedItems->appendRequest(resource)) {
        resource->makeRequest();
        return true;
    }
    return false;
}

void ResourceCache::requestCompleted(QWeakPointer<Resource> resource) {
    auto sharedItems = DependencyManager::get<ResourceCacheSharedItems>();
    if (!sharedItems) {
        // An assignment stops on its own thread and destroys these while the main thread keeps delivering events
        // (AssignmentClient::stopAssignmentClient), so a reply already queued can still land here. Holding the shared
        // pointer above makes this check safe; cancelling requests first could not catch a reply already queued.
        return;
    }

    sharedItems->removeRequest(resource);

    // Now go fill any new request spots
    while (sharedItems->getLoadingRequestsCount() < sharedItems->getRequestLimit() && sharedItems->getPendingRequestsCount() > 0) {
        attemptHighestPriorityRequest();
    }
}

bool ResourceCache::attemptHighestPriorityRequest() {
    auto sharedItems = DependencyManager::get<ResourceCacheSharedItems>();
    if (!sharedItems) {
        return false; // shutting down, see requestCompleted
    }
    auto resource = sharedItems->getHighestPendingRequest();
    return (resource && attemptRequest(resource));
}

static int requestID = 0;

Resource::Resource(const Resource& other) :
    QObject(),
    _url(other._url),
    _effectiveBaseURL(other._effectiveBaseURL),
    _activeUrl(other._activeUrl),
    _requestByteRange(other._requestByteRange),
    _shouldFailOnRedirect(other._shouldFailOnRedirect),
    _startedLoading(other._startedLoading),
    _failedToLoad(other._failedToLoad),
    _loaded(other._loaded),
    _loadPriorities(other._loadPriorities),
    _bytesReceived(other._bytesReceived),
    _bytesTotal(other._bytesTotal),
    _bytes(other._bytes),
    _requestID(++requestID),
    _extraHash(other._extraHash) {
    if (!other._loaded) {
        _startedLoading = false;
    }
}

Resource::Resource(const QUrl& url) :
    _url(url),
    _effectiveBaseURL(url),
    _activeUrl(url),
    _requestID(++requestID) {
    init();
}

Resource::~Resource() {
    if (_request) {
        _request->disconnect(this);
        _request->deleteLater();
        _request = nullptr;
        ResourceCache::requestCompleted(_self);
    }
}

void Resource::ensureLoading() {
    if (!_startedLoading) {
        attemptRequest();
    }
}

void Resource::setLoadPriority(const QPointer<QObject>& owner, float priority) {
    if (!_failedToLoad) {
        _loadPriorities.insert(owner, priority);
    }
}

void Resource::setLoadPriorities(const QHash<QPointer<QObject>, float>& priorities) {
    if (_failedToLoad) {
        return;
    }
    for (QHash<QPointer<QObject>, float>::const_iterator it = priorities.constBegin();
            it != priorities.constEnd(); it++) {
        _loadPriorities.insert(it.key(), it.value());
    }
}

void Resource::clearLoadPriority(const QPointer<QObject>& owner) {
    if (!_failedToLoad) {
        _loadPriorities.remove(owner);
    }
}

float Resource::getLoadPriority() {
    if (_loadPriorities.size() == 0) {
        return 0;
    }

    float highestPriority = -FLT_MAX;
    for (QHash<QPointer<QObject>, float>::iterator it = _loadPriorities.begin(); it != _loadPriorities.end(); ) {
        if (it.key().isNull()) {
            it = _loadPriorities.erase(it);
            continue;
        }
        highestPriority = qMax(highestPriority, it.value());
        it++;
    }
    return highestPriority;
}

void Resource::refresh() {
    if (_request && !(_loaded || _failedToLoad)) {
        return;
    }

    if (_request) {
        _request->disconnect(this);
        _request->deleteLater();
        _request = nullptr;
        ResourceCache::requestCompleted(_self);
    }
    
    _activeUrl = _url;
    init();
    ensureLoading();
    emit onRefresh();
}

void Resource::allReferencesCleared() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "allReferencesCleared");
        return;
    }

    if (_cache && isCacheable()) {
        // create and reinsert new shared pointer 
        QSharedPointer<Resource> self(this, &Resource::deleter);
        setSelf(self);
        reinsert();

        // add to the unused list
        _cache->addUnusedResource(self);
    } else {
        if (_cache) {
            // remove from the cache
            _cache->removeResource(getURL(), getExtraHash(), getBytes());
            _cache->resetTotalResourceCounter();
        }

        deleteLater();
    }
}

void Resource::init(bool resetLoaded) {
    _startedLoading = false;
    _failedToLoad = false;
    _failureReason.clear();
    if (resetLoaded) {
        _loaded = false;
    }
    _attempts = 0;

    if (_url.isEmpty()) {
        _startedLoading = _loaded = true;

    } else if (!(_url.isValid())) {
        _startedLoading = _failedToLoad = true;
        _failureReason = "not a valid address";
        ResourceCache::recordFailure(_url, getType(), _failureReason);
    }
}

void Resource::attemptRequest() {
    _startedLoading = true;

    if (_attempts > 0) {
        qCDebug(networking).noquote() << "Server unavailable "
            << "- retrying asset load - attempt" << _attempts << " of " << MAX_ATTEMPTS;
    }

    auto self = _self.lock();
    if (self) {
        ResourceCache::attemptRequest(self);
    }
}

void Resource::finishedLoading(bool success) {
    if (success) {
        _loadPriorities.clear();
        _loaded = true;
        ResourceCache::forgetFailure(_url);   // it loads now (a retry, or the link was fixed)
    } else {
        _failedToLoad = true;
        if (_failureReason.isEmpty()) {
            // it arrived, but the client could not make anything of it (a model or picture in a format it cannot read,
            // a damaged file)
            _failureReason = "could not be read";
        }
        ResourceCache::recordFailure(_url, getType(), _failureReason);
    }
    emit finished(success);
}

void Resource::setSize(const qint64& bytes) {
    emit updateSize(bytes - _bytes);
    _bytes = bytes;
}

void Resource::reinsert() {
    QWriteLocker locker(&_cache->_resourcesLock);
    _cache->_resources[_url].insert(_extraHash, _self);
}


void Resource::makeRequest() {
    // a host that did not resolve a moment ago is dead for a while: no request, and the failure is final. It is
    // delivered from the event loop, as a request's failure would be, since a caller may still be setting the
    // resource up when ensureLoading() brings it here.
    if (ResourceCache::isHostDead(_activeUrl.host())) {
        qCDebug(networking) << "Not loading, the host is dead for now:" << _activeUrl.toString();
        QTimer::singleShot(0, this, [this] {
            ResourceCache::requestCompleted(_self);
            _attemptsRemaining = 0;
            emit failed(QNetworkReply::HostNotFoundError);
            finishedLoading(false);
        });
        return;
    }

    if (_request) {
        PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID));
        _request->disconnect();
        _request->deleteLater();
    }

    PROFILE_ASYNC_BEGIN(resource, "Resource:" + getType(), QString::number(_requestID), { { "url", _url.toString() }, { "activeURL", _activeUrl.toString() } });

    _request = DependencyManager::get<ResourceManager>()->createResourceRequest(
        this, _activeUrl, true, -1, "Resource::makeRequest");

    if (!_request) {
        ResourceCache::requestCompleted(_self);
        finishedLoading(false);
        PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID));
        return;
    }

    _request->setByteRange(_requestByteRange);
    _request->setFailOnRedirect(_shouldFailOnRedirect);

    emit loading();

    connect(_request, &ResourceRequest::progress, this, &Resource::onProgress);
    connect(this, &Resource::onProgress, this, &Resource::handleDownloadProgress);

    connect(_request, &ResourceRequest::finished, this, &Resource::handleReplyFinished);

    _bytesReceived = _bytesTotal = _bytes = 0;

    _request->send();
}

void Resource::handleDownloadProgress(uint64_t bytesReceived, uint64_t bytesTotal) {
    _bytesReceived = bytesReceived;
    _bytesTotal = bytesTotal;
}

void Resource::handleReplyFinished() {
    if (!DependencyManager::isSet<ResourceCacheSharedItems>()) {
        // Shutting down (see ResourceCache::requestCompleted): what a reader needs to parse this may be gone too
        if (_request) {
            _request->disconnect(this);
            _request->deleteLater();
            _request = nullptr;
        }
        return;
    }
    if (!_request || _request != sender()) {
        // This can happen in the edge case that a request is timed out, but a `finished` signal is emitted before it is deleted.
        qWarning(networking) << "Received signal Resource::handleReplyFinished from ResourceRequest that is not the current"
            << " request: " << sender() << ", " << _request;
        PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID), {
            { "from_cache", false },
            { "size_mb", _bytesTotal / 1000000.0 }
            });
        ResourceCache::requestCompleted(_self);
        return;
    }

    PROFILE_ASYNC_END(resource, "Resource:" + getType(), QString::number(_requestID), {
        { "from_cache", _request->loadedFromCache() },
        { "size_mb", _bytesTotal / 1000000.0 }
    });

    setSize(_bytesTotal);

    // Make sure we keep the Resource alive here
    auto self = _self.lock();
    ResourceCache::requestCompleted(_self);

    auto result = _request->getResult();
    if (result == ResourceRequest::Success) {

        auto relativePathURL = _request->getRelativePathUrl();
        if (!relativePathURL.isEmpty()) {
            _effectiveBaseURL = relativePathURL;
        }

        auto data = _request->getData();
        emit loaded(data);
        downloadFinished(data);
    } else {
        handleFailedRequest(result);
    }
    
    _request->disconnect(this);
    _request->deleteLater();
    _request = nullptr;
}

bool Resource::handleFailedRequest(ResourceRequest::Result result) {
    bool willRetry = false;
    if (result == ResourceRequest::Result::Timeout && _bytesReceived == 0 && !ResourceCache::hasHostAnswered(_activeUrl.host())) {
        // ten seconds with nothing from a host that has not answered once this run: an address where nothing listens
        // (Portalarium names three links on one), so it is dead for a while like a name that does not resolve,
        // instead of eight ten-second attempts per link on the retry ladder. A host that has answered is never
        // declared dead by a timeout: a slow or busy link is not a dead host.
        qCDebug(networking) << "Timed out with nothing received from a host that never answered:" << _activeUrl.toString();
        result = ResourceRequest::Result::HostNotFound;
    }
    switch (result) {
        case ResourceRequest::Result::Timeout: {
            qCDebug(networking) << "Timed out loading: received " << _bytesReceived << " total " << _bytesTotal;
            // Fall through to other cases
        }
        // FALLTHRU
        case ResourceRequest::Result::ServerUnavailable: {
            _attempts++;
            _attemptsRemaining--;

            qCDebug(networking) << "Retryable error while loading: attempt:" << _attempts << "attemptsRemaining:" << _attemptsRemaining;

            // retry with increasing delays
            const int BASE_DELAY_MS = 1000;
            if (_attempts < MAX_ATTEMPTS) {
                auto waitTime = BASE_DELAY_MS * (int)pow(2.0, _attempts);
                qCDebug(networking).noquote() << "Server unavailable for - may retry in" << waitTime << "ms"
                    << "if resource is still needed";
                QTimer::singleShot(waitTime, this, &Resource::attemptRequest);
                willRetry = true;
                break;
            }
            // fall through to final failure
        }
        // FALLTHRU
        case ResourceRequest::Result::HostNotFound: {
            // the name did not resolve: every link to that host would fail the same way, so the host is dead for a
            // while (DEAD_HOST_MSECS) and this failure is final: the ladder of retries was what held old worlds'
            // dead links in the loading state for minutes (ARCHITECTURE.md 2.3)
            ResourceCache::markHostDead(_activeUrl.host());
        }
        // FALLTHRU
        default: {
            _attemptsRemaining = 0;
            QMetaEnum metaEnum = QMetaEnum::fromType<ResourceRequest::Result>();
            qCDebug(networking) << "Error loading:" << metaEnum.valueToKey(result) << "resource:" << _url.toString();
            // the reason, for build mode: what went wrong, with the server's own status when it answered with one
            // (some texture paths fail here after their request is gone)
            QString what;
            switch (result) {
                case ResourceRequest::Result::HostNotFound: what = "host not found: " + _activeUrl.host(); break;
                case ResourceRequest::Result::Timeout: what = "timed out"; break;
                case ResourceRequest::Result::ServerUnavailable: what = "server unavailable"; break;
                case ResourceRequest::Result::AccessDenied: what = "access denied"; break;
                case ResourceRequest::Result::NotFound: what = "not found"; break;
                case ResourceRequest::Result::InvalidURL: what = "not a valid address"; break;
                case ResourceRequest::Result::RedirectFail: what = "redirected where it may not go"; break;
                default: what = "could not be fetched"; break;
            }
            int statusCode = _request ? _request->getStatusCode() : 0;
            _failureReason = statusCode >= 400 ? QString("%1 %2").arg(statusCode).arg(what) : what;
            auto error = (result == ResourceRequest::Timeout) ? QNetworkReply::TimeoutError
                                                              : QNetworkReply::UnknownNetworkError;
            emit failed(error);
            willRetry = false;
            finishedLoading(false);
            break;
        }
    }
    return willRetry;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
size_t qHash(const QPointer<QObject>& value, size_t seed) {
#else
uint qHash(const QPointer<QObject>& value, uint seed) {
#endif
    return qHash(value.data(), seed);
}
