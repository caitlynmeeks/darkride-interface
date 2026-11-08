//
//  AssetMappingsScriptingInterface.h
//  libraries/script-engine/src
//
//  Created by Ryan Huffman on 2016-03-09.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_AssetMappingsScriptingInterface_h
#define hifi_AssetMappingsScriptingInterface_h

#include <functional>

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtQml/QJSEngine>
#include <QtQml/QJSValue>

#include <AssetClient.h>
#include <QSortFilterProxyModel>

#include "DependencyManager.h"


class AssetMappingModel : public QStandardItemModel {
    Q_OBJECT
    Q_PROPERTY(bool autoRefreshEnabled READ isAutoRefreshEnabled WRITE setAutoRefreshEnabled)

public:
    AssetMappingModel();

    Q_INVOKABLE void refresh();

    bool isAutoRefreshEnabled();
    void setAutoRefreshEnabled(bool enabled);

    bool isKnownMapping(QString path) const { return _pathToItemMap.contains(path); }
    bool isKnownFolder(QString path) const;

public slots:
    void clear();

signals:
    void errorGettingMappings(QString errorString);
    void updated();

private:
    void setupRoles();

    QHash<QString, QStandardItem*> _pathToItemMap;
    QTimer _autoRefreshTimer;
};

Q_DECLARE_METATYPE(AssetMappingModel*)

class AssetMappingsScriptingInterface : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    Q_PROPERTY(AssetMappingModel* mappingModel READ getAssetMappingModel CONSTANT)
    Q_PROPERTY(QAbstractProxyModel* proxyModel READ getProxyModel CONSTANT)
public:
    AssetMappingsScriptingInterface();

    Q_INVOKABLE AssetMappingModel* getAssetMappingModel() { return &_assetMappingModel; }
    Q_INVOKABLE QAbstractProxyModel* getProxyModel() { return &_proxyModel; }

    Q_INVOKABLE bool isKnownMapping(QString path) const { return _assetMappingModel.isKnownMapping(path); }
    Q_INVOKABLE bool isKnownFolder(QString path) const { return _assetMappingModel.isKnownFolder(path); }

    Q_INVOKABLE void setMapping(QString path, QString hash, QJSValue callback = QJSValue());
    Q_INVOKABLE void getMapping(QString path, QJSValue callback = QJSValue());
    Q_INVOKABLE void uploadFile(QString path, QString mapping, QJSValue startedCallback = QJSValue(), QJSValue completedCallback = QJSValue(), bool dropEvent = false);
    Q_INVOKABLE void deleteMappings(QStringList paths, QJSValue callback = QJSValue());
    Q_INVOKABLE void deleteMapping(QString path, QJSValue callback) { deleteMappings(QStringList(path), callback = QJSValue()); }
    /// Every mapping of the world's files, path -> hash, with the error's words (empty when none)
    void getAllMappings(std::function<void(const QString& error, const QJsonObject& mappings)> done);
    Q_INVOKABLE void renameMapping(QString oldPath, QString newPath, QJSValue callback = QJSValue());
    Q_INVOKABLE void sortProxyModel(int column, Qt::SortOrder order = Qt::AscendingOrder);

protected:
    QSet<AssetRequest*> _pendingRequests;
    AssetMappingModel _assetMappingModel;
    QSortFilterProxyModel _proxyModel;
};


// The Assets object a QML context sees: one per context, holding that context's engine, so an answer built here (the
// mappings object of getAllMappings) is made in the engine of the callback that receives it. The one shared object
// above wrapped by the first surface that saw it gave Qt 6 a callback from another surface's engine, which Qt 6
// refuses, so the asset picker on the other surface never listed anything. The models and the requests stay shared.
class AssetMappingsQmlInterface : public QObject {
    Q_OBJECT
    Q_PROPERTY(AssetMappingModel* mappingModel READ getAssetMappingModel CONSTANT)
    Q_PROPERTY(QAbstractProxyModel* proxyModel READ getProxyModel CONSTANT)

public:
    /// engine: the QML engine of the context this object is set on; parent: that context
    AssetMappingsQmlInterface(QJSEngine* engine, QObject* parent);

    Q_INVOKABLE AssetMappingModel* getAssetMappingModel() { return shared()->getAssetMappingModel(); }
    Q_INVOKABLE QAbstractProxyModel* getProxyModel() { return shared()->getProxyModel(); }
    Q_INVOKABLE bool isKnownMapping(QString path) const { return shared()->isKnownMapping(path); }
    Q_INVOKABLE bool isKnownFolder(QString path) const { return shared()->isKnownFolder(path); }
    Q_INVOKABLE void setMapping(QString path, QString hash, QJSValue callback = QJSValue()) { shared()->setMapping(path, hash, callback); }
    Q_INVOKABLE void getMapping(QString path, QJSValue callback = QJSValue()) { shared()->getMapping(path, callback); }
    Q_INVOKABLE void uploadFile(QString path, QString mapping, QJSValue startedCallback = QJSValue(),
                                QJSValue completedCallback = QJSValue(), bool dropEvent = false) {
        shared()->uploadFile(path, mapping, startedCallback, completedCallback, dropEvent);
    }
    Q_INVOKABLE void deleteMappings(QStringList paths, QJSValue callback = QJSValue()) { shared()->deleteMappings(paths, callback); }
    Q_INVOKABLE void deleteMapping(QString path, QJSValue callback = QJSValue()) { shared()->deleteMappings(QStringList(path), callback); }
    /// callback(error, mappings): mappings an object of path -> hash, made in this context's engine
    Q_INVOKABLE void getAllMappings(QJSValue callback = QJSValue());
    Q_INVOKABLE void renameMapping(QString oldPath, QString newPath, QJSValue callback = QJSValue()) { shared()->renameMapping(oldPath, newPath, callback); }
    Q_INVOKABLE void sortProxyModel(int column, Qt::SortOrder order = Qt::AscendingOrder) { shared()->sortProxyModel(column, order); }

private:
    static QSharedPointer<AssetMappingsScriptingInterface> shared() { return DependencyManager::get<AssetMappingsScriptingInterface>(); }
    QPointer<QJSEngine> _engine;
};

#endif // hifi_AssetMappingsScriptingInterface_h
