//
//  AssetMappingsScriptingInterface.cpp
//  libraries/script-engine/src
//
//  Created by Ryan Huffman on 2016-03-09.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AssetMappingsScriptingInterface.h"

#include <QtCore/QFile>
#include <QtCore/QThread>
#include <QtQml/QJSEngine>

#include <AssetRequest.h>
#include <AssetUpload.h>
#include <MappingRequest.h>
#include <NetworkLogging.h>
#include <NodeList.h>
#include <OffscreenUi.h>

static const int AUTO_REFRESH_INTERVAL = 1000;

int assetMappingModelMetatypeId = qRegisterMetaType<AssetMappingModel*>("AssetMappingModel*");

AssetMappingsScriptingInterface::AssetMappingsScriptingInterface() {
    _proxyModel.setSourceModel(&_assetMappingModel);
    _proxyModel.setSortRole(Qt::DisplayRole);
    _proxyModel.setDynamicSortFilter(true);
    _proxyModel.setSortLocaleAware(true);
    _proxyModel.setFilterCaseSensitivity(Qt::CaseInsensitive);
    _proxyModel.sort(0);
}

void AssetMappingsScriptingInterface::setMapping(QString path, QString hash, QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createSetMappingRequest(path, hash);
    connect(request, &SetMappingRequest::finished, this, [callback](SetMappingRequest* request) mutable {
        if (callback.isCallable()) {
            QJSValueList args { request->getErrorString(), request->getPath() };
            callback.call(args);
        }

        request->deleteLater();
    });
    request->start();
}

void AssetMappingsScriptingInterface::getMapping(QString path, QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createGetMappingRequest(path);
    connect(request, &GetMappingRequest::finished, this, [callback](GetMappingRequest* request) mutable {
        auto hash = request->getHash();

        if (callback.isCallable()) {
            QJSValueList args { request->getErrorString(), hash };
            callback.call(args);
        }

        request->deleteLater();
    });

    request->start();
}

void AssetMappingsScriptingInterface::uploadFile(QString path, QString mapping, QJSValue startedCallback, QJSValue completedCallback, bool dropEvent) {
    // Called from the Asset Browser's QML, on the main thread, so the prompts get their words before they wait. A
    // dropped file and a chosen one get the same prompt.
    Q_UNUSED(dropEvent);
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    QQuickItem* prompt = offscreenUi->createInputDialog(OffscreenUi::ICON_INFORMATION, "Upload to the World's Assets",
                                                        "New folders are made for you.", mapping);
    if (prompt) {
        OffscreenUi::setButtonText(prompt, QMessageBox::Ok, "Upload");
        prompt->setProperty("monospace", true);
    }
    auto result = offscreenUi->waitForInputDialogResult(prompt);

    if (!result.isValid() || result.toString() == "") {
        completedCallback.call({ -1 });
        return;
    }
    mapping = result.toString();
    mapping = mapping.trimmed();

    if (mapping[0] != '/') {
        mapping = "/" + mapping;
    }

    // Check for override: only Replace replaces (Escape and closing the box keep the file)
    if (isKnownMapping(mapping)) {
        QQuickItem* box = offscreenUi->createMessageBox(OffscreenUi::ICON_QUESTION,
                                                        "Replace " + QFileInfo(mapping).fileName() + "?",
                                                        "A file with this name is already in the world's assets.",
                                                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (box) {
            OffscreenUi::setButtonText(box, QMessageBox::Yes, "Replace");
            box->setProperty("destructiveButton", (int)QMessageBox::Yes);
        }
        if (offscreenUi->waitForMessageBoxResult(box) != QMessageBox::Yes) {
            completedCallback.call({ -1 });
            return;
        }
    }

    startedCallback.call();

    auto upload = DependencyManager::get<AssetClient>()->createUpload(path);
    QObject::connect(upload, &AssetUpload::finished, this, [=](AssetUpload* upload, const QString& hash) mutable {
        if (upload->getError() != AssetUpload::NoError) {
            if (completedCallback.isCallable()) {
                QJSValueList args { upload->getErrorString() };
                completedCallback.call(args);
            }
        } else {
            setMapping(mapping, hash, completedCallback);
        }

        upload->deleteLater();
    });

    upload->start();
}

void AssetMappingsScriptingInterface::deleteMappings(QStringList paths, QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createDeleteMappingsRequest(paths);

    connect(request, &DeleteMappingsRequest::finished, this, [callback](DeleteMappingsRequest* request) mutable {
        if (callback.isCallable()) {
            QJSValueList args { request->getErrorString() };
            callback.call(args);
        }

        request->deleteLater();
    });

    request->start();
}

void AssetMappingsScriptingInterface::sortProxyModel(int column, Qt::SortOrder order) {
    _proxyModel.sort(column, order);
}

void AssetMappingsScriptingInterface::getAllMappings(QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createGetAllMappingsRequest();

    connect(request, &GetAllMappingsRequest::finished, this, [this, callback](GetAllMappingsRequest* request) mutable {
        auto mappings = request->getMappings();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QJSEngine* engine = qjsEngine(this);   // the engine this object is exposed to (Qt 6 has no QJSValue::engine())
#else
        QJSEngine* engine = callback.engine();
#endif
        if (!engine) {
            request->deleteLater();
            return;
        }
        auto map = engine->newObject();

        for (auto& kv : mappings ) {
            map.setProperty(kv.first, kv.second.hash);
        }

        if (callback.isCallable()) {
            QJSValueList args { request->getErrorString(), map };
            callback.call(args);
        }

        request->deleteLater();
    });

    request->start();
}

void AssetMappingsScriptingInterface::renameMapping(QString oldPath, QString newPath, QJSValue callback) {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createRenameMappingRequest(oldPath, newPath);

    connect(request, &RenameMappingRequest::finished, this, [callback](RenameMappingRequest* request) mutable {
        if (callback.isCallable()) {
            QJSValueList args{ request->getErrorString() };
            callback.call(args);
        }

        request->deleteLater();
    });

    request->start();
}

AssetMappingModel::AssetMappingModel() {
    setupRoles();

    connect(&_autoRefreshTimer, &QTimer::timeout, this, [this] {
        auto nodeList = DependencyManager::get<NodeList>();
        auto assetServer = nodeList->soloNodeOfType(NodeType::AssetServer);
        if (assetServer) {
            refresh();
        }
    });
    _autoRefreshTimer.setInterval(AUTO_REFRESH_INTERVAL);
}

bool AssetMappingModel::isAutoRefreshEnabled() {
    return _autoRefreshTimer.isActive();
}

void AssetMappingModel::setAutoRefreshEnabled(bool enabled) {
    if (enabled != _autoRefreshTimer.isActive()) {
        if (enabled) {
            _autoRefreshTimer.start();
        } else {
            _autoRefreshTimer.stop();
        }
    }
}

bool AssetMappingModel::isKnownFolder(QString path) const {
    if (!path.endsWith("/")) {
        return false;
    }

    auto existingPaths = _pathToItemMap.keys();
    for (auto& entry : existingPaths) {
        if (entry.startsWith(path)) {
            return true;
        }
    }
    return false;
}

void AssetMappingModel::refresh() {
    auto assetClient = DependencyManager::get<AssetClient>();
    auto request = assetClient->createGetAllMappingsRequest();

    connect(request, &GetAllMappingsRequest::finished, this, [this](GetAllMappingsRequest* request) mutable {
        if (request->getError() == MappingRequest::NoError) {
            auto mappings = request->getMappings();
            auto existingPaths = _pathToItemMap.keys();
            for (auto& mapping : mappings) {
                auto& path = mapping.first;

                if (path.startsWith(AssetUtils::HIDDEN_BAKED_CONTENT_FOLDER)) {
                    // Hide baked mappings
                    continue;
                }

                auto parts = path.split("/");
                auto length = parts.length();

                existingPaths.removeOne(mapping.first);

                QString fullPath = "/";

                QStandardItem* lastItem = nullptr;

                // start index at 1 to avoid empty string from leading slash
                for (int i = 1; i < length; ++i) {
                    fullPath += (i == 1 ? "" : "/") + parts[i];
                    bool isFolder = i < length - 1;

                    auto it = _pathToItemMap.find(fullPath);
                    if (it == _pathToItemMap.end()) {
                        auto item = new QStandardItem(parts[i]);
                        item->setData(isFolder ? fullPath + "/" : fullPath, Qt::UserRole);
                        item->setData(isFolder, Qt::UserRole + 1);
                        item->setData(parts[i], Qt::UserRole + 2);
                        item->setData("atp:" + fullPath, Qt::UserRole + 3);
                        item->setData(fullPath, Qt::UserRole + 4);

                        if (lastItem) {
                            lastItem->appendRow(item);
                        } else {
                            appendRow(item);
                        }
                        lastItem = item;
                        _pathToItemMap[fullPath] = lastItem;
                    } else {
                        lastItem = it.value();
                    }
                }

                Q_ASSERT(fullPath == path);
            }

            // Remove folders from list
            auto it = existingPaths.begin();
            while (it != existingPaths.end()) {
                Q_ASSERT(_pathToItemMap.contains(*it));
                auto item = _pathToItemMap[*it];
                if (item && item->data(Qt::UserRole + 1).toBool()) {
                    it = existingPaths.erase(it);
                } else {
                    ++it;
                }
            }

            for (auto& path : existingPaths) {
                Q_ASSERT(_pathToItemMap.contains(path));

                auto item = _pathToItemMap[path];

                while (item) {
                    // During each iteration, delete item
                    QStandardItem* nextItem = nullptr;

                    auto fullPath = item->data(Qt::UserRole + 4).toString();
                    auto parent = item->parent();
                    if (parent) {
                        parent->removeRow(item->row());
                        if (parent->rowCount() > 0) {
                            // The parent still contains children, set the nextItem to null so we stop processing
                            nextItem = nullptr;
                        } else {
                            nextItem = parent;
                        }
                    } else {
                        auto removed = removeRow(item->row());
                        Q_ASSERT(removed);
                    }

                    Q_ASSERT(_pathToItemMap.contains(fullPath));
                    _pathToItemMap.remove(fullPath);

                    item = nextItem;
                }
            }
        } else {
            emit errorGettingMappings(request->getErrorString());
        }

        emit updated();

        request->deleteLater();
    });

    request->start();
}

void AssetMappingModel::clear() {
    // make sure we are on the same thread before we touch the hash
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, "clear");
        return;
    }

    qDebug() << "Clearing loaded asset mappings for Asset Browser";

    _pathToItemMap.clear();
    QStandardItemModel::clear();
}

void AssetMappingModel::setupRoles() {
    QHash<int, QByteArray> roleNames;
    roleNames[Qt::DisplayRole] = "name";
    setItemRoleNames(roleNames);
}
