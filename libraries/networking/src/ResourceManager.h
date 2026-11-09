//
//  ResourceManager.h
//  libraries/networking/src
//
//  Created by Ryan Huffman on 2015/07/23
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ResourceManager_h
#define hifi_ResourceManager_h

#include <functional>

#include <QNetworkDiskCache>
#include <QObject>
#include <QtCore/QMutex>
#include <QThread>

#include <DependencyManager.h>

#include "ResourceRequest.h"

class ResourceManager: public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:
    ResourceManager(bool atpSupportEnabled = true);
    ~ResourceManager();

    void setUrlPrefixOverride(const QString& prefix, const QString& replacement);
    QString normalizeURL(const QString& urlString);
    QUrl normalizeURL(const QUrl& url);

    ResourceRequest* createResourceRequest(
        QObject* parent,
        const QUrl& url,
        const bool isObservable = true,
        const qint64 callerId = -1,
        const QString& extra = "");

    void init();
    void cleanup();

    // Blocking call to check if a resource exists. This function uses a QEventLoop internally
    // to return to the calling thread so that events can still be processed.
    bool resourceExists(const QUrl& url);

    // adjust where we persist the cache
    void setCacheDir(const QString& cacheDir);

    /// Every http, https and ftp request made from now on is held to the script network rule, judged as from `origin`
    /// (ResourceRequest::setNetworkRule: each hop judged, then sent to the address judged), for a process whose web
    /// requests are all its scripts' (the entity script server: its scripts, their includes, the sounds they load).
    /// Set once, at the process's start.
    void ruleEveryWebRequest(const QUrl& origin);

    QNetworkDiskCache* networkDiskCache = new QNetworkDiskCache();

private:
    QThread _thread;

    using PrefixMap = std::map<QString, QString>;

    bool _atpSupportEnabled;
    PrefixMap _prefixMap;
    QMutex _prefixMapLock;

    QUrl _everyWebRequestRuleOrigin;   // ruleEveryWebRequest; empty: none
    QMutex _ruleLock;

};

#endif
