//
//  ScriptCache.h
//  libraries/script-engine/src
//
//  Created by Brad Hefta-Gaub on 2015-03-30
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ScriptCache_h
#define hifi_ScriptCache_h

#include <mutex>
#include <ResourceCache.h>

using contentAvailableCallback = std::function<void(const QString& scriptOrURL, const QString& contents, bool isURL, bool contentAvailable, const QString& status)>;
/// The bytes of a script as fetched (a WebAssembly module); `success` false with the status when they could not be.
using bytesAvailableCallback = std::function<void(const QString& url, const QByteArray& bytes, bool success, const QString& status)>;

class ScriptUser {
public:
    virtual void scriptContentsAvailable(const QUrl& url, const QString& scriptContents) = 0;
    virtual void errorInLoadingScript(const QUrl& url) = 0;
};

class ScriptRequest {
public:
    static const int MAX_RETRIES { 5 };
    static const int START_DELAY_BETWEEN_RETRIES { 200 };
    std::vector<contentAvailableCallback> scriptUsers { };
    std::vector<bytesAvailableCallback> byteUsers { };
    int numRetries { 0 };
    int maxRetries { MAX_RETRIES };
    // a ruled script's download (ScriptCache::ruleFetch): its retries are ruled too
    bool ruled { false };
    QUrl ruleOrigin;
    bool ruleWorldIsLocal { false };
    /// The rule this download runs under, as one key (ScriptCache::ruleKey); empty for a download under no rule.
    QString ruleKey() const;
    // ruled callers that may not join this download (it runs under no rule, or under another): each gets its own
    // ruled download when this one ends
    std::vector<contentAvailableCallback> ruledWaiters { };
    std::vector<bytesAvailableCallback> ruledByteWaiters { };
    QUrl waitOrigin;
    bool waitWorldIsLocal { false };
};

/// Interface for loading scripts
class ScriptCache : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    using Mutex = std::mutex;
    using Lock = std::unique_lock<Mutex>;

public:
    static const QString STATUS_INLINE;
    static const QString STATUS_CACHED;
    /// Whether an entity's script property holds the script itself (code, not an address).
    static bool isInlineScript(const QString& scriptOrURL);
    static bool isSuccessStatus(const QString& status) {
        return status == "Success" || status == STATUS_INLINE || status == STATUS_CACHED;
    }

    void clearCache();
    Q_INVOKABLE void clearATPScriptsFromCache();
    void getScriptContents(const QString& scriptOrURL, contentAvailableCallback contentAvailable, bool forceDownload = false, int maxRetries = ScriptRequest::MAX_RETRIES);
    /// The same fetch and cache, delivering the bytes as they came rather than as text (a WebAssembly module).
    void getScriptBytes(const QString& url, bytesAvailableCallback bytesAvailable, bool forceDownload = false, int maxRetries = ScriptRequest::MAX_RETRIES);

    void deleteScript(const QUrl& unnormalizedURL);

    /// The next fetch of this URL is a ruled script's own (ScriptNetworkPolicy): its download judges and pins each
    /// address it goes to, and the rule holds for the cache too: a ruled fetch takes a cached copy only when that copy
    /// was fetched under the same rule, and never joins a download under no rule or another one; otherwise it makes
    /// its own ruled download.
    void ruleFetch(const QUrl& url, const QUrl& origin, bool worldIsLocal);
    /// A rule as one key, "origin|local" or "origin|remote": what a cached copy records and a ruled fetch asks for.
    static QString ruleKey(const QUrl& origin, bool worldIsLocal);

private:
    void applyRule(ResourceRequest* request, const QUrl& url);
    struct FetchRule { QUrl origin; bool worldIsLocal; };
    QHash<QUrl, FetchRule> _fetchRules;   // under _containerLock
    void scriptContentAvailable(int maxRetries); // new version
    void request(const QUrl& url, contentAvailableCallback contentAvailable, bytesAvailableCallback bytesAvailable, bool forceDownload, int maxRetries);
    ScriptCache(QObject* parent = NULL);
    
    Mutex _containerLock;
    QMap<QUrl, ScriptRequest> _activeScriptRequests;
    
    QHash<QUrl, QVariantMap> _scriptCache;
    QMultiMap<QUrl, ScriptUser*> _scriptUsers;
};

#endif // hifi_ScriptCache_h
