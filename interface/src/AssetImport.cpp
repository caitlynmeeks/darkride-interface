//
//  AssetImport.cpp
//  interface/src
//
//  A file from a URL or a drop becomes an asset and an entity (see AssetImport.h). The code of Application's
//  addAssetToWorld members and handleUnzip, and of the start-up's set-up of their timers, with Application reached
//  through qApp for the unzipper, the asset server widget and the quit flag.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "AssetImport.h"

#include <QtCore/QRegularExpression>
#include <QtWidgets/QMessageBox>

#include <AssetClient.h>
#include <AssetUpload.h>
#include <EntityScriptingInterface.h>
#include <MappingRequest.h>
#include <OffscreenUi.h>

#include "Application.h"
#include "avatar/AvatarManager.h"
#include "ImportExtensions.h"
#include "InterfaceLogging.h"

QSharedPointer<OffscreenUi> getOffscreenUI();   // Application.cpp's

// Application's getMyAvatar is private; this is the same call
static std::shared_ptr<MyAvatar> getMyAvatar() {
    return DependencyManager::get<AvatarManager>()->getMyAvatar();
}

AssetImport::AssetImport() {
    // Monitor model assets (e.g., from Clara.io) added to the world that may need resizing.
    static const int ADD_ASSET_TO_WORLD_TIMER_INTERVAL_MS = 1000;
    _addAssetToWorldResizeTimer.setInterval(ADD_ASSET_TO_WORLD_TIMER_INTERVAL_MS); // 1s, Qt::CoarseTimer acceptable
    connect(&_addAssetToWorldResizeTimer, &QTimer::timeout, this, &AssetImport::addAssetToWorldCheckModelSize);

    // Auto-update and close adding asset to world info message box.
    static const int ADD_ASSET_TO_WORLD_INFO_TIMEOUT_MS = 5000;
    _addAssetToWorldInfoTimer.setInterval(ADD_ASSET_TO_WORLD_INFO_TIMEOUT_MS); // 5s, Qt::CoarseTimer acceptable
    _addAssetToWorldInfoTimer.setSingleShot(true);
    connect(&_addAssetToWorldInfoTimer, &QTimer::timeout, this, &AssetImport::addAssetToWorldInfoTimeout);
    static const int ADD_ASSET_TO_WORLD_ERROR_TIMEOUT_MS = 8000;
    _addAssetToWorldErrorTimer.setInterval(ADD_ASSET_TO_WORLD_ERROR_TIMEOUT_MS); // 8s, Qt::CoarseTimer acceptable
    _addAssetToWorldErrorTimer.setSingleShot(true);
    connect(&_addAssetToWorldErrorTimer, &QTimer::timeout, this, &AssetImport::addAssetToWorldErrorTimeout);
}

static QString filenameFromPath(QString filePath) {
    return filePath.right(filePath.length() - filePath.lastIndexOf("/") - 1);
}

void AssetImport::addAssetToWorldUnzipFailure(QString filePath) {
    QString filename = filenameFromPath(QUrl(filePath).toLocalFile());
    qWarning(interfaceapp) << "Couldn't unzip file" << filePath;
    addAssetToWorldError(filename, "It couldn't be unzipped.");
}

void AssetImport::addAssetToWorld(QString path, QString zipFile, bool isZip, bool isBlocks) {
    // Automatically upload and add asset to world as an alternative manual process initiated by showAssetServerWidget().
    QString mapping;
    QString filename = filenameFromPath(path);
    if (isZip || isBlocks) {
        QString assetName = zipFile.section("/", -1).remove(QRegularExpression("[.]zip(.*)$"));   // a search, as remove(QRegExp) was
        QString assetFolder = path.section("model_repo/", -1);
        mapping = "/" + assetName + "/" + assetFolder;
    } else {
        mapping = "/" + filename;
    }

    // Test repeated because possibly different code paths.
    if (!DependencyManager::get<NodeList>()->getThisNodeCanWriteAssets()) {
        // QString errorInfo = "You do not have permissions to write to the Asset Server.";
        // qWarning(interfaceapp) << "Error downloading model: " + errorInfo;
        // addAssetToWorldError(filename, errorInfo);
        return;
    }

    addAssetToWorldInfo(filename, "Adding " + mapping.mid(1) + " to the Asset Server.");

    addAssetToWorldWithNewMapping(path, mapping, 0, isZip, isBlocks);
}

void AssetImport::addAssetToWorldWithNewMapping(QString filePath, QString mapping, int copy, bool isZip, bool isBlocks) {
    auto request = DependencyManager::get<AssetClient>()->createGetMappingRequest(mapping);

    QObject::connect(request, &GetMappingRequest::finished, this, [=](GetMappingRequest* request) mutable {
        const int MAX_COPY_COUNT = 100;  // Limit number of duplicate assets; recursion guard.
        auto result = request->getError();
        if (result == GetMappingRequest::NotFound) {
            addAssetToWorldUpload(filePath, mapping, isZip, isBlocks);
        } else if (result != GetMappingRequest::NoError) {
            QString errorInfo = "Could not map asset name: "
                + mapping.left(mapping.length() - QString::number(copy).length() - 1);
            //qWarning(interfaceapp) << "Error downloading model: " + errorInfo;
            qDebug() << "Couldn't map asset name... Error downloading model: " + errorInfo;
           // addAssetToWorldError(filenameFromPath(filePath), errorInfo);
        } else if (copy < MAX_COPY_COUNT - 1) {
            if (copy > 0) {
                mapping = mapping.remove(mapping.lastIndexOf("-"), QString::number(copy).length() + 1);
            }
            copy++;
            mapping = mapping.insert(mapping.lastIndexOf("."), "-" + QString::number(copy));
            addAssetToWorldWithNewMapping(filePath, mapping, copy, isZip, isBlocks);
        } else {
            QString original = mapping.left(mapping.length() - QString::number(copy).length() - 1);
            qWarning(interfaceapp) << "Error downloading model: Too many copies of asset name: " + original;
            addAssetToWorldError(filenameFromPath(filePath),
                                 "There are too many files called " + filenameFromPath(original) + ".");
        }
        request->deleteLater();
    });

    request->start();
}

void AssetImport::addAssetToWorldUpload(QString filePath, QString mapping, bool isZip, bool isBlocks) {
    qInfo(interfaceapp) << "Uploading" << filePath << "to Asset Server as" << mapping;
    auto upload = DependencyManager::get<AssetClient>()->createUpload(filePath);
    QObject::connect(upload, &AssetUpload::finished, this, [=](AssetUpload* upload, const QString& hash) mutable {
        if (upload->getError() != AssetUpload::NoError) {
            QString errorInfo = "Unspecified error downloading from the Asset Server.";
            qDebug(interfaceapp) << "Error downloading model: " + errorInfo;
            //addAssetToWorldError(filenameFromPath(filePath), errorInfo);
        } else {
            addAssetToWorldSetMapping(filePath, mapping, hash, isZip, isBlocks);
        }

        // Remove temporary directory created by Clara.io market place download.
        int index = filePath.lastIndexOf("/model_repo/");
        if (index > 0) {
            QString tempDir = filePath.left(index);
            qCDebug(interfaceapp) << "Removing temporary directory at: " + tempDir;
            QDir(tempDir).removeRecursively();
        }

        upload->deleteLater();
    });

    upload->start();
}

void AssetImport::addAssetToWorldSetMapping(QString filePath, QString mapping, QString hash, bool isZip, bool isBlocks) {
    auto request = DependencyManager::get<AssetClient>()->createSetMappingRequest(mapping, hash);
    connect(request, &SetMappingRequest::finished, this, [=](SetMappingRequest* request) mutable {
        if (request->getError() != SetMappingRequest::NoError) {
            qWarning(interfaceapp) << "Error downloading model: Could not set asset mapping.";
            addAssetToWorldError(filenameFromPath(filePath), "The world's assets didn't take it.");
        } else {
            // to prevent files that aren't models or texture files from being loaded into world automatically
            if ((filePath.toLower().endsWith(OBJ_EXTENSION) || filePath.toLower().endsWith(FBX_EXTENSION)) ||
                ((filePath.toLower().endsWith(JPG_EXTENSION) || filePath.toLower().endsWith(PNG_EXTENSION)) &&
                ((!isBlocks) && (!isZip)))) {
                addAssetToWorldAddEntity(filePath, mapping);
            } else {
                qCDebug(interfaceapp) << "Zipped contents are not supported entity files";
                addAssetToWorldInfoDone(filenameFromPath(filePath));
            }
        }
        request->deleteLater();
    });

    request->start();
}

void AssetImport::addAssetToWorldAddEntity(QString filePath, QString mapping) {
    EntityItemProperties properties;
    properties.setName(mapping.right(mapping.length() - 1));
    if (filePath.toLower().endsWith(PNG_EXTENSION) || filePath.toLower().endsWith(JPG_EXTENSION)) {
        properties.setType(EntityTypes::Image);
        properties.setImageURL(QString("atp:" + mapping));
        properties.setKeepAspectRatio(false);
    } else {
        properties.setType(EntityTypes::Model);
        properties.setModelURL("atp:" + mapping);
        properties.setShapeType(SHAPE_TYPE_SIMPLE_COMPOUND);
    }
    properties.setCollisionless(true);  // Temporarily set so that doesn't collide with avatar.
    properties.setVisible(false);  // Temporarily set so that don't see at large unresized dimensions.
    bool grabbable = (Menu::getInstance()->isOptionChecked(MenuOption::CreateEntitiesGrabbable));
    properties.setUserData(grabbable ? GRABBABLE_USER_DATA : NOT_GRABBABLE_USER_DATA);
    glm::vec3 positionOffset = getMyAvatar()->getWorldOrientation() * (getMyAvatar()->getSensorToWorldScale() * glm::vec3(0.0f, 0.0f, -2.0f));
    properties.setPosition(getMyAvatar()->getWorldPosition() + positionOffset);
    properties.setRotation(getMyAvatar()->getWorldOrientation());
    properties.setGravity(glm::vec3(0.0f, 0.0f, 0.0f));
    auto entityID = DependencyManager::get<EntityScriptingInterface>()->addEntity(properties);

    // Note: Model dimensions are not available here; model is scaled per FBX mesh in RenderableModelEntityItem::update() later
    // on. But FBX dimensions may be in cm, so we monitor for the dimension change and rescale again if warranted.

    if (entityID == QUuid()) {
        qWarning(interfaceapp) << "Could not add model to world: " + mapping;
        addAssetToWorldError(filenameFromPath(filePath), "It couldn't be placed.");
    } else {
        // Monitor when asset is rendered in world so that can resize if necessary.
        _addAssetToWorldResizeList.insert(entityID, 0);  // List value is count of checks performed.
        if (!_addAssetToWorldResizeTimer.isActive()) {
            _addAssetToWorldResizeTimer.start();
        }

        // Close progress message box.
        addAssetToWorldInfoDone(filenameFromPath(filePath));
    }
}

void AssetImport::addAssetToWorldCheckModelSize() {
    if (_addAssetToWorldResizeList.size() == 0) {
        return;
    }

    auto item = _addAssetToWorldResizeList.begin();
    while (item != _addAssetToWorldResizeList.end()) {
        auto entityID = item.key();

        EntityPropertyFlags propertyFlags;
        propertyFlags += PROP_NAME;
        propertyFlags += PROP_DIMENSIONS;
        auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
        auto properties = entityScriptingInterface->getEntityProperties(entityID, propertyFlags);
        auto name = properties.getName();
        auto dimensions = properties.getDimensions();

        bool doResize = false;

        const glm::vec3 DEFAULT_DIMENSIONS = glm::vec3(0.1f, 0.1f, 0.1f);
        if (dimensions != DEFAULT_DIMENSIONS) {

            // Scale model so that its maximum is exactly specific size.
            const float MAXIMUM_DIMENSION = getMyAvatar()->getSensorToWorldScale();
            auto previousDimensions = dimensions;
            auto scale = std::min(MAXIMUM_DIMENSION / dimensions.x, std::min(MAXIMUM_DIMENSION / dimensions.y,
                MAXIMUM_DIMENSION / dimensions.z));
            dimensions *= scale;
            qInfo(interfaceapp) << "Model" << name << "auto-resized from" << previousDimensions << " to " << dimensions;
            doResize = true;

            item = _addAssetToWorldResizeList.erase(item);  // Finished with this entity; advance to next.
        } else {
            // Increment count of checks done.
            _addAssetToWorldResizeList[entityID]++;

            const int CHECK_MODEL_SIZE_MAX_CHECKS = 300;
            if (_addAssetToWorldResizeList[entityID] > CHECK_MODEL_SIZE_MAX_CHECKS) {
                // Have done enough checks; model was either the default size or something's gone wrong.

                // Rescale all dimensions.
                const glm::vec3 UNIT_DIMENSIONS = glm::vec3(1.0f, 1.0f, 1.0f);
                dimensions = UNIT_DIMENSIONS;
                qInfo(interfaceapp) << "Model" << name << "auto-resize timed out; resized to " << dimensions;
                doResize = true;

                item = _addAssetToWorldResizeList.erase(item);  // Finished with this entity; advance to next.
            } else {
                // No action on this entity; advance to next.
                ++item;
            }
        }

        if (doResize) {
            EntityItemProperties properties;
            properties.setDimensions(dimensions);
            properties.setVisible(true);
            if (!name.toLower().endsWith(PNG_EXTENSION) && !name.toLower().endsWith(JPG_EXTENSION)) {
                properties.setCollisionless(false);
            }
            bool grabbable = (Menu::getInstance()->isOptionChecked(MenuOption::CreateEntitiesGrabbable));
            properties.setUserData(grabbable ? GRABBABLE_USER_DATA : NOT_GRABBABLE_USER_DATA);
            properties.setLastEdited(usecTimestampNow());
            entityScriptingInterface->editEntity(entityID, properties);
        }
    }

    // Stop timer if nothing in list to check.
    if (_addAssetToWorldResizeList.size() == 0) {
        _addAssetToWorldResizeTimer.stop();
    }
}

void AssetImport::addAssetToWorldInfo(QString modelName, QString infoText) {
    // Displays the most recent info message, subject to being overridden by error messages.

    if (qApp->isAboutToQuit()) {
        return;
    }

    /*
    Cancel info timer if running.
    If list has an entry for modelName, delete it (just one).
    Append modelName, infoText to list.
    Display infoText in message box unless an error is being displayed (i.e., error timer is running).
    Show message box if not already visible.
    */

    _addAssetToWorldInfoTimer.stop();

    addAssetToWorldInfoClear(modelName);

    _addAssetToWorldInfoKeys.append(modelName);
    _addAssetToWorldInfoMessages.append(infoText);

    return; // this message is annoying and disruptive
    if (!_addAssetToWorldErrorTimer.isActive()) {
        if (!_addAssetToWorldMessageBox) {
            _addAssetToWorldMessageBox = getOffscreenUI()->createMessageBox(OffscreenUi::ICON_INFORMATION,
                "Downloading Model", "", QMessageBox::NoButton, QMessageBox::NoButton);
            connect(_addAssetToWorldMessageBox, SIGNAL(destroyed()), this, SLOT(onAssetToWorldMessageBoxClosed()));
        }

        _addAssetToWorldMessageBox->setProperty("text", "\n" + infoText);
        _addAssetToWorldMessageBox->setVisible(true);
    }
}

void AssetImport::addAssetToWorldInfoClear(QString modelName) {
    // Clears modelName entry from message list without affecting message currently displayed.

    if (qApp->isAboutToQuit()) {
        return;
    }

    /*
    Delete entry for modelName from list.
    */

    auto index = _addAssetToWorldInfoKeys.indexOf(modelName);
    if (index > -1) {
        _addAssetToWorldInfoKeys.removeAt(index);
        _addAssetToWorldInfoMessages.removeAt(index);
    }
}

void AssetImport::addAssetToWorldInfoDone(QString modelName) {
    // Continues to display this message if the latest for a few seconds, then deletes it and displays the next latest.

    if (qApp->isAboutToQuit()) {
        return;
    }

    /*
    Delete entry for modelName from list.
    (Re)start the info timer to update message box. ... onAddAssetToWorldInfoTimeout()
    */

    addAssetToWorldInfoClear(modelName);
    _addAssetToWorldInfoTimer.start();
}

void AssetImport::addAssetToWorldInfoTimeout() {
    if (qApp->isAboutToQuit()) {
        return;
    }

    /*
    If list not empty, display last message in list (may already be displayed ) unless an error is being displayed.
    If list empty, close the message box unless an error is being displayed.
    */

    if (!_addAssetToWorldErrorTimer.isActive() && _addAssetToWorldMessageBox) {
        if (_addAssetToWorldInfoKeys.length() > 0) {
            _addAssetToWorldMessageBox->setProperty("text", "\n" + _addAssetToWorldInfoMessages.last());
        } else {
            disconnect(_addAssetToWorldMessageBox);
            _addAssetToWorldMessageBox->setVisible(false);
            _addAssetToWorldMessageBox->deleteLater();
            _addAssetToWorldMessageBox = nullptr;
        }
    }
}

void AssetImport::addAssetToWorldError(QString modelName, QString errorText) {
    // Says what went wrong in an alert of its own, which stays until it is answered.

    if (qApp->isAboutToQuit()) {
        return;
    }

    addAssetToWorldInfoClear(modelName);
    OffscreenUi::asyncWarning("Couldn't add " + modelName + " to the world", errorText);
}

void AssetImport::addAssetToWorldErrorTimeout() {
    if (qApp->isAboutToQuit()) {
        return;
    }

    /*
    If list is not empty, display message from last entry.
    If list is empty, close the message box.
    */

    if (_addAssetToWorldMessageBox) {
        if (_addAssetToWorldInfoKeys.length() > 0) {
            _addAssetToWorldMessageBox->setProperty("text", "\n" + _addAssetToWorldInfoMessages.last());
        } else {
            disconnect(_addAssetToWorldMessageBox);
            _addAssetToWorldMessageBox->setVisible(false);
            _addAssetToWorldMessageBox->deleteLater();
            _addAssetToWorldMessageBox = nullptr;
        }
    }
}

void AssetImport::addAssetToWorldMessageClose() {
    // Clear messages, e.g., if Interface is being closed or domain changes.

    /*
    Call if user manually closes message box.
    Call if domain changes.
    Call if application is shutting down.

    Stop timers.
    Close the message box if open.
    Clear lists.
    */

    _addAssetToWorldInfoTimer.stop();
    _addAssetToWorldErrorTimer.stop();

    if (_addAssetToWorldMessageBox) {
        disconnect(_addAssetToWorldMessageBox);
        _addAssetToWorldMessageBox->setVisible(false);
        _addAssetToWorldMessageBox->deleteLater();
        _addAssetToWorldMessageBox = nullptr;
    }

    _addAssetToWorldInfoKeys.clear();
    _addAssetToWorldInfoMessages.clear();
}

void AssetImport::onAssetToWorldMessageBoxClosed() {
    if (_addAssetToWorldMessageBox) {
        // User manually closed message box; perhaps because it has become stuck, so reset all messages.
        qInfo(interfaceapp) << "User manually closed download status message box";
        disconnect(_addAssetToWorldMessageBox);
        _addAssetToWorldMessageBox = nullptr;
        addAssetToWorldMessageClose();
    }
}

void AssetImport::handleUnzip(QString zipFile, QStringList unzipFile, bool autoAdd, bool isZip, bool isBlocks) {
    if (autoAdd) {
        if (!unzipFile.isEmpty()) {
            for (int i = 0; i < unzipFile.length(); i++) {
                if (QFileInfo(unzipFile.at(i)).isFile()) {
                    qCDebug(interfaceapp) << "Preparing file for asset server: " << unzipFile.at(i);
                    addAssetToWorld(unzipFile.at(i), zipFile, isZip, isBlocks);
                }
            }
        } else {
            addAssetToWorldUnzipFailure(zipFile);
        }
    } else {
        if (unzipFile.isEmpty()) {
            qCWarning(interfaceapp) << "Nothing was unpacked from" << zipFile << "to show";
            return;
        }
        qApp->showAssetServerWidget(unzipFile.first());
    }
}
