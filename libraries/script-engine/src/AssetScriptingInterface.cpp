//
//  AssetScriptingInterface.cpp
//  libraries/script-engine/src
//
//  Created by Stephen Birarda on 2016-03-08.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssetScriptingInterface.h"

#include <QMimeDatabase>
#include <QThread>

#include <AssetRequest.h>
#include <AssetUpload.h>
#include <AssetUtils.h>
#include <MappingRequest.h>
#include <NodeList.h>

#include <RegisteredMetaTypes.h>
#include "ScriptEngine.h"
#include "ScriptEngineLogging.h"

#include <shared/QtHelpers.h>
#include <Gzip.h>
#include <HostInterface.h>

#include <future>

using Promise = MiniPromise::Promise;

AssetScriptingInterface::AssetScriptingInterface(QObject* parent) : BaseAssetScriptingInterface(parent) {
    qCDebug(scriptengine) << "AssetScriptingInterface::AssetScriptingInterface" << parent;
}

bool AssetScriptingInterface::initializeCache() {
    if (!Parent::initializeCache()) {
        if (assetClient()) {
            std::promise<bool> cacheStatusResult;
            Promise assetClientPromise(makePromise(__func__));
            assetClientPromise->moveToThread(qApp->thread());  // To ensure the finally() is processed.

            assetClient()->cacheInfoRequestAsync(assetClientPromise);
            assetClientPromise->finally([&](QString, QVariantMap result)
                { cacheStatusResult.set_value(!result.isEmpty()); });
            return cacheStatusResult.get_future().get();
        } else {
            return false;
        }
    } else {
        return true;
    }
}

/**jsdoc
 * The success or failure of an {@link Assets.downloadData} call.
 * @typedef {object} Assets.DownloadDataError
 * @property {string} errorMessage - <code>""</code> if the download was successful, otherwise a description of the error.
 */

#if (PR_BUILD || DEV_BUILD)
void AssetScriptingInterface::sendFakedHandshake() {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);

    nodeList->sendFakedHandshakeRequestToNode(assetServer);
}

#endif

/**jsdoc
 * Source and download options for {@link Assets.getAsset}.
 * @typedef {object} Assets.GetOptions
 * @property {boolean} [decompress=false] - <code>true</code> to gunzip decompress the downloaded data. Synonym:
 *     <code>compressed</code>.
 * @property {Assets.ResponseType} [responseType="text"] - The desired result type.
 * @property {string} url - The mapped path or hash to download. May have a leading <code>"atp:"</code>.
 */
/**jsdoc
 * Result value returned by {@link Assets.getAsset}. 
 * @typedef {object} Assets.GetResult
 * @property {number} [byteLength] - The number of bytes in the downloaded content in <code>response</code>.
 * @property {boolean} cached - <code>true</code> if the item was retrieved from the cache, <code>false</code> if it was 
 *     downloaded.
 * @property {string} [contentType] - The automatically detected MIME type of the content.
 * @property {boolean} [decompressed] - <code>true</code> if the content was decompressed, <code>false</code> if it wasn't.
 * @property {string} [hash] - The hash for the downloaded asset.
 * @property {string} [hashURL] - The ATP URL of the hash file.
 * @property {string} [path] - The path for the asset, if a path was requested. Otherwise, <code>undefined</code>.
 * @property {string|object|ArrayBuffer} [response] - The downloaded content.
 * @property {Assets.ResponseType} [responseType] - The type of the downloaded content in <code>response</code>.
 * @property {string} [url] - The URL of the asset requested: the path with leading <code>"atp:"</code> if a path was 
 *     requested, otherwise the requested URL.
 * @property {boolean} [wasRedirected] - <code>true</code> if the downloaded data is the baked version of the asset, 
 *      <code>false</code> if it isn't baked.
 */

/**jsdoc
 * Source options for {@link Assets.resolveAsset}.
 * @typedef {object} Assets.ResolveOptions
 * @property {string} url - The hash or path to resolve. May have a leading <code>"atp:"</code>.
 */
/**jsdoc
 * Result value returned by {@link Assets.resolveAsset}.
 * <p>Note: If resolving a hash, a file of that hash need not be present on the asset server for the hash to resolve.</p>
 * @typedef {object} Assets.ResolveResult
 * @property {string} [hash] - The hash of the asset.
 * @property {string} [hashURL] - The url of the asset's hash file, with leading <code>atp:</code>. 
 * @property {string} [path] - The path to the asset.
 * @property {string} [url] - The URL of the asset.
 * @property {boolean} [wasRedirected] - <code>true</code> if the resolved data is for the baked version of the asset,
 *      <code>false</code> if it isn't.
 */

/**jsdoc
 * Content and decompression options for {@link Assets.decompressData}.
 * @typedef {object} Assets.DecompressOptions
 * @property {ArrayBuffer} data - The data to decompress.
 * @property {Assets.ResponseType} [responseType=text] - The type of decompressed data to return.
 */
/**jsdoc
 * Result value returned by {@link Assets.decompressData}.
 * @typedef {object} Assets.DecompressResult
 * @property {number} [byteLength] - The number of bytes in the decompressed data.
 * @property {string} [contentType] - The MIME type of the decompressed data.
 * @property {boolean} [decompressed] - <code>true</code> if the data is decompressed.
 * @property {string|object|ArrayBuffer} [response] - The decompressed data.
 * @property {Assets.ResponseType} [responseType] - The type of the decompressed data in <code>response</code>.
 */

namespace {
    const int32_t DEFAULT_GZIP_COMPRESSION_LEVEL = -1;
    const int32_t MAX_GZIP_COMPRESSION_LEVEL = 9;
}

/**jsdoc
 * Content and compression options for {@link Assets.compressData}.
 * @typedef {object} Assets.CompressOptions
 * @property {string|ArrayBuffer} data - The data to compress.
 * @property {number} level - The compression level, range <code>-1</code> &ndash; <code>9</code>. <code>-1</code> means 
 *     use the default gzip compression level, <code>0</code> means no compression, and <code>9</code> means maximum 
 *     compression.
 */
/**jsdoc
 * Result value returned by {@link Assets.compressData}.
 * @typedef {object} Assets.CompressResult
 * @property {number} [byteLength] - The number of bytes in the compressed data.
 * @property {boolean} [compressed] - <code>true</code> if the data is compressed.
 * @property {string} [contentType] - The MIME type of the compressed data, i.e., <code>"application/gzip"</code>.
 * @property {ArrayBuffer} [data] - The compressed data.
 */

/**jsdoc
 * Content and upload options for {@link Assets.putAsset}.
 * @typedef {object} Assets.PutOptions
 * @property {boolean} [compress=false] - <code>true</code> to gzip compress the content for upload and storage,
 *     <code>false</code> to upload and store the data without gzip compression. Synonym: <code>compressed</code>.
 * @property {string|ArrayBuffer} data - The content to upload.
 * @property {string} [path] - A user-friendly path for the file in the asset server. May have a leading 
 *     <code>"atp:"</code>. If not specified, no path-to-hash mapping is set.
 *     <p>Note: The asset server destroys any unmapped SHA256-named file at server restart. Either set the mapping path 
 *     with this property or use {@link Assets.setMapping} to set a path-to-hash mapping for the uploaded file.</p>
 */
/**jsdoc
 * Result value returned by {@link Assets.putAsset}.
 * @typedef {object} Assets.PutResult
 * @property {number} [byteLength] - The number of bytes in the hash file stored on the asset server.
 * @property {boolean} [compressed] - <code>true</code> if the content stored is gzip compressed.
 * @property {string} [contentType] - <code>"application/gzip"</code> if the content stored is gzip compressed.
 * @property {string} [hash] - The SHA256 hash of the content.
 * @property {string} [url] - The <code>atp:</code> URL of the content: using the path if specified, otherwise the hash.
 * @property {string} [path] - The uploaded content's mapped path, if specified.
 */

/**jsdoc
 * Source for {@link Assets.queryCacheMeta}.
 * @typedef {object} Assets.QueryCacheMetaOptions
 * @property {string} url - The URL of the cached asset to get information on. Must start with <code>"atp:"</code> or 
 *     <code>"cache:"</code>.
 */

/**jsdoc
 * Source and retrieval options for {@link Assets.loadFromCache}.
 * @typedef {object} Assets.LoadFromCacheOptions
 * @property {boolean} [decompress=false] - <code>true</code> to gunzip decompress the cached data. Synonym:
 *     <code>compressed</code>.
 * @property {Assets.ResponseType} [responseType=text] - The desired result type.
 * @property {string} url - The URL of the asset to load from cache. Must start with <code>"atp:"</code> or
 *     <code>"cache:"</code>.
 */

bool AssetScriptingInterface::canWriteCacheValue(const QUrl& url) {
    auto scriptEngine = qobject_cast<ScriptEngine*>(parent());   // the engine this object is part of
    if (!scriptEngine) {
        return false;
    }
    // allow cache writes only from Client, EntityServer and Agent scripts
    bool isAllowedContext = (
        scriptEngine->isClientScript() ||
        scriptEngine->isAgentScript()
    );
    if (!isAllowedContext) {
        return false;
    }
    return true;
}

/**jsdoc
 * The data to save to the cache and cache options for {@link Assets.saveToCache}.
 * @typedef {object} Assets.SaveToCacheOptions
 * @property {string|ArrayBuffer} data - The data to save to the cache.
 * @property {Assets.SaveToCacheHeaders} [headers] - The last-modified and expiry times for the cache item.
 * @property {string} [url] - The URL to associate with the cache item. Must start with <code>"atp:"</code> or
 *     <code>"cache:"</code>. If not specified, the URL is <code>"atp:"</code> followed by the SHA256 hash of the content.
 */

// --- the methods ----------------------------------------------------------------------------------------------------
// Options come as a QVariant (the text of a URL, or a map); the callback is posted to the script, so it runs after
// the call returns.

namespace {

// A check that fails: an error record, which the script's call raises.
QVariant verifyFailed(const QString& error) {
    return HostInterface::errorRecord("Error", error);
}

// The first argument a callback gets: null when there is no error.
QVariant errorOrNull(const QString& error) {
    return error.isEmpty() ? QVariant::fromValue(nullptr) : QVariant(error);
}

// An option as JavaScript reads it: its truthiness.
bool truthy(const QVariant& value) {
    switch (value.userType()) {
        case QMetaType::UnknownType:
        case QMetaType::Nullptr:
            return false;
        case QMetaType::QString:
            return !value.toString().isEmpty();
        case QMetaType::Bool:
            return value.toBool();
        case QMetaType::QVariantMap:
        case QMetaType::QVariantList:
        case QMetaType::QByteArray:
            return true;
        default: {
            bool isNumber = false;
            double number = value.toDouble(&isNumber);
            return isNumber ? (number != 0.0 && number == number) : value.isValid();
        }
    }
}

// Data as ArrayBufferClass::fromScriptValue reads it: text as UTF-8, bytes as they are, a list of numbers as bytes.
QByteArray bytesOf(const QVariant& value) {
    switch (value.userType()) {
        case QMetaType::QString:
            return value.toString().toUtf8();
        case QMetaType::QByteArray:
            return value.toByteArray();
        case QMetaType::QVariantList: {
            QByteArray bytes;
            for (const QVariant& item : value.toList()) {
                bytes.append(char(uint8_t(item.toInt())));
            }
            return bytes;
        }
        default:
            return QByteArray();
    }
}

// The URL in options given as the URL itself or as {url}.
QString urlOf(const QVariant& options) {
    return options.userType() == QMetaType::QString ? options.toString() : options.toMap().value("url").toString();
}

const QString NO_CALLBACK = "expected a callback function";

} // namespace

AssetScriptingInterface::Promise AssetScriptingInterface::callbackReady(Promise promise, const ScriptCallback& callback) {
    return promise->ready([callback](QString error, QVariantMap result) {
        callback.post({ errorOrNull(error), result });
    });
}

QVariant AssetScriptingInterface::uploadData(const QString& data, const ScriptCallback& callback) {
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    auto upload = DependencyManager::get<AssetClient>()->createUpload(data.toUtf8());
    Promise deferred = makePromise(__FUNCTION__);
    deferred->ready([callback](QString error, QVariantMap result) {
        // the callback gets (url, hash)
        callback.post({ result.value("url").toString(), result.value("hash").toString() });
    });
    connect(upload, &AssetUpload::finished, upload, [deferred](AssetUpload* upload, const QString& hash) {
        deferred->resolve({
            { "url", "atp:" + hash },
            { "hash", hash },
        });
        upload->deleteLater();
    });
    upload->start();
    return QVariant();
}

QVariant AssetScriptingInterface::setMapping(const QString& path, const QString& hash, const ScriptCallback& callback) {
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    auto setMappingRequest = assetClient()->createSetMappingRequest(path, hash);
    Promise deferred = makePromise(__FUNCTION__);
    callbackReady(deferred, callback);
    connect(setMappingRequest, &SetMappingRequest::finished, setMappingRequest, [deferred](SetMappingRequest* request) {
        QString error = request->getErrorString();
        deferred->handle(error, { { "error", request->getError() } });
        request->deleteLater();
    });
    setMappingRequest->start();
    return QVariant();
}

QVariant AssetScriptingInterface::downloadData(const QString& urlString, const ScriptCallback& callback) {
    if (!urlString.startsWith(ATP_SCHEME)) {
        qCDebug(scriptengine) << "AssetScriptingInterface::downloadData url must be of form atp:<hash-value>";
        return QVariant();
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    QString hash = AssetUtils::extractAssetHash(urlString);
    auto assetRequest = DependencyManager::get<AssetClient>()->createRequest(hash);
    Promise deferred = makePromise(__FUNCTION__);
    deferred->ready([callback](QString error, QVariantMap result) {
        // the callback gets (data, { errorMessage })
        callback.post({ errorOrNull(result.value("data").toString()), QVariantMap { { "errorMessage", error } } });
    });
    connect(assetRequest, &AssetRequest::finished, assetRequest, [deferred](AssetRequest* request) {
        if (request->getError() == AssetRequest::Error::NoError) {
            deferred->resolve({ { "data", QString::fromUtf8(request->getData()) } });
        } else {
            qCDebug(scriptengine) << "AssetScriptingInterface::downloadData ERROR: " << request->getErrorString();
        }
        request->deleteLater();
    });
    assetRequest->start();
    return QVariant();
}

QVariant AssetScriptingInterface::getMapping(const QString& asset, const ScriptCallback& callback) {
    auto path = AssetUtils::getATPUrl(asset).path();
    if (!AssetUtils::isValidFilePath(path)) {
        return verifyFailed("invalid ATP file path: " + asset + "(path:" + path + ")");
    }
    if (!callback.isValid()) {
        return verifyFailed("expected second parameter to be a callback function");
    }
    getAssetInfo(path)->ready([callback](QString error, QVariantMap result) {
        // the callback gets (error, hash)
        callback.post({ errorOrNull(error), result.value("hash").toString() });
    });
    return QVariant();
}

QVariant AssetScriptingInterface::deleteAsset(const QVariant& options, const ScriptCallback& callback) {
    Q_UNUSED(options);
    Q_UNUSED(callback);
    return verifyFailed("TODO: deleteAsset API");
}

QVariant AssetScriptingInterface::getAsset(const QVariant& options, const ScriptCallback& callback) {
    const bool isText = options.userType() == QMetaType::QString;
    if (!isText && options.userType() != QMetaType::QVariantMap) {
        return verifyFailed("expected request options Object or URL as first parameter");
    }
    const QVariantMap map = options.toMap();
    const bool decompress = truthy(map.value("decompress")) || truthy(map.value("compressed"));
    QString responseType = map.value("responseType").toString().toLower();
    if (responseType.isEmpty()) {
        responseType = "text";
    }
    const QString url = urlOf(options);
    auto asset = AssetUtils::getATPUrl(url).path();
    if (!AssetUtils::isValidHash(asset) && !AssetUtils::isValidFilePath(asset)) {
        return verifyFailed(QString("Invalid ATP url '%1'").arg(url));
    }
    if (!RESPONSE_TYPES.contains(responseType)) {
        return verifyFailed(QString("Invalid responseType: '%1' (expected: %2)").arg(responseType).arg(RESPONSE_TYPES.join(" | ")));
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    Promise fetched = callbackReady(makePromise("fetched"), callback);
    Promise mapped = makePromise("mapped");
    mapped->fail(fetched);
    mapped->then([=](QVariantMap result) {
        QString hash = result.value("hash").toString();
        QString mappedUrl = result.value("url").toString();
        if (!AssetUtils::isValidHash(hash)) {
            fetched->reject("internal hash error: " + hash, result);
        } else {
            Promise promise = loadAsset(hash, decompress, responseType);
            promise->mixin(result);
            promise->ready([=](QString error, QVariantMap loadResult) {
                loadResult["url"] = mappedUrl;  // the mapped .url
                fetched->handle(error, loadResult);
            });
        }
    });
    if (AssetUtils::isValidHash(asset)) {
        mapped->resolve({
            { "hash", asset },
            { "url", url },
        });
    } else {
        getAssetInfo(asset)->ready(mapped);
    }
    return QVariant();
}

QVariant AssetScriptingInterface::resolveAsset(const QVariant& options, const ScriptCallback& callback) {
    auto asset = AssetUtils::getATPUrl(urlOf(options)).path();
    if (!AssetUtils::isValidFilePath(asset) && !AssetUtils::isValidHash(asset)) {
        return verifyFailed("expected options to be an asset URL or request options containing .url property");
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    callbackReady(getAssetInfo(asset), callback);
    return QVariant();
}

QVariant AssetScriptingInterface::decompressData(const QVariant& options, const ScriptCallback& callback) {
    const QVariantMap map = options.toMap();
    QByteArray dataByteArray = bytesOf(map.value("data"));
    QString responseType = map.value("responseType").toString().toLower();
    if (responseType.isEmpty()) {
        responseType = "text";
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    Promise completed = callbackReady(makePromise(__FUNCTION__), callback);
    Promise decompressed = decompressBytes(dataByteArray);
    if (responseType == "arraybuffer") {
        decompressed->ready(completed);
    } else {
        decompressed->ready([=](QString error, QVariantMap result) {
            Promise converted = convertBytes(result.value("data").toByteArray(), responseType);
            converted->mixin(result);
            converted->ready(completed);
        });
    }
    return QVariant();
}

QVariant AssetScriptingInterface::compressData(const QVariant& options, const ScriptCallback& callback) {
    const QVariantMap map = options.toMap();
    // the data is options.data when there is one, else the options themselves (text or bytes)
    QVariant data = map.contains("data") ? map.value("data") : options;
    QByteArray dataByteArray = bytesOf(data);
    const QVariant levelValue = map.value("level");
    bool isNumber = false;
    double levelNumber = levelValue.toDouble(&isNumber);
    isNumber = isNumber && levelValue.userType() != QMetaType::QString && levelValue.userType() != QMetaType::Bool;
    int level = isNumber ? int(levelNumber) : DEFAULT_GZIP_COMPRESSION_LEVEL;
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    callbackReady(compressBytes(dataByteArray, level), callback);
    return QVariant();
}

QVariant AssetScriptingInterface::putAsset(const QVariant& options, const ScriptCallback& callback) {
    const bool isObject = options.userType() == QMetaType::QVariantMap;
    const QVariantMap map = options.toMap();
    const bool compress = truthy(map.value("compress")) || truthy(map.value("compressed"));
    QVariant data = isObject ? map.value("data") : options;
    QString rawPath = map.value("path").toString();
    auto path = AssetUtils::getATPUrl(rawPath).path();
    QByteArray dataByteArray = bytesOf(data);
    if (!path.isEmpty() && !AssetUtils::isValidFilePath(path)) {
        return verifyFailed(QString("expected valid ATP file path '%1' ('%2')").arg(rawPath).arg(path));
    }
    if (dataByteArray.size() <= 0) {
        return verifyFailed(QString("expected non-zero .data (got %1 / #%2 bytes)").arg(data.typeName()).arg(dataByteArray.size()));
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    // [compressed] => uploaded to server => [mapped to path]
    Promise prepared = makePromise("putAsset::prepared");
    Promise uploaded = makePromise("putAsset::uploaded");
    Promise completed = makePromise("putAsset::completed");
    callbackReady(completed, callback);
    if (compress) {
        compressBytes(dataByteArray, DEFAULT_GZIP_COMPRESSION_LEVEL)->ready(prepared);
    } else {
        prepared->resolve({ { "data", dataByteArray } });
    }
    prepared->fail(completed);
    prepared->then([=](QVariantMap result) {
        Promise upload = uploadBytes(result.value("data").toByteArray());
        upload->mixin(result);
        upload->ready(uploaded);
    });
    uploaded->fail(completed);
    if (path.isEmpty()) {
        uploaded->then(completed);
    } else {
        uploaded->then([=](QVariantMap result) {
            QString hash = result.value("hash").toString();
            if (!AssetUtils::isValidHash(hash)) {
                completed->reject("path mapping requested, but did not receive valid hash", result);
            } else {
                Promise link = symlinkAsset(hash, path);
                link->mixin(result);
                link->ready(completed);
            }
        });
    }
    return QVariant();
}

QVariant AssetScriptingInterface::getCacheStatus(const ScriptCallback& callback) {
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    callbackReady(Parent::getCacheStatus(), callback);
    return QVariant();
}

QVariant AssetScriptingInterface::queryCacheMeta(const QVariant& options, const ScriptCallback& callback) {
    QString url = urlOf(options);
    if (!QUrl(url).isValid()) {
        return verifyFailed(QString("Invalid URL '%1'").arg(url));
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    callbackReady(Parent::queryCacheMeta(url), callback);
    return QVariant();
}

QVariant AssetScriptingInterface::loadFromCache(const QVariant& options, const ScriptCallback& callback) {
    QString url = urlOf(options);
    QString responseType = "text";
    bool decompress = false;
    if (options.userType() != QMetaType::QString) {
        const QVariantMap map = options.toMap();
        if (map.contains("responseType")) {
            responseType = map.value("responseType").toString();
        }
        decompress = truthy(map.value("decompress")) || truthy(map.value("compressed"));
    }
    if (!QUrl(url).isValid()) {
        return verifyFailed(QString("Invalid URL '%1'").arg(url));
    }
    if (!RESPONSE_TYPES.contains(responseType)) {
        return verifyFailed(QString("Invalid responseType: '%1' (expected: %2)").arg(responseType).arg(RESPONSE_TYPES.join(" | ")));
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    callbackReady(Parent::loadFromCache(url, decompress, responseType), callback);
    return QVariant();
}

QVariant AssetScriptingInterface::saveToCache(const QVariant& options, const ScriptCallback& callback) {
    if (options.userType() != QMetaType::QVariantMap) {
        return verifyFailed(QString("expected options object as first parameter not: %1").arg(options.typeName()));
    }
    const QVariantMap map = options.toMap();
    return saveToCache(QUrl(map.value("url").toString()), bytesOf(map.value("data")), map.value("headers").toMap(), callback);
}

QVariant AssetScriptingInterface::saveToCache(const QUrl& rawURL, const QByteArray& data, const QVariantMap& metadata,
                                              const ScriptCallback& callback) {
    QUrl url = rawURL;
    if (url.path().isEmpty() && !data.isEmpty()) {
        // a valid ATP URL from the data, keeping any query or fragment
        auto atpURL = AssetUtils::getATPUrl(hashDataHex(data));
        atpURL.setQuery(url.query());
        atpURL.setFragment(url.fragment());
        url = atpURL;
    }
    auto hash = AssetUtils::extractAssetHash(url.toDisplayString());
    if (!url.isValid()) {
        return verifyFailed(QString("Invalid URL '%1'").arg(url.toString()));
    }
    if (!canWriteCacheValue(url)) {
        return verifyFailed("Invalid cache write URL: " + url.toString());
    }
    if (url.scheme() != "atp" && url.scheme() != "cache") {
        return verifyFailed("only 'atp' and 'cache' URL schemes supported");
    }
    if (!hash.isEmpty() && hash != hashDataHex(data)) {
        return verifyFailed(QString("invalid checksum hash for atp:HASH style URL (%1 != %2)").arg(hash, hashDataHex(data)));
    }
    if (!callback.isValid()) {
        return verifyFailed(NO_CALLBACK);
    }
    callbackReady(Parent::saveToCache(url, data, metadata), callback);
    return QVariant();
}

