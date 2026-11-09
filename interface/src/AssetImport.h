//
//  AssetImport.h
//  interface/src
//
//  A model, image or zip from a URL or a drop becomes an asset on the domain's asset server and then an entity in the
//  world. handleUnzip() takes the unzipper's
//  result (the files to add, or the asset server widget when nothing is to be added automatically);
//  addAssetToWorld() uploads a file under a free mapping (the name counted up while it is taken), sets the mapping
//  and adds the entity in front of the avatar, which addAssetToWorldCheckModelSize() resizes once its model's
//  dimensions arrive (a timer, once a second); the info and error messages along the way (an info box the code no
//  longer shows, an alert for an error); addAssetToWorldMessageClose() clears them when the domain changes or the
//  client quits. A QObject: it owns timers and receives the requests' and uploads' signals. Application owns one,
//  reached by getAssetImport(); Application's URL dispatch (a dropped image) reaches it through addAssetToWorld(). These were
//  Application's addAssetToWorld members, handleUnzip and their state; the members keep the flow's original name
//  (ARCHITECTURE.md §5.4 step 4, the sixth owner class).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_AssetImport_h
#define hifi_AssetImport_h

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

class QQuickItem;

class AssetImport : public QObject {
    Q_OBJECT
public:
    AssetImport();   // the timers set up: the size check once a second, the info and error messages' time-outs

    void addAssetToWorld(QString filePath, QString zipFile, bool isZip = false, bool isBlocks = false);   // upload, map, add
    void handleUnzip(QString sourceFile, QStringList destinationFile, bool autoAdd, bool isZip, bool isBlocks);   // the unzipper's result
    void addAssetToWorldMessageClose();   // the messages cleared: the domain changed, the client quits, the box was closed by hand

private slots:
    void addAssetToWorldCheckModelSize();
    void addAssetToWorldInfoTimeout();
    void addAssetToWorldErrorTimeout();
    void onAssetToWorldMessageBoxClosed();

private:
    void addAssetToWorldUnzipFailure(QString filePath);
    void addAssetToWorldWithNewMapping(QString filePath, QString mapping, int copy, bool isZip = false, bool isBlocks = false);
    void addAssetToWorldUpload(QString filePath, QString mapping, bool isZip = false, bool isBlocks = false);
    void addAssetToWorldSetMapping(QString filePath, QString mapping, QString hash, bool isZip = false, bool isBlocks = false);
    void addAssetToWorldAddEntity(QString filePath, QString mapping);
    void addAssetToWorldInfo(QString modelName, QString infoText);
    void addAssetToWorldInfoClear(QString modelName);
    void addAssetToWorldInfoDone(QString modelName);
    void addAssetToWorldError(QString modelName, QString errorText);

    QTimer _addAssetToWorldResizeTimer;
    QHash<QUuid, int> _addAssetToWorldResizeList;   // the entities added, each with the count of size checks done
    QQuickItem* _addAssetToWorldMessageBox{ nullptr };
    QStringList _addAssetToWorldInfoKeys;      // Model name
    QStringList _addAssetToWorldInfoMessages;  // Info message
    QTimer _addAssetToWorldInfoTimer;
    QTimer _addAssetToWorldErrorTimer;
};

#endif // hifi_AssetImport_h
