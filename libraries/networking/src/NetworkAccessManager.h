//
//  NetworkAccessManager.h
//  libraries/networking/src
//
//  Created by Clement on 7/1/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_NetworkAccessManager_h
#define hifi_NetworkAccessManager_h

#include <QtNetwork/QAbstractNetworkCache>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtQml/QQmlNetworkAccessManagerFactory>

class QNetworkDiskCache;

/// Wrapper around QNetworkAccessManager to restrict at one instance by thread
class NetworkAccessManager : public QNetworkAccessManager {
    Q_OBJECT
public:
    static QNetworkAccessManager& getInstance();
protected:
    NetworkAccessManager(QObject* parent = Q_NULLPTR) : QNetworkAccessManager(parent) {}
    virtual QNetworkReply* createRequest(Operation op, const QNetworkRequest& request, QIODevice* device = Q_NULLPTR) override;
};

/// One disk cache serves every thread's QNetworkAccessManager (getInstance gives each thread its own manager), and a
/// QNetworkDiskCache is not thread-safe: two threads in it at once corrupt the item it keeps open and the inserts in
/// progress. Each manager, and anything else that uses the disk cache, gets its own LockedNetworkCache, which passes
/// every call on to the one disk cache under one lock. The disk cache itself is owned elsewhere (ResourceManager),
/// never by a manager. A write in progress (prepare(), then the caller writes, then insert()) is the one thing done
/// outside the lock, so at most one write per address is in progress at a time, and a remove() never cancels another
/// LockedNetworkCache's write (QNetworkDiskCache::remove cancels whichever write it finds for the address, freeing the
/// device another thread is still writing to).
class LockedNetworkCache : public QAbstractNetworkCache {
    Q_OBJECT
public:
    LockedNetworkCache(QNetworkDiskCache* diskCache, QObject* parent = nullptr);
    ~LockedNetworkCache();

    QNetworkCacheMetaData metaData(const QUrl& url) override;
    void updateMetaData(const QNetworkCacheMetaData& metaData) override;
    QIODevice* data(const QUrl& url) override;
    bool remove(const QUrl& url) override;
    qint64 cacheSize() const override;
    QIODevice* prepare(const QNetworkCacheMetaData& metaData) override;
    void insert(QIODevice* device) override;

    // the disk cache's own settings
    QString cacheDirectory() const;
    void setCacheDirectory(const QString& cacheDirectory);
    qint64 maximumCacheSize() const;
    void setMaximumCacheSize(qint64 size);

public slots:
    void clear() override;

private:
    QNetworkDiskCache* _diskCache;
};

#endif // hifi_NetworkAccessManager_h