//
//  AssetServer.cpp
//  assignment-client/src/assets
//
//  Created by Ryan Huffman on 2015/07/21
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//


#include "AssetServer.h"

#include <thread>
#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtGui/QImageReader>
#include <QtCore/QVector>
#include <QtCore/QUrlQuery>

#include <RegexCompat.h>
#include <ClientServerUtils.h>
#include <NodeType.h>
#include <SharedUtil.h>
#include <PathUtils.h>

#include "AssetServerLogging.h"
#include "SendAssetTask.h"
#include "UploadAssetTask.h"

const QString ASSET_SERVER_LOGGING_TARGET_NAME = "asset-server";

QString AssetServer::getPathToAssetHash(const AssetUtils::AssetHash& assetHash) {
    return _filesDirectory.absoluteFilePath(assetHash);
}


AssetServer::AssetServer(ReceivedMessage& message) :
    ThreadedAssignment(message),
    _transferTaskPool(this),
    _filesizeLimit(AssetUtils::MAX_UPLOAD_SIZE)
{
    // Most of the work will be I/O bound, reading from disk and constructing packet objects,
    // so the ideal is greater than the number of cores on the system.
    static const int TASK_POOL_THREAD_COUNT = 50;
    _transferTaskPool.setMaxThreadCount(TASK_POOL_THREAD_COUNT);

    // Queue all requests until the Asset Server is fully setup
    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListenerForTypes({ PacketType::AssetGet, PacketType::AssetGetInfo, PacketType::AssetUpload, PacketType::AssetMappingOperation }, this, "queueRequests");
}

void AssetServer::aboutToFinish() {

    // remove pending transfer tasks
    _transferTaskPool.clear();
}

void AssetServer::run() {

    qCDebug(asset_server) << "Waiting for connection to domain to request settings from domain-server.";

    // wait until we have the domain-server settings, otherwise we bail
    DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    connect(&domainHandler, &DomainHandler::settingsReceived, this, &AssetServer::completeSetup);
    connect(&domainHandler, &DomainHandler::settingsReceiveFail, this, &AssetServer::domainSettingsRequestFailed);

    ThreadedAssignment::commonInit(ASSET_SERVER_LOGGING_TARGET_NAME, NodeType::AssetServer);
}

static const QString ASSET_FILES_SUBDIR = "files";

void AssetServer::completeSetup() {
    auto nodeList = DependencyManager::get<NodeList>();

    auto& domainHandler = nodeList->getDomainHandler();
    const QJsonObject& settingsObject = domainHandler.getSettingsObject();

    static const QString ASSET_SERVER_SETTINGS_KEY = "asset_server";

    if (!settingsObject.contains(ASSET_SERVER_SETTINGS_KEY)) {
        qCCritical(asset_server) << "Received settings from the domain-server with no asset-server section. Stopping assignment.";
        setFinished(true);
        return;
    }

    auto assetServerObject = settingsObject[ASSET_SERVER_SETTINGS_KEY].toObject();

    static const QString MAX_BANDWIDTH_OPTION = "max_bandwidth";
    auto maxBandwidthValue = assetServerObject[MAX_BANDWIDTH_OPTION];
    auto maxBandwidthFloat = maxBandwidthValue.toDouble(-1);

    const int BITS_PER_MEGABITS = 1000 * 1000;
    if (maxBandwidthFloat > 0.0) {
        int maxBandwidth = maxBandwidthFloat * BITS_PER_MEGABITS;
        nodeList->setConnectionMaxBandwidth(maxBandwidth);
        qCInfo(asset_server) << "Set maximum bandwith per connection to" << maxBandwidthFloat << "Mb/s."
                    " (" << maxBandwidth << "bits/s)";
    }

    // get the path to the asset folder from the domain server settings
    static const QString ASSETS_PATH_OPTION = "assets_path";
    auto assetsJSONValue = assetServerObject[ASSETS_PATH_OPTION];

    if (!assetsJSONValue.isString()) {
        qCCritical(asset_server) << "Received an assets path from the domain-server that could not be parsed. Stopping assignment.";
        setFinished(true);
        return;
    }

    auto assetsPathString = assetsJSONValue.toString();
    QDir assetsPath { assetsPathString };
    QString absoluteFilePath = assetsPath.absolutePath();

    if (assetsPath.isRelative()) {
        // if the domain settings passed us a relative path, make an absolute path that is relative to the
        // default data directory
        absoluteFilePath = PathUtils::getAppDataFilePath("assets/" + assetsPathString);
    }

    _resourcesDirectory = QDir(absoluteFilePath);

    qCDebug(asset_server) << "Creating resources directory";
    _resourcesDirectory.mkpath(".");
    _filesDirectory = _resourcesDirectory;

    if (!_resourcesDirectory.mkpath(ASSET_FILES_SUBDIR) || !_filesDirectory.cd(ASSET_FILES_SUBDIR)) {
        qCCritical(asset_server) << "Unable to create file directory for asset-server files. Stopping assignment.";
        setFinished(true);
        return;
    }

    // load whatever mappings we currently have from the local file
    if (loadMappingsFromFile()) {
        qCInfo(asset_server) << "Serving files from: " << _filesDirectory.path();

        // Check the asset directory to output some information about what we have
        auto files = _filesDirectory.entryList(QDir::Files);

        // QStringList::filter keeps the names the pattern is found in, as it did with QRegExp
        auto hashedFiles = files.filter(regex_compat::search(AssetUtils::ASSET_HASH_REGEX_STRING));

        qCInfo(asset_server) << "There are" << hashedFiles.size() << "asset files in the asset directory.";

        if (_fileMappings.size() > 0) {
            cleanupUnmappedFiles();
            cleanupBakedFilesForDeletedAssets();
        }

        // the physics server too: it fetches a world's terrain at an atp: URL, and a node the asset server isn't told
        // about can't complete its handshake (its packets come from an unknown node and are dropped)
        nodeList->addSetOfNodeTypesToNodeInterestSet({ NodeType::Agent, NodeType::EntityScriptServer, NodeType::PhysicsServer });
    } else {
        qCCritical(asset_server) << "Asset Server assignment will not continue because mapping file could not be loaded.";
        setFinished(true);
    }

    // get file size limit for an asset
    static const QString ASSETS_FILESIZE_LIMIT_OPTION = "assets_filesize_limit";
    auto assetsFilesizeLimitJSONValue = assetServerObject[ASSETS_FILESIZE_LIMIT_OPTION];
    auto assetsFilesizeLimit = (uint64_t)assetsFilesizeLimitJSONValue.toInt(AssetUtils::MAX_UPLOAD_SIZE);

    if (assetsFilesizeLimit != 0 && assetsFilesizeLimit < AssetUtils::MAX_UPLOAD_SIZE) {
        _filesizeLimit = assetsFilesizeLimit * BITS_PER_MEGABITS;
    }

    PathUtils::removeTemporaryApplicationDirs();

    qCDebug(asset_server) << "Overriding temporary queuing packet handler.";
    // We're fully setup, override the request queueing handler and replay all requests
    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::AssetGet, this, "handleAssetGet");
    packetReceiver.registerListener(PacketType::AssetGetInfo, this, "handleAssetGetInfo");
    packetReceiver.registerListener(PacketType::AssetUpload, this, "handleAssetUpload");
    packetReceiver.registerListener(PacketType::AssetMappingOperation, this, "handleAssetMappingOperation");

    replayRequests();
}

void AssetServer::queueRequests(QSharedPointer<ReceivedMessage> packet, SharedNodePointer senderNode) {
    qCDebug(asset_server) << "Queuing requests until fully setup";

    QMutexLocker lock { &_queuedRequestsMutex };
    _queuedRequests.push_back({ packet, senderNode });

    // If we've stopped queueing but the callback was already in flight,
    // then replay it immediately.
    if (!_isQueueingRequests) {
        lock.unlock();
        replayRequests();
    }
}

void AssetServer::replayRequests() {
    RequestQueue queue;
    {
        QMutexLocker lock { &_queuedRequestsMutex };
        std::swap(queue, _queuedRequests);
        _isQueueingRequests = false;
    }

    qCDebug(asset_server) << "Replaying" << queue.size() << "requests.";

    for (const auto& request : queue) {
        switch (request.first->getType()) {
            case PacketType::AssetGet:
                handleAssetGet(request.first, request.second);
                break;
            case PacketType::AssetGetInfo:
                handleAssetGetInfo(request.first, request.second);
                break;
            case PacketType::AssetUpload:
                handleAssetUpload(request.first, request.second);
                break;
            case PacketType::AssetMappingOperation:
                handleAssetMappingOperation(request.first, request.second);
                break;
            default:
                qCWarning(asset_server) << "Unknown queued request type:" << request.first->getType();
                break;
        }
    }
}

void AssetServer::cleanupUnmappedFiles() {
    static const QRegularExpression hashFileRegex = regex_compat::exact(AssetUtils::ASSET_HASH_REGEX_STRING);

    auto files = _filesDirectory.entryInfoList(QDir::Files);

    qCInfo(asset_server) << "Performing unmapped asset cleanup.";

    for (const auto& fileInfo : files) {
        auto filename = fileInfo.fileName();
        if (hashFileRegex.match(filename).hasMatch()) {
            bool matched { false };
            for (auto& pair : _fileMappings) {
                if (pair.second == filename) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                // remove the unmapped file
                QFile removeableFile { fileInfo.absoluteFilePath() };

                if (removeableFile.remove()) {
                    qCDebug(asset_server) << "\tDeleted" << filename << "from asset files directory since it is unmapped.";

                    removeBakedPathsForDeletedAsset(filename);
                } else {
                    qCDebug(asset_server) << "\tAttempt to delete unmapped file" << filename << "failed";
                }
            }
        }
    }
}

void AssetServer::cleanupBakedFilesForDeletedAssets() {
    qCInfo(asset_server) << "Performing baked asset cleanup for deleted assets";

    std::set<AssetUtils::AssetHash> bakedHashes;

    for (const auto& it : _fileMappings) {
        // check if this is a mapping to baked content
        if (it.first.startsWith(AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER)) {
            // extract the hash from the baked mapping
            AssetUtils::AssetHash hash = it.first.mid(AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER.length(),
                                                      AssetUtils::SHA256_HASH_HEX_LENGTH);

            // add the hash to our set of hashes for which we have baked content
            bakedHashes.insert(hash);
        }
    }

    // enumerate the hashes for which we have baked content
    for (const auto& hash : bakedHashes) {
        // check if we have a mapping that points to this hash
        auto matchingMapping = std::find_if(std::begin(_fileMappings), std::end(_fileMappings),
                                            [&hash](const std::pair<AssetUtils::AssetPath, AssetUtils::AssetHash> mappingPair) {
            return mappingPair.second == hash;
        });

        if (matchingMapping == std::end(_fileMappings)) {
            // we didn't find a mapping for this hash, remove any baked content we still have for it
            removeBakedPathsForDeletedAsset(hash);
        }
    }
}

void AssetServer::handleAssetMappingOperation(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    using AssetMappingOperationType = AssetUtils::AssetMappingOperationType;

    MessageID messageID;
    message->readPrimitive(&messageID);

    AssetMappingOperationType operationType;
    message->readPrimitive(&operationType);

    auto replyPacket = NLPacketList::create(PacketType::AssetMappingOperationReply, QByteArray(), true, true);
    replyPacket->writePrimitive(messageID);

    bool canWriteToAssetServer = true;
    if (senderNode) {
        canWriteToAssetServer = senderNode->getCanWriteToAssetServer();
    }

    switch (operationType) {
        case AssetMappingOperationType::Get:
            handleGetMappingOperation(*message, *replyPacket);
            break;
        case AssetMappingOperationType::GetAll:
            handleGetAllMappingOperation(*replyPacket);
            break;
        case AssetMappingOperationType::Set:
            handleSetMappingOperation(*message, canWriteToAssetServer, *replyPacket);
            break;
        case AssetMappingOperationType::Delete:
            handleDeleteMappingsOperation(*message, canWriteToAssetServer, *replyPacket);
            break;
        case AssetMappingOperationType::Rename:
            handleRenameMappingOperation(*message, canWriteToAssetServer, *replyPacket);
            break;
        case AssetMappingOperationType::SetBakingEnabled:
            handleSetBakingEnabledOperation(*message, canWriteToAssetServer, *replyPacket);
            break;
    }

    auto nodeList = DependencyManager::get<NodeList>();
    if (senderNode) {
        nodeList->sendPacketList(std::move(replyPacket), *senderNode);
    } else {
        nodeList->sendPacketList(std::move(replyPacket), message->getSenderSockAddr());
    }
}

void AssetServer::handleGetMappingOperation(ReceivedMessage& message, NLPacketList& replyPacket) {
    QString assetPath = message.readString();

    QUrl url { assetPath };
    assetPath = url.path();

    auto it = _fileMappings.find(assetPath);
    if (it != _fileMappings.end()) {
        replyPacket.writePrimitive(AssetUtils::AssetServerError::NoError);
        replyPacket.write(QByteArray::fromHex(it->second.toUtf8()));
        quint8 wasRedirected = false;   // baked content was served by a redirect; nothing is baked any more
        replyPacket.writePrimitive(wasRedirected);
    } else {
        replyPacket.writePrimitive(AssetUtils::AssetServerError::AssetNotFound);
    }
}

void AssetServer::handleGetAllMappingOperation(NLPacketList& replyPacket) {
    replyPacket.writePrimitive(AssetUtils::AssetServerError::NoError);

    uint32_t count = (uint32_t)_fileMappings.size();

    replyPacket.writePrimitive(count);

    for (auto it = _fileMappings.cbegin(); it != _fileMappings.cend(); ++ it) {
        auto mapping = it->first;
        auto hash = it->second;
        replyPacket.writeString(mapping);
        replyPacket.write(QByteArray::fromHex(hash.toUtf8()));

        // the listing's format keeps a baking status per mapping: nothing is baked any more
        replyPacket.writePrimitive(AssetUtils::Irrelevant);
    }
}

void AssetServer::handleSetMappingOperation(ReceivedMessage& message, bool hasWriteAccess, NLPacketList& replyPacket) {
    if (hasWriteAccess) {
        QString assetPath = message.readString();

        auto assetHash = message.read(AssetUtils::SHA256_HASH_LENGTH).toHex();

        // don't process a set mapping operation that is inside the hidden baked folder
        if (assetPath.startsWith(AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER)) {
            qCDebug(asset_server) << "Refusing to process a set mapping operation inside" << AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER;
            replyPacket.writePrimitive(AssetUtils::AssetServerError::PermissionDenied);
        } else {
            if (setMapping(assetPath, assetHash)) {
                replyPacket.writePrimitive(AssetUtils::AssetServerError::NoError);
            } else {
                replyPacket.writePrimitive(AssetUtils::AssetServerError::MappingOperationFailed);
            }
        }

    } else {
        replyPacket.writePrimitive(AssetUtils::AssetServerError::PermissionDenied);
    }
}

void AssetServer::handleDeleteMappingsOperation(ReceivedMessage& message, bool hasWriteAccess, NLPacketList& replyPacket) {
    if (hasWriteAccess) {
        int numberOfDeletedMappings { 0 };
        message.readPrimitive(&numberOfDeletedMappings);

        QStringList mappingsToDelete;

        // as many names as the message holds
        for (int i = 0; i < numberOfDeletedMappings && message.getBytesLeftToRead() > 0; ++i) {
            auto mapping = message.readString();

            if (!mapping.startsWith(AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER)) {
                mappingsToDelete << mapping;
            } else {
                qCDebug(asset_server) << "Refusing to delete mapping" << mapping
                    << "that is inside" << AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER;
            }
        }

        if (deleteMappings(mappingsToDelete)) {
            replyPacket.writePrimitive(AssetUtils::AssetServerError::NoError);
        } else {
            replyPacket.writePrimitive(AssetUtils::AssetServerError::MappingOperationFailed);
        }
    } else {
        replyPacket.writePrimitive(AssetUtils::AssetServerError::PermissionDenied);
    }
}

void AssetServer::handleRenameMappingOperation(ReceivedMessage& message, bool hasWriteAccess, NLPacketList& replyPacket) {
    if (hasWriteAccess) {
        QString oldPath = message.readString();
        QString newPath = message.readString();

        if (oldPath.startsWith(AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER) || newPath.startsWith(AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER)) {
            qCDebug(asset_server) << "Cannot rename" << oldPath << "to" << newPath
                << "since one of the paths is inside" << AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER;
            replyPacket.writePrimitive(AssetUtils::AssetServerError::PermissionDenied);
        } else {
            if (renameMapping(oldPath, newPath)) {
                replyPacket.writePrimitive(AssetUtils::AssetServerError::NoError);
            } else {
                replyPacket.writePrimitive(AssetUtils::AssetServerError::MappingOperationFailed);
            }
        }

    } else {
        replyPacket.writePrimitive(AssetUtils::AssetServerError::PermissionDenied);
    }
}

void AssetServer::handleSetBakingEnabledOperation(ReceivedMessage& message, bool hasWriteAccess, NLPacketList& replyPacket) {
    // the asset server no longer bakes; a client that still asks is told the operation failed (nothing of the request
    // is used)
    Q_UNUSED(message);
    replyPacket.writePrimitive(hasWriteAccess ? AssetUtils::AssetServerError::MappingOperationFailed
                                              : AssetUtils::AssetServerError::PermissionDenied);
}

void AssetServer::handleAssetGetInfo(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    QByteArray assetHash;
    MessageID messageID;

    if (message->getSize() < qint64(AssetUtils::SHA256_HASH_LENGTH + sizeof(messageID))) {
        qCDebug(asset_server) << "ERROR bad file request";
        return;
    }

    message->readPrimitive(&messageID);
    assetHash = message->readWithoutCopy(AssetUtils::SHA256_HASH_LENGTH);

    auto size = qint64(sizeof(MessageID) + AssetUtils::SHA256_HASH_LENGTH + sizeof(AssetUtils::AssetServerError) + sizeof(qint64));
    auto replyPacket = NLPacket::create(PacketType::AssetGetInfoReply, size, true);

    QByteArray hexHash = assetHash.toHex();

    replyPacket->writePrimitive(messageID);
    replyPacket->write(assetHash);

    QString fileName = QString(hexHash);
    QFileInfo fileInfo { _filesDirectory.filePath(fileName) };

    if (fileInfo.exists() && fileInfo.isReadable()) {
        qCDebug(asset_server) << "Opening file: " << fileInfo.filePath();
        replyPacket->writePrimitive(AssetUtils::AssetServerError::NoError);
        replyPacket->writePrimitive(fileInfo.size());
    } else {
        qCDebug(asset_server) << "Asset not found: " << QString(hexHash);
        replyPacket->writePrimitive(AssetUtils::AssetServerError::AssetNotFound);
    }

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->sendPacket(std::move(replyPacket), *senderNode);
}

void AssetServer::handleAssetGet(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {

    auto minSize = qint64(sizeof(MessageID) + AssetUtils::SHA256_HASH_LENGTH + sizeof(AssetUtils::DataOffset) + sizeof(AssetUtils::DataOffset));

    if (message->getSize() < minSize) {
        qCDebug(asset_server) << "ERROR bad file request";
        return;
    }

    // Queue task
    auto task = new SendAssetTask(message, senderNode, _filesDirectory);
    _transferTaskPool.start(task);
}

void AssetServer::handleAssetUpload(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    bool canWriteToAssetServer = true;
    if (senderNode) {
        canWriteToAssetServer = senderNode->getCanWriteToAssetServer();
    }


    if (canWriteToAssetServer) {
        qCDebug(asset_server) << "Starting an UploadAssetTask for upload from" << message->getSourceID();

        auto task = new UploadAssetTask(message, senderNode, _filesDirectory, _filesizeLimit);
        _transferTaskPool.start(task);
    } else {
        // this is a node the domain told us is not allowed to rez entities
        // for now this also means it isn't allowed to add assets
        // so return a packet with error that indicates that

        auto permissionErrorPacket = NLPacket::create(PacketType::AssetUploadReply, sizeof(MessageID) + sizeof(AssetUtils::AssetServerError), true);

        MessageID messageID;
        message->readPrimitive(&messageID);

        // write the message ID and a permission denied error
        permissionErrorPacket->writePrimitive(messageID);
        permissionErrorPacket->writePrimitive(AssetUtils::AssetServerError::PermissionDenied);

        // send off the packet
        auto nodeList = DependencyManager::get<NodeList>();
        if (senderNode) {
            nodeList->sendPacket(std::move(permissionErrorPacket), *senderNode);
        } else {
            nodeList->sendPacket(std::move(permissionErrorPacket), message->getSenderSockAddr());
        }
    }
}

void AssetServer::sendStatsPacket() {
    QJsonObject serverStats;

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->eachNode([&](auto& node) {
        auto& stats = node->getConnectionStats();

        QJsonObject nodeStats;
        auto endTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(stats.endTime);
        QDateTime date = QDateTime::fromMSecsSinceEpoch(endTimeMs.count());

        static const float USEC_PER_SEC = 1000000.0f;
        static const float MEGABITS_PER_BYTE = 8.0f / 1000000.0f; // Bytes => Mbits
        float elapsed = (float)(stats.endTime - stats.startTime).count() / USEC_PER_SEC; // sec
        float megabitsPerSecPerByte = MEGABITS_PER_BYTE / elapsed; // Bytes => Mb/s

        QJsonObject connectionStats;
        connectionStats["1. Last Heard"] = date.toString();
        connectionStats["2. Est. Max (P/s)"] = stats.estimatedBandwith;
        connectionStats["3. RTT (ms)"] = stats.rtt;
        connectionStats["4. CW (P)"] = stats.congestionWindowSize;
        connectionStats["5. Period (us)"] = stats.packetSendPeriod;
        connectionStats["6. Up (Mb/s)"] = stats.sentBytes * megabitsPerSecPerByte;
        connectionStats["7. Down (Mb/s)"] = stats.receivedBytes * megabitsPerSecPerByte;
        nodeStats["Connection Stats"] = connectionStats;

        using Events = udt::ConnectionStats::Stats::Event;
        const auto& events = stats.events;

        QJsonObject upstreamStats;
        upstreamStats["1. Sent (P/s)"] = stats.sendRate;
        upstreamStats["2. Sent Packets"] = (int)stats.sentPackets;
        upstreamStats["3. Recvd ACK"] = events[Events::ReceivedACK];
        upstreamStats["4. Procd ACK"] = events[Events::ProcessedACK];
        upstreamStats["5. Retransmitted"] = (int)stats.retransmittedPackets;
        nodeStats["Upstream Stats"] = upstreamStats;

        QJsonObject downstreamStats;
        downstreamStats["1. Recvd (P/s)"] = stats.receiveRate;
        downstreamStats["2. Recvd Packets"] = (int)stats.receivedPackets;
        downstreamStats["3. Sent ACK"] = events[Events::SentACK];
        downstreamStats["4. Duplicates"] = (int)stats.duplicatePackets;
        nodeStats["Downstream Stats"] = downstreamStats;

        QString uuid = uuidStringWithoutCurlyBraces(node->getUUID());
        nodeStats[USERNAME_UUID_REPLACEMENT_STATS_KEY] = uuid;

        serverStats[uuid] = nodeStats;
    });

    // send off the stats packets
    ThreadedAssignment::addPacketStatsAndSendStatsPacket(serverStats);
}

static const QString MAP_FILE_NAME = "map.json";

bool AssetServer::loadMappingsFromFile() {

    auto mapFilePath = _resourcesDirectory.absoluteFilePath(MAP_FILE_NAME);

    QFile mapFile { mapFilePath };
    if (mapFile.exists()) {
        if (mapFile.open(QIODevice::ReadOnly)) {
            QJsonParseError error;

            auto jsonDocument = QJsonDocument::fromJson(mapFile.readAll(), &error);

            if (error.error == QJsonParseError::NoError) {
                if (!jsonDocument.isObject()) {
                    qCWarning(asset_server) << "Failed to read mapping file, root value in" << mapFilePath << "is not an object";
                    return false;
                }

                //_fileMappings = jsonDocument.object().toVariantHash();
                auto root = jsonDocument.object();
                for (auto it = root.begin(); it != root.end(); ++it) {
                    auto key = it.key();
                    auto value = it.value();

                    if (!value.isString()) {
                        qCWarning(asset_server) << "Skipping" << key << ":" << value << "because it is not a string";
                        continue;
                    }

                    if (!AssetUtils::isValidFilePath(key)) {
                        qCWarning(asset_server) << "Will not keep mapping for" << key << "since it is not a valid path.";
                        continue;
                    }

                    if (!AssetUtils::isValidHash(value.toString())) {
                        qCWarning(asset_server) << "Will not keep mapping for" << key << "since it does not have a valid hash.";
                        continue;
                    }


                    qDebug() << "Added " << key << value.toString();
                    _fileMappings[key] = value.toString();
                }

                qCInfo(asset_server) << "Loaded" << _fileMappings.size() << "mappings from map file at" << mapFilePath;
                return true;
            }
        }

        qCCritical(asset_server) << "Failed to read mapping file at" << mapFilePath;
        return false;
    } else {
        qCInfo(asset_server) << "No existing mappings loaded from file since no file was found at" << mapFilePath;
    }

    return true;
}

bool AssetServer::writeMappingsToFile() {
    auto mapFilePath = _resourcesDirectory.absoluteFilePath(MAP_FILE_NAME);

    QSaveFile mapFile { mapFilePath };
    if (mapFile.open(QIODevice::WriteOnly)) {
        QJsonObject root;

        for (auto it : _fileMappings) {
            root[it.first] = it.second;
        }

        QJsonDocument jsonDocument { root };

        if (mapFile.write(jsonDocument.toJson()) != -1) {
            if (mapFile.commit()) {
                qCDebug(asset_server) << "Wrote JSON mappings to file at" << mapFilePath;
                return true;
            } else {
                qCWarning(asset_server) << "Failed to commit JSON mappings to file at" << mapFilePath;
            }
        } else {
            qCWarning(asset_server) << "Failed to write JSON mappings to file at" << mapFilePath;
        }
    } else {
        qCWarning(asset_server) << "Failed to open map file at" << mapFilePath;
    }

    return false;
}

bool AssetServer::setMapping(AssetUtils::AssetPath path, AssetUtils::AssetHash hash) {
    path = path.trimmed();

    if (!AssetUtils::isValidFilePath(path)) {
        qCWarning(asset_server) << "Cannot set a mapping for invalid path:" << path << "=>" << hash;
        return false;
    }

    if (!AssetUtils::isValidHash(hash)) {
        qCWarning(asset_server) << "Cannot set a mapping for invalid hash" << path << "=>" << hash;
        return false;
    }

    // remember what the old mapping was in case persistence fails
    auto it = _fileMappings.find(path);
    auto oldMapping = it != _fileMappings.end() ? it->second : "";

    // update the in memory QHash
    _fileMappings[path] = hash;

    // attempt to write to file
    if (writeMappingsToFile()) {
        // persistence succeeded, we are good to go
        qCDebug(asset_server) << "Set mapping:" << path << "=>" << hash;
        return true;
    } else {
        // failed to persist this mapping to file - put back the old one in our in-memory representation
        if (oldMapping.isEmpty()) {
            _fileMappings.erase(_fileMappings.find(path));
        } else {
            _fileMappings[path] = oldMapping;
        }

        qCWarning(asset_server) << "Failed to persist mapping:" << path << "=>" << hash;

        return false;
    }
}

bool pathIsFolder(const AssetUtils::AssetPath& path) {
    return path.endsWith('/');
}

void AssetServer::removeBakedPathsForDeletedAsset(AssetUtils::AssetHash hash) {
    // we deleted the file with this hash

    // check if we had baked content for that file that should also now be removed
    // by calling deleteMappings for the hidden baked content folder for this hash
    AssetUtils::AssetPathList hiddenBakedFolder { AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER + hash + "/" };

    qCDebug(asset_server) << "Deleting baked content below" << hiddenBakedFolder << "since" << hash << "was deleted";

    deleteMappings(hiddenBakedFolder);
}

bool AssetServer::deleteMappings(const AssetUtils::AssetPathList& paths) {
    // take a copy of the current mappings in case persistence of these deletes fails
    auto oldMappings = _fileMappings;

    QSet<QString> hashesToCheckForDeletion;

    // enumerate the paths to delete and remove them all
    for (const auto& rawPath : paths) {
        auto path = rawPath.trimmed();

        // figure out if this path will delete a file or folder
        if (pathIsFolder(path)) {
            // enumerate the in memory file mappings and remove anything that matches
            auto it = _fileMappings.begin();
            auto sizeBefore = _fileMappings.size();

            while (it != _fileMappings.end()) {
                if (it->first.startsWith(path)) {
                    // add this hash to the list we need to check for asset removal from the server
                    hashesToCheckForDeletion << it->second;

                    it = _fileMappings.erase(it);
                } else {
                    ++it;
                }
            }

            auto sizeNow = _fileMappings.size();
            if (sizeBefore != sizeNow) {
                qCDebug(asset_server) << "Deleted" << sizeBefore - sizeNow << "mappings in folder: " << path;
            } else {
                qCDebug(asset_server) << "Did not find any mappings to delete in folder:" << path;
            }

        } else {
            auto it = _fileMappings.find(path);
            if (it != _fileMappings.end()) {
                // add this hash to the list we need to check for asset removal from server
                hashesToCheckForDeletion << it->second;

                qCDebug(asset_server) << "Deleted a mapping:" << path << "=>" << it->second;
                
                _fileMappings.erase(it);
            } else {
                qCDebug(asset_server) << "Unable to delete a mapping that was not found:" << path;
            }
        }
    }

    // deleted the old mappings, attempt to persist to file
    if (writeMappingsToFile()) {
        // persistence succeeded we are good to go

        // TODO iterate through hashesToCheckForDeletion instead
        for (auto& pair : _fileMappings) {
            auto it = hashesToCheckForDeletion.find(pair.second);
            if (it != hashesToCheckForDeletion.end()) {
                hashesToCheckForDeletion.erase(it);
            }
        }

        // we now have a set of hashes that are unmapped - we will delete those asset files
        for (auto& hash : hashesToCheckForDeletion) {
            // remove the unmapped file
            QFile removeableFile { _filesDirectory.absoluteFilePath(hash) };

            if (removeableFile.remove()) {
                qCDebug(asset_server) << "\tDeleted" << hash << "from asset files directory since it is now unmapped.";

                removeBakedPathsForDeletedAsset(hash);
            } else {
                qCDebug(asset_server) << "\tAttempt to delete unmapped file" << hash << "failed";
            }
        }

        return true;
    } else {
        qCWarning(asset_server) << "Failed to persist deleted mappings, rolling back";

        // we didn't delete the previous mapping, put it back in our in-memory representation
        _fileMappings = oldMappings;

        return false;
    }
}

bool AssetServer::renameMapping(AssetUtils::AssetPath oldPath, AssetUtils::AssetPath newPath) {
    oldPath = oldPath.trimmed();
    newPath = newPath.trimmed();

    if (!AssetUtils::isValidFilePath(oldPath) || !AssetUtils::isValidFilePath(newPath)) {
        qCWarning(asset_server) << "Cannot perform rename with invalid paths - both should have leading forward and no ending slashes:"
            << oldPath << "=>" << newPath;

        return false;
    }

    // figure out if this rename is for a file or folder
    if (pathIsFolder(oldPath)) {
        if (!pathIsFolder(newPath)) {
            // we were asked to rename a path to a folder to a path that isn't a folder, this is a fail
            qCWarning(asset_server) << "Cannot rename mapping from folder path" << oldPath << "to file path" << newPath;

            return false;
        }

        // take a copy of the old mappings
        auto oldMappings = _fileMappings;

        // iterate the current mappings and adjust any that matches the renamed folder
        auto it = oldMappings.begin();

        while (it != oldMappings.end()) {
            auto& oldKey = it->first;
            if (oldKey.startsWith(oldPath)) {
                auto newKey = oldKey;
                newKey.replace(0, oldPath.size(), newPath);

                // remove the old version from the in memory file mappings
                _fileMappings.erase(_fileMappings.find(oldKey));
                _fileMappings[newKey] = it->second;
            }

            ++it;
        }

        if (writeMappingsToFile()) {
            // persisted the changed mappings, return success
            qCDebug(asset_server) << "Renamed folder mapping:" << oldPath << "=>" << newPath;

            return true;
        } else {
            // couldn't persist the renamed paths, rollback and return failure
            _fileMappings = oldMappings;

            qCWarning(asset_server) << "Failed to persist renamed folder mapping:" << oldPath << "=>" << newPath;

            return false;
        }
    } else {
        if (pathIsFolder(newPath)) {
            // we were asked to rename a path to a file to a path that is a folder, this is a fail
            qCWarning(asset_server) << "Cannot rename mapping from file path" << oldPath << "to folder path" << newPath;

            return false;
        }

        // take the old hash to remove the old mapping
        auto it = _fileMappings.find(oldPath);
        auto oldSourceMapping = it->second;
        _fileMappings.erase(it);

        // in case we're overwriting, keep the current destination mapping for potential rollback
        auto oldDestinationIt = _fileMappings.find(newPath);

        if (!oldSourceMapping.isEmpty()) {
            _fileMappings[newPath] = oldSourceMapping;

            if (writeMappingsToFile()) {
                // persisted the renamed mapping, return success
                qCDebug(asset_server) << "Renamed mapping:" << oldPath << "=>" << newPath;

                return true;
            } else {
                // we couldn't persist the renamed mapping, rollback and return failure
                _fileMappings[oldPath] = oldSourceMapping;

                if (oldDestinationIt != _fileMappings.end()) {
                    // put back the overwritten mapping for the destination path
                    _fileMappings[newPath] = oldDestinationIt->second;
                } else {
                    // clear the new mapping
                    _fileMappings.erase(_fileMappings.find(newPath));
                }

                qCDebug(asset_server) << "Failed to persist renamed mapping:" << oldPath << "=>" << newPath;

                return false;
            }
        } else {
            // failed to find a mapping that was to be renamed, return failure
            return false;
        }
    }
}


