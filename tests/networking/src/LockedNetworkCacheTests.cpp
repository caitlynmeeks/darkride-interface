//
//  LockedNetworkCacheTests.cpp
//  tests/networking/src
//
//  Created by servers on 2026-11-09.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "LockedNetworkCacheTests.h"

#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include <QtNetwork/QNetworkDiskCache>
#include <QTemporaryDir>

#include <NetworkAccessManager.h>

QTEST_MAIN(LockedNetworkCacheTests)

namespace {

const int KEYS = 16;
const int THREADS = 6;
const int RUN_MSECS = 1500;

QUrl urlFor(int key) {
    return QUrl(QString("http://cache-test.invalid/item-%1").arg(key));
}

// a payload names its key and its length, and fills the rest with one letter per key
QByteArray payloadFor(int key, int length) {
    QByteArray payload = QString("key:%1:length:%2:").arg(key).arg(length).toUtf8();
    payload.append(QByteArray(length - payload.size(), char('a' + key)));
    return payload;
}

bool isWholePayload(const QByteArray& bytes, int key) {
    const QByteArray prefix = QString("key:%1:length:").arg(key).toUtf8();
    if (!bytes.startsWith(prefix)) {
        return false;
    }
    int end = bytes.indexOf(':', prefix.size());
    bool ok = false;
    int length = bytes.mid(prefix.size(), end - prefix.size()).toInt(&ok);
    if (!ok || bytes.size() != length) {
        return false;
    }
    for (int i = end + 1; i < bytes.size(); ++i) {
        if (bytes[i] != char('a' + key)) {
            return false;
        }
    }
    return true;
}

struct Counts {
    std::atomic<int> writes { 0 };
    std::atomic<int> reads { 0 };
    std::atomic<int> brokenReads { 0 };
    std::atomic<int> wrongMetaData { 0 };
};

// what the asset client, the http requests and the main thread's manager do with the cache, all at once: write an
// item (prepare, write, insert), read it back whole, look up its metadata, now and then remove it or ask the size
void useCache(QAbstractNetworkCache& cache, int seed, Counts& counts) {
    std::mt19937 random(seed);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < RUN_MSECS) {
        const int key = random() % KEYS;
        const QUrl url = urlFor(key);
        switch (random() % 4) {
            case 0: {
                QNetworkCacheMetaData metaData;
                metaData.setUrl(url);
                metaData.setSaveToDisk(true);
                metaData.setLastModified(QDateTime::currentDateTimeUtc());
                if (QIODevice* device = cache.prepare(metaData)) {
                    device->write(payloadFor(key, 2000 + (int)(random() % 38000)));
                    cache.insert(device);
                    counts.writes++;
                }
                break;
            }
            case 1: {
                std::unique_ptr<QIODevice> device(cache.data(url));
                if (device) {
                    counts.reads++;
                    if (!isWholePayload(device->readAll(), key)) {
                        counts.brokenReads++;
                    }
                }
                break;
            }
            case 2: {
                QNetworkCacheMetaData metaData = cache.metaData(url);
                if (metaData.isValid() && metaData.url() != url) {
                    counts.wrongMetaData++;
                }
                break;
            }
            default:
                if (random() % 8 == 0) {
                    cache.remove(url);
                } else {
                    cache.cacheSize();
                }
                break;
        }
    }
}

} // namespace

// Two managers (two LockedNetworkCaches) loading one address: only the first writes it to the cache; the second's
// prepare() is declined, as when the disk cache declines an item, until the first write is in.
void LockedNetworkCacheTests::oneWriteInProgressPerAddress() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QNetworkDiskCache diskCache;
    diskCache.setCacheDirectory(directory.path());
    LockedNetworkCache first(&diskCache);
    LockedNetworkCache second(&diskCache);
    QNetworkCacheMetaData metaData;
    metaData.setUrl(urlFor(1));
    metaData.setSaveToDisk(true);

    QIODevice* device = first.prepare(metaData);
    QVERIFY(device);
    QVERIFY(!second.prepare(metaData));
    device->write(payloadFor(1, 5000));
    first.insert(device);
    QIODevice* again = second.prepare(metaData);
    QVERIFY(again);
    again->write(payloadFor(1, 6000));
    second.insert(again);
    std::unique_ptr<QIODevice> read(first.data(urlFor(1)));
    QVERIFY(read);
    QVERIFY(isWholePayload(read->readAll(), 1));
}

// One manager's remove() of an address another manager is still writing leaves that write alone (the disk cache's own
// remove() would cancel it and free the device being written); the writer's own remove() still cancels its write.
void LockedNetworkCacheTests::aRemoveNeverCancelsAnotherCachesWrite() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QNetworkDiskCache diskCache;
    diskCache.setCacheDirectory(directory.path());
    LockedNetworkCache writer(&diskCache);
    LockedNetworkCache other(&diskCache);
    QNetworkCacheMetaData metaData;
    metaData.setUrl(urlFor(2));
    metaData.setSaveToDisk(true);

    QIODevice* device = writer.prepare(metaData);
    QVERIFY(device);
    QVERIFY(!other.remove(urlFor(2)));
    device->write(payloadFor(2, 40000));  // still the writer's: not freed
    writer.insert(device);
    std::unique_ptr<QIODevice> read(other.data(urlFor(2)));
    QVERIFY(read);
    QVERIFY(isWholePayload(read->readAll(), 2));

    QIODevice* cancelled = writer.prepare(metaData);
    QVERIFY(cancelled);
    cancelled->write(payloadFor(2, 3000));
    QVERIFY(writer.remove(urlFor(2)));  // the writer cancels its own write
    QVERIFY(other.prepare(metaData) != nullptr);  // nothing in progress for the address any more
}

// Six threads use one disk cache at once, each through its own LockedNetworkCache, as every thread's network manager
// does (NetworkAccessManager::getInstance). The cache is small, so inserts also remove the oldest items. Every item
// read back is whole and every metadata lookup names its own address.
void LockedNetworkCacheTests::manyThreadsShareOneDiskCache() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QNetworkDiskCache diskCache;
    diskCache.setCacheDirectory(directory.path());
    diskCache.setMaximumCacheSize(256 * 1024);

    Counts counts;
    std::vector<std::unique_ptr<LockedNetworkCache>> caches;
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        caches.emplace_back(new LockedNetworkCache(&diskCache));
    }
    for (int i = 0; i < THREADS; ++i) {
        LockedNetworkCache* cache = caches[i].get();
        threads.emplace_back([cache, i, &counts] { useCache(*cache, 1000 + i, counts); });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    qInfo() << counts.writes << "writes," << counts.reads << "reads," << counts.brokenReads << "broken reads,"
            << counts.wrongMetaData << "wrong metadata; cache size" << caches[0]->cacheSize();
    QVERIFY2(counts.writes > 100 && counts.reads > 100, "the threads did too little to show anything");
    QCOMPARE(counts.brokenReads.load(), 0);
    QCOMPARE(counts.wrongMetaData.load(), 0);
    QVERIFY(caches[0]->cacheSize() <= 256 * 1024);
}

// The old way, for a look by hand: the same six threads straight into the one QNetworkDiskCache, with no lock. It
// breaks reads or crashes the test, which is why it only runs when asked (TIVOLI_SHOW_CACHE_RACE=1).
void LockedNetworkCacheTests::unlockedSharingRaces() {
    if (qEnvironmentVariableIsEmpty("TIVOLI_SHOW_CACHE_RACE")) {
        QSKIP("shows the unlocked race (broken reads or a crash); set TIVOLI_SHOW_CACHE_RACE=1 to run it");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QNetworkDiskCache diskCache;
    diskCache.setCacheDirectory(directory.path());
    diskCache.setMaximumCacheSize(256 * 1024);

    Counts counts;
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&diskCache, i, &counts] { useCache(diskCache, 1000 + i, counts); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    qInfo() << "UNLOCKED:" << counts.writes << "writes," << counts.reads << "reads," << counts.brokenReads
            << "broken reads," << counts.wrongMetaData << "wrong metadata";
}
