//
//  EntityClipboard.cpp
//  interface/src
//
//  The entities' clipboard (see EntityClipboard.h). The code of Application's exportEntities, importEntities and
//  pasteEntities, with Application reached through qApp for the world's entities, the window and the entity edit
//  packet sender.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "EntityClipboard.h"

#include <MainWindow.h>

#include "Application.h"
#include "avatar/AvatarManager.h"
#include "InterfaceLogging.h"

// Application's getMyAvatar is private; this is the same call
static std::shared_ptr<MyAvatar> getMyAvatar() {
    return DependencyManager::get<AvatarManager>()->getMyAvatar();
}

EntityClipboard::EntityClipboard() : _tree(new EntityTree()) {
    _tree->createRootElement();
}

void EntityClipboard::shutdown() {
    _tree->eraseAllOctreeElements();
    _tree.reset();
}

bool EntityClipboard::exportEntities(const QString& filename,
                                     const QVector<QUuid>& entityIDs,
                                     const glm::vec3* givenOffset) {
    QHash<EntityItemID, EntityItemPointer> entities;

    auto nodeList = DependencyManager::get<NodeList>();
    const QUuid myAvatarID = nodeList->getSessionUUID();

    auto entityTree = qApp->getEntities()->getTree();
    auto exportTree = std::make_shared<EntityTree>();
    exportTree->setMyAvatar(getMyAvatar());
    exportTree->createRootElement();
    glm::vec3 root(TREE_SCALE, TREE_SCALE, TREE_SCALE);
    bool success = true;
    entityTree->withReadLock([entityIDs, entityTree, givenOffset, myAvatarID, &root, &entities, &success, &exportTree] {
        for (auto entityID : entityIDs) { // Gather entities and properties.
            auto entityItem = entityTree->findEntityByEntityItemID(entityID);
            if (!entityItem) {
                qCWarning(interfaceapp) << "Skipping export of" << entityID << "that is not in scene.";
                continue;
            }

            if (!givenOffset) {
                EntityItemID parentID = entityItem->getParentID();
                bool parentIsAvatar = (parentID == AVATAR_SELF_ID || parentID == myAvatarID);
                if (!parentIsAvatar && (parentID.isInvalidID() ||
                                        !entityIDs.contains(parentID) ||
                                        !entityTree->findEntityByEntityItemID(parentID))) {
                    // If parent wasn't selected, we want absolute position, which isn't in properties.
                    auto position = entityItem->getWorldPosition();
                    root.x = glm::min(root.x, position.x);
                    root.y = glm::min(root.y, position.y);
                    root.z = glm::min(root.z, position.z);
                }
            }
            entities[entityID] = entityItem;
        }

        if (entities.size() == 0) {
            success = false;
            return;
        }

        if (givenOffset) {
            root = *givenOffset;
        }
        for (EntityItemPointer& entityDatum : entities) {
            auto properties = entityDatum->getProperties();
            EntityItemID parentID = properties.getParentID();
            bool parentIsAvatar = (parentID == AVATAR_SELF_ID || parentID == myAvatarID);
            if (parentIsAvatar) {
                properties.setParentID(AVATAR_SELF_ID);
            } else {
                if (parentID.isInvalidID()) {
                    properties.setPosition(properties.getPosition() - root);
                } else if (!entities.contains(parentID)) {
                    entityDatum->globalizeProperties(properties, "Parent %3 of %2 %1 is not selected for export.", -root);
                } // else valid parent -- don't offset
            }
            exportTree->addEntity(entityDatum->getEntityItemID(), properties);
        }
    });
    if (success) {
        success = exportTree->writeToJSONFile(filename.toLocal8Bit().constData());

        // restore the main window's active state
        qApp->getWindow()->activateWindow();
    }
    return success;
}

bool EntityClipboard::exportEntities(const QString& filename, float x, float y, float z, float scale) {
    glm::vec3 center(x, y, z);
    glm::vec3 minCorner = center - vec3(scale);
    float cubeSize = scale * 2;
    AACube boundingCube(minCorner, cubeSize);
    QVector<QUuid> entities;
    auto entityTree = qApp->getEntities()->getTree();
    entityTree->withReadLock([&] {
        entityTree->evalEntitiesInCube(boundingCube, PickFilter(), entities);
    });
    return exportEntities(filename, entities, &center);
}

bool EntityClipboard::importEntities(const QString& urlOrFilename, const bool isObservable, const qint64 callerId) {
    bool success = false;
    _tree->withWriteLock([&] {
        _tree->eraseAllOctreeElements();

        // an import follows the world's rule: into a world that isn't local, an address naming a file on this
        // computer is emptied with a warning (it would show to no one else); a local world keeps it
        _tree->setRefusesContentFromThisComputer(qApp->getEntities()->getTree()->refusesContentFromThisComputer());

        // FIXME: readFromURL() can take over the main event loop which may cause problems, especially if downloading the JSON 
        // from the Web.
        success = _tree->readFromURL(urlOrFilename, isObservable, callerId, true);
        if (success) {
            _tree->reaverageOctreeElements();
        }
    });
    return success;
}

QVector<EntityItemID> EntityClipboard::pasteEntities(const QString& entityHostType, float x, float y, float z) {
    return _tree->sendEntities(qApp->getEntityEditPacketSender(), qApp->getEntities()->getTree(), entityHostType, x, y, z);
}
