//
//  EntityTreeSendThread.cpp
//  assignment-client/src/entities
//
//  Created by Stephen Birarda on 2/15/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EntityTreeSendThread.h"

#include <functional>
#include <mutex>
#include <set>

#include <QtCore/QProcessEnvironment>

#include <EntityNodeData.h>
#include <EntityTypes.h>
#include <OctreeUtils.h>
#include <UUID.h>

#include "EntityServer.h"

EntityTreeSendThread::EntityTreeSendThread(OctreeServer* myServer, const SharedNodePointer& node) :
    OctreeSendThread(myServer, node)
{
    // the servers the domain server assigns, which only it can start: the entity script server, the avatar mixer and
    // the physics server send filtered queries whose ancestors and descendants are tracked (preDistributionProcessing)
    _tracksFamilies = node && (node->getType() == NodeType::EntityScriptServer || node->getType() == NodeType::AvatarMixer
        || node->getType() == NodeType::PhysicsServer);

    connect(std::static_pointer_cast<EntityTree>(myServer->getOctree()).get(), &EntityTree::editingEntityPointer, this, &EntityTreeSendThread::editingEntityPointer, Qt::QueuedConnection);
    connect(std::static_pointer_cast<EntityTree>(myServer->getOctree()).get(), &EntityTree::deletingEntityPointer, this, &EntityTreeSendThread::deletingEntityPointer, Qt::QueuedConnection);

    // connect to connection ID change on EntityNodeData so we can clear state for this receiver
    auto nodeData = static_cast<EntityNodeData*>(node->getLinkedData());
    connect(nodeData, &EntityNodeData::incomingConnectionIDChanged, this, &EntityTreeSendThread::resetState);
}

void EntityTreeSendThread::resetState() {
    qCDebug(entities) << "Clearing known EntityTreeSendThread state for" << _nodeUuid;

    _knownState.clear();
    _traversal.reset();
}

void EntityTreeSendThread::preDistributionProcessing() {
    auto node = _node.toStrongRef();
    auto nodeData = static_cast<EntityNodeData*>(node->getLinkedData());

    // only the domain's own servers have the ancestors and descendants their query asks for tracked (_tracksFamilies);
    // a visitor's app or an agent gets what its view takes in
    if (nodeData && _tracksFamilies) {
        auto jsonQuery = nodeData->getJSONParameters();

        // check if we have a JSON query with flags
        auto flags = jsonQuery[EntityJSONQueryProperties::FLAGS_PROPERTY].toObject();
        if (!flags.isEmpty()) {
            // check the flags object for specific flags that require special pre-processing

            bool includeAncestors = flags[EntityJSONQueryProperties::INCLUDE_ANCESTORS_PROPERTY].toBool();
            bool includeDescendants = flags[EntityJSONQueryProperties::INCLUDE_DESCENDANTS_PROPERTY].toBool();

            if (includeAncestors || includeDescendants) {
                // we need to either include the ancestors, descendants, or both for entities matching the filter
                // included in the JSON query

                // first reset our flagged extra entities so we start with an empty set
                nodeData->resetFlaggedExtraEntities();

                auto entityTree = std::static_pointer_cast<EntityTree>(_myServer->getOctree());

                // enumerate the set of entity IDs we know currently match the filter; the extras that are new since
                // the last pass are kept in _newExtraEntities, and the next traversal start puts them on the queue
                foreach(const QUuid& entityID, nodeData->getSentFilteredEntities()) {
                    if (includeAncestors) {
                        // we need to include ancestors - recurse up to reach them all and add their IDs
                        // to the set of extra entities to include for this node
                        entityTree->withReadLock([&]{
                            auto filteredEntity = entityTree->findEntityByID(entityID);
                            if (filteredEntity) {
                                addAncestorsToExtraFlaggedEntities(entityID, *filteredEntity, *nodeData);
                            }
                        });
                    }

                    if (includeDescendants) {
                        // we need to include descendants - recurse down to reach them all and add their IDs
                        // to the set of extra entities to include for this node
                        entityTree->withReadLock([&]{
                            auto filteredEntity = entityTree->findEntityByID(entityID);
                            if (filteredEntity) {
                                addDescendantsToExtraFlaggedEntities(entityID, *filteredEntity, *nodeData);
                            }
                        });
                    }
                }

                // the new extras go on the send queue at the next traversal start, without a full resend
            }
        }
    }
}

bool EntityTreeSendThread::traverseTreeAndSendContents(SharedNodePointer node, OctreeQueryNode* nodeData,
            bool viewFrustumChanged, bool isFullScene) {
    if (viewFrustumChanged || _traversal.finished()) {
        EntityTreeElementPointer root = std::dynamic_pointer_cast<EntityTreeElement>(_myServer->getOctree()->getRoot());


        DiffTraversal::View newView;
        newView.viewFrustums = nodeData->getCurrentViews();

        int32_t lodLevelOffset = nodeData->getBoundaryLevelAdjust() + (viewFrustumChanged ? LOW_RES_MOVING_ADJUST : NO_BOUNDARY_ADJUST);
        newView.lodScaleFactor = powf(2.0f, lodLevelOffset);

        // a client that draws impostor cards says so among its query's JSON flags (ENTITY-STREAMING.md 3.3): carded
        // entities are then sized by their cards. Read once per traversal, like the LOD above; an older client sends
        // no such flag and gets the stream as before
        auto queryFlags = nodeData->getJSONParameters()[EntityJSONQueryProperties::FLAGS_PROPERTY].toObject();
        newView.impostorCards = queryFlags[EntityJSONQueryProperties::IMPOSTOR_CARDS_PROPERTY].toBool();

        DiffTraversal::Type type = startNewTraversal(newView, root, isFullScene);

        // TIVOLI_STREAM_DIAG in the environment: a log line per traversal started other than a Repeat, and one per
        // second with what went out (the hunt for entities that never reach a client, KNOWN-ISSUES.md). Off by
        // default: nothing is formatted.
        static const bool STREAM_DIAG = QProcessEnvironment::systemEnvironment().contains("TIVOLI_STREAM_DIAG");
        if (STREAM_DIAG) {
            quint64 now = usecTimestampNow();
            bool periodic = now - _diagLastLog > USECS_PER_SECOND;
            if (type != DiffTraversal::Repeat || periodic) {
                QString views;
                for (const auto& view : newView.viewFrustums) {
                    const auto& p = view.getPosition();
                    const auto& d = view.getDirection();
                    views += QString(" [pos %1 %2 %3 dir %4 %5 %6 angle %7 far %8 radius %9]")
                        .arg(p.x, 0, 'f', 2).arg(p.y, 0, 'f', 2).arg(p.z, 0, 'f', 2)
                        .arg(d.x, 0, 'f', 3).arg(d.y, 0, 'f', 3).arg(d.z, 0, 'f', 3)
                        .arg(glm::degrees(view.getAngle()), 0, 'f', 1).arg(view.getFarClip(), 0, 'f', 0).arg(view.getRadius(), 0, 'f', 2);
                }
                qCDebug(entities).noquote() << "stream" << _nodeUuid.toString().mid(1, 8)
                    << (type == DiffTraversal::First ? "FIRST" : type == DiffTraversal::Differential ? "DIFFERENTIAL" : "repeat")
                    << "viewChanged" << viewFrustumChanged << "fullScene" << isFullScene
                    << "views" << newView.viewFrustums.size() << views
                    << "lodScale" << newView.lodScaleFactor << "cards" << newView.impostorCards
                    << "queue" << _sendQueue.size() << "known" << _knownState.size()
                    << "sentSinceLast" << _diagSentSinceLog
                    << "droppedTooBig" << _diagDroppedProperties << "serverStreamChecks" << EntityTree::streamChecksRun()
                    << "editsStampedAhead" << EntityTree::editsStampedAhead()
                    << "largestLeadMs" << (double)EntityTree::largestStampLeadUsec() / USECS_PER_MSEC
                    << "sendBacksSkipped" << EntityTree::sendBacksSkipped();
                _diagLastLog = now;
                _diagSentSinceLog = 0;
            }
            if (type == DiffTraversal::First) {
                logMisplacedEntities(root);
            }
        }
        // round 10/11 diagnostics: when this traversal began and what it has sent since (the completion line below)
        _traversalStartTime = usecTimestampNow();
        _packetsSentSinceMark = 0;
        _entitiesSinceMark = 0;

        // When the viewFrustum changed the sort order may be incorrect, so we re-sort
        // and also use the opportunity to cull anything no longer in view
        if (viewFrustumChanged && !_sendQueue.empty()) {
            EntityPriorityQueue prevSendQueue;
            std::swap(_sendQueue, prevSendQueue);
            assert(_sendQueue.empty());

            // Re-add elements from previous traversal if they still need to be sent
            while (!prevSendQueue.empty()) {
                EntityItemPointer entity = prevSendQueue.top().getEntity();
                bool forceRemove = prevSendQueue.top().shouldForceRemove();
                prevSendQueue.pop();
                if (entity) {
                    float priority = PrioritizedEntity::DO_NOT_SEND;

                    if (forceRemove) {
                        priority = PrioritizedEntity::FORCE_REMOVE;
                    } else {
                        const auto& view = _traversal.getCurrentView();
                        priority = view.computePriority(entity);
                    }

                    if (priority != PrioritizedEntity::DO_NOT_SEND) {
                        _sendQueue.emplace(entity, priority, forceRemove);
                    }
                }
            }
        }

        // the ancestors and descendants flagged since the last traversal start, at this view's priority: the
        // entities a First traversal would queue for them, without forgetting what the receiver already has. One
        // already known goes once more: for a filtered query the known state also holds what the filter left out.
        if (!_newExtraEntities.empty()) {
            const auto& view = _traversal.getCurrentView();
            for (const auto& entity : _newExtraEntities) {
                if (entity && !_sendQueue.contains(entity.get())) {
                    float priority = view.computePriority(entity);
                    if (priority != PrioritizedEntity::DO_NOT_SEND) {
                        _sendQueue.emplace(entity, priority);
                    }
                }
            }
            _newExtraEntities.clear();
        }
    }

    if (!_traversal.finished()) {
        quint64 startTime = usecTimestampNow();

        #ifdef DEBUG
        const uint64_t TIME_BUDGET = 400; // usec
        #else
        const uint64_t TIME_BUDGET = 200; // usec
        #endif
        _traversal.traverse(TIME_BUDGET);
        OctreeServer::trackTreeTraverseTime((float)(usecTimestampNow() - startTime));
    }

    bool sendComplete = OctreeSendThread::traverseTreeAndSendContents(node, nodeData, viewFrustumChanged, isFullScene);

    if (sendComplete && nodeData->wantReportInitialCompletion() && _traversal.finished()) {
        // Dealt with all nearby entities.
        nodeData->setReportInitialCompletion(false);
        // a line per completion reported that sent something, else one in ten seconds (a headless viewer asks for the
        // completion with every query): the sequence number the client will wait up to, and what this traversal sent
        quint64 now = usecTimestampNow();
        if (_packetsSentSinceMark > 0 || now - _lastCompletionLog > 10 * USECS_PER_SECOND) {
            _lastCompletionLog = now;
            qCDebug(entities) << "initial scene complete for" << uuidStringWithoutCurlyBraces(_nodeUuid) << ": sequence"
                << nodeData->getSequenceNumber() << "," << _packetsSentSinceMark << "packets (" << _entitiesSinceMark
                << "entities) in" << (now - _traversalStartTime) / 1000 << "ms since the traversal began, known"
                << _knownState.size() << "entities";
        }
        // initial stats and entity packets are reliable until the initial query is complete
        // to guarantee all entity data is available for safe landing/physics start.  Afterwards
        // the packets are unreliable for performance.
        nodeData->stats.getStatsMessage().setReliable(false);
        nodeData->getPacket().setReliable(false);

        // Send EntityQueryInitialResultsComplete reliable packet ...
        auto initialCompletion = NLPacket::create(PacketType::EntityQueryInitialResultsComplete,
            sizeof(OCTREE_PACKET_SEQUENCE), true);
        initialCompletion->writePrimitive(OCTREE_PACKET_SEQUENCE(nodeData->getSequenceNumber()));
        DependencyManager::get<NodeList>()->sendPacket(std::move(initialCompletion), *node);
    }

    return sendComplete;
}

void EntityTreeSendThread::logMisplacedEntities(const EntityTreeElementPointer& root) {
    // TIVOLI_STREAM_DIAG, at each first traversal: the entities whose octree element does not hold their query cube.
    // The traversal reaches an entity only through its element, so such an entity is judged by its element's size
    // rather than its own (the hunt for Tivoli Town's island, 519 children of a 1 mm group that often never arrived)
    int misplaced = 0;
    int entityCount = 0;
    std::function<void(const EntityTreeElementPointer&)> visit = [&](const EntityTreeElementPointer& element) {
        element->forEachEntity([&](const EntityItemPointer& entity) {
            ++entityCount;
            bool success = false;
            AACube queryCube = entity->getQueryAACube(success);
            if (success && !element->containsBounds(queryCube)) {   // as the octree files it: clamped to the tree
                const int MAX_LINES = 20;
                if (++misplaced <= MAX_LINES) {
                    qCDebug(entities).noquote() << "stream misplaced" << entity->getID().toString() << entity->getName()
                        << "children" << entity->getChildren().size() << "query cube" << queryCube.getScale()
                        << "element cube" << element->getAACube().getScale();
                }
            }
        });
        for (int i = 0; i < NUMBER_OF_CHILDREN; ++i) {
            auto child = element->getChildAtIndex(i);
            if (child) {
                visit(child);
            }
        }
    };
    visit(root);
    qCDebug(entities) << "stream misplaced entities" << misplaced << "of" << entityCount;
}

bool EntityTreeSendThread::addAncestorsToExtraFlaggedEntities(const QUuid& filteredEntityID,
                                                              EntityItem& entityItem, EntityNodeData& nodeData) {
    // check if this entity has a parent that is also an entity
    bool success = false;
    auto entityParent = entityItem.getParentPointer(success);

    if (success && entityParent && entityParent->getNestableType() == NestableType::Entity) {
        // we found a parent that is an entity item

        // first add it to the extra list of things we need to send
        bool parentWasNew = nodeData.insertFlaggedExtraEntity(filteredEntityID, entityParent->getID());

        // now recursively call ourselves to get its ancestors added too
        auto parentEntityItem = std::static_pointer_cast<EntityItem>(entityParent);
        if (parentWasNew) {
            _newExtraEntities.push_back(parentEntityItem);
        }
        bool ancestorsWereNew = addAncestorsToExtraFlaggedEntities(filteredEntityID, *parentEntityItem, nodeData);

        // return boolean if our parent or any of our ancestors were new additions (via insertFlaggedExtraEntity)
        return parentWasNew || ancestorsWereNew;
    }

    // since we didn't have a parent, neither of our parents or ancestors could be new additions
    return false;
}

bool EntityTreeSendThread::addDescendantsToExtraFlaggedEntities(const QUuid& filteredEntityID,
                                                                EntityItem& entityItem, EntityNodeData& nodeData) {
    bool hasNewChild = false;
    bool hasNewDescendants = false;

    // enumerate the immediate children of this entity
    foreach (SpatiallyNestablePointer child, entityItem.getChildren()) {

        if (child && child->getNestableType() == NestableType::Entity) {
            // this is a child that is an entity

            // first add it to the extra list of things we need to send
            bool childWasNew = nodeData.insertFlaggedExtraEntity(filteredEntityID, child->getID());
            hasNewChild |= childWasNew;

            // now recursively call ourselves to get its descendants added too
            auto childEntityItem = std::static_pointer_cast<EntityItem>(child);
            if (childWasNew) {
                _newExtraEntities.push_back(childEntityItem);
            }
            hasNewDescendants |= addDescendantsToExtraFlaggedEntities(filteredEntityID, *childEntityItem, nodeData);
        }
    }

    // return our boolean indicating if we added new children or descendants as extra entities to send
    // (via insertFlaggedExtraEntity)
    return hasNewChild || hasNewDescendants;
}

DiffTraversal::Type EntityTreeSendThread::startNewTraversal(const DiffTraversal::View& view, EntityTreeElementPointer root,
                                                            bool forceFirstPass) {

    DiffTraversal::Type type = _traversal.prepareNewTraversal(view, root, forceFirstPass);
    // there are three types of traversal:
    //
    //      (1) FirstTime = at login --> find everything in view
    //      (2) Repeat = view hasn't changed --> find what has changed since last complete traversal
    //      (3) Differential = view has changed --> find what has changed or in new view but not old
    //
    // The "scanCallback" we provide to the traversal depends on the type:

    switch (type) {
        case DiffTraversal::First:
            // When we get to a First traversal, clear the _knownState
            qCDebug(entities) << "first traversal for" << uuidStringWithoutCurlyBraces(_nodeUuid) << ": forgetting" << _knownState.size()
                << "known entities (forced" << forceFirstPass << "), views" << view.viewFrustums.size() << ", lod scale" << view.lodScaleFactor;
            _knownState.clear();
            _traversal.setScanCallback([this](DiffTraversal::VisibleElement& next) {
                next.element->forEachEntity([&](EntityItemPointer entity) {
                    // Bail early if we've already checked this entity this frame
                    if (_sendQueue.contains(entity.get())) {
                        return;
                    }
                    const auto& view = _traversal.getCurrentView();
                    float priority = view.computePriority(entity);

                    if (priority != PrioritizedEntity::DO_NOT_SEND) {
                        _sendQueue.emplace(entity, priority);
                    }
                });
            });
            break;
        case DiffTraversal::Repeat:
            _traversal.setScanCallback([this](DiffTraversal::VisibleElement& next) {
                uint64_t startOfCompletedTraversal = _traversal.getStartOfCompletedTraversal();
                if (next.element->getLastChangedContent() > startOfCompletedTraversal) {
                    next.element->forEachEntity([&](EntityItemPointer entity) {
                        // Bail early if we've already checked this entity this frame
                        if (_sendQueue.contains(entity.get())) {
                            return;
                        }
                        float priority = PrioritizedEntity::DO_NOT_SEND;

                        auto knownTimestamp = _knownState.find(entity.get());
                        if (knownTimestamp == _knownState.end()) {
                            const auto& view = _traversal.getCurrentView();
                            priority = view.computePriority(entity);

                        } else if (entity->getLastEdited() > knownTimestamp->second ||
                                   entity->getLastChangedOnServer() > knownTimestamp->second) {
                            // it is known and it changed --> put it on the queue with any priority
                            // TODO: sort these correctly
                            priority = PrioritizedEntity::WHEN_IN_DOUBT_PRIORITY;
                        }

                        if (priority != PrioritizedEntity::DO_NOT_SEND) {
                            _sendQueue.emplace(entity, priority);
                        }
                    });
                }
            });
            break;
        case DiffTraversal::Differential:
            assert(view.usesViewFrustums());
            _traversal.setScanCallback([this] (DiffTraversal::VisibleElement& next) {
                next.element->forEachEntity([&](EntityItemPointer entity) {
                    // Bail early if we've already checked this entity this frame
                    if (_sendQueue.contains(entity.get())) {
                        return;
                    }
                    float priority = PrioritizedEntity::DO_NOT_SEND;

                    auto knownTimestamp = _knownState.find(entity.get());
                    if (knownTimestamp == _knownState.end()) {
                        const auto& view = _traversal.getCurrentView();
                        priority = view.computePriority(entity);

                    } else if (entity->getLastEdited() > knownTimestamp->second ||
                               entity->getLastChangedOnServer() > knownTimestamp->second) {
                        // it is known and it changed --> put it on the queue with any priority
                        // TODO: sort these correctly
                        priority = PrioritizedEntity::WHEN_IN_DOUBT_PRIORITY;
                    }

                    if (priority != PrioritizedEntity::DO_NOT_SEND) {
                        _sendQueue.emplace(entity, priority);
                    }
                });
            });
            break;
    }
    return type;
}

bool EntityTreeSendThread::traverseTreeAndBuildNextPacketPayload(EncodeBitstreamParams& params, const QJsonObject& jsonFilters) {
    if (_sendQueue.empty()) {
        params.stopReason = EncodeBitstreamParams::FINISHED;
        OctreeServer::trackEncodeTime(OctreeServer::SKIP_TIME);
        return false;
    }
    quint64 encodeStart = usecTimestampNow();
    if (!_packetData.hasContent()) {
        // This is the beginning of a new packet.
        // We pack minimal data for this to be accepted as an OctreeElement payload for the root element.
        // The Octree header bytes look like this:
        //
        // 0x00  octalcode for root
        // 0x00  colors (1 bit where recipient should call: child->readElementDataFromBuffer())
        // 0xXX  childrenInTreeMask (when params.includeExistsBits is true: 1 bit where child is existant)
        // 0x00  childrenInBufferMask (1 bit where recipient should call: child->readElementData() recursively)
        const uint8_t zeroByte = 0;
        _packetData.appendValue(zeroByte); // octalcode
        _packetData.appendValue(zeroByte); // colors
        if (params.includeExistsBits) {
            uint8_t childrenExistBits = 0;
            EntityTreeElementPointer root = std::dynamic_pointer_cast<EntityTreeElement>(_myServer->getOctree()->getRoot());
            for (int32_t i = 0; i < NUMBER_OF_CHILDREN; ++i) {
                if (root->getChildAtIndex(i)) {
                    childrenExistBits += (1 << i);
                }
            }
            _packetData.appendValue(childrenExistBits); // childrenInTreeMask
        }
        _packetData.appendValue(zeroByte); // childrenInBufferMask

        // Pack zero for numEntities.
        // But before we do: grab current byteOffset so we can come back later
        // and update this with the real number.
        _numEntities = 0;
        _numEntitiesOffset = _packetData.getUncompressedByteOffset();
        _packetData.appendValue(_numEntities);
    }

    LevelDetails entitiesLevel = _packetData.startLevel();
    uint64_t sendTime = usecTimestampNow();
    auto nodeData = static_cast<OctreeQueryNode*>(params.nodeData);
    nodeData->stats.encodeStarted();
    auto entityNode = _node.toStrongRef();
    auto entityNodeData = static_cast<EntityNodeData*>(entityNode->getLinkedData());
    while(!_sendQueue.empty()) {
        PrioritizedEntity queuedItem = _sendQueue.top();
        EntityItemPointer entity = queuedItem.getEntity();
        if (entity) {
            const QUuid& entityID = entity->getID();
            // Only send entities that match the jsonFilters, but keep track of everything we've tried to send so we don't try to send it again;
            // also send if we previously matched since this represents change to a matched item.
            bool entityMatchesFilters = entity->matchesJSONFilters(jsonFilters);
            bool entityPreviouslyMatchedFilter = entityNodeData->sentFilteredEntity(entityID);

            if (entityMatchesFilters || entityNodeData->isEntityFlaggedAsExtra(entityID) || entityPreviouslyMatchedFilter) {
                if (_tracksFamilies && !jsonFilters.isEmpty() && entityMatchesFilters) {
                    // Record explicitly filtered-in entity so that extra entities can be flagged.
                    entityNodeData->insertSentFilteredEntity(entityID);
                }
                OctreeElement::AppendState appendEntityState = entity->appendEntityData(&_packetData, params, _extraEncodeData, entityNode->getCanGetAndSetPrivateUserData());

                if (appendEntityState != OctreeElement::COMPLETED) {
                    if (appendEntityState == OctreeElement::NONE && _numEntities == 0 &&
                        (int)_packetData.getTargetSize() >= EntityItem::streamSectionRoom()) {
                        // Nothing more of this entity fits even an empty packet, so it never will. Drop what's left of
                        // it for this viewer and go on, rather than keep it at the head of the queue for good with
                        // everything ranked after it waiting. Its first pass already sent every property that fit.
                        dropUnsendableProperties(*entity, entityNode->getCanGetAndSetPrivateUserData());
                        if (entityPreviouslyMatchedFilter && !entityMatchesFilters) {
                            entityNodeData->removeSentFilteredEntity(entityID);
                        }
                        if (queuedItem.shouldForceRemove()) {
                            _knownState.erase(entity.get());
                        } else {
                            _knownState[entity.get()] = sendTime;
                        }
                        _sendQueue.pop();
                        continue;
                    }
                    if (appendEntityState == OctreeElement::PARTIAL) {
                        ++_numEntities;
                    }
                    params.stopReason = EncodeBitstreamParams::DIDNT_FIT;
                    break;
                }

                if (entityPreviouslyMatchedFilter && !entityMatchesFilters) {
                    entityNodeData->removeSentFilteredEntity(entityID);
                }
                ++_numEntities;
                ++_entitiesSinceMark;
            }
            if (queuedItem.shouldForceRemove()) {
                _knownState.erase(entity.get());
            } else {
                _knownState[entity.get()] = sendTime;
                ++_diagSentSinceLog;
            }
        }
        _sendQueue.pop();
    }
    nodeData->stats.encodeStopped();
    if (_sendQueue.empty()) {
        assert(_sendQueue.empty());
        params.stopReason = EncodeBitstreamParams::FINISHED;
        _extraEncodeData->entities.clear();
    }

    if (_numEntities == 0) {
        _packetData.discardLevel(entitiesLevel);
        OctreeServer::trackEncodeTime((float)(usecTimestampNow() - encodeStart));
        return false;
    }
    _packetData.endLevel(entitiesLevel);
    _packetData.updatePriorBytes(_numEntitiesOffset, (const unsigned char*)&_numEntities, sizeof(_numEntities));
    OctreeServer::trackEncodeTime((float)(usecTimestampNow() - encodeStart));
    return true;
}

namespace {

// An entity's name for a log line: it is world text, so at most 64 characters, without control, format (bidi) or
// line-separator characters, and quoted, so it can't break the line or pass for another one.
QString loggableName(const QString& name) {
    const int MAX_NAME_CHARACTERS = 64;
    QString out;
    for (const QChar& c : name) {
        if (out.size() >= MAX_NAME_CHARACTERS || (c.isHighSurrogate() && out.size() >= MAX_NAME_CHARACTERS - 1)) {
            out += QStringLiteral("...");
            break;
        }
        const QChar::Category category = c.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format ||
            category == QChar::Separator_Line || category == QChar::Separator_Paragraph) {
            continue;
        }
        out += (c == QChar('"') || c == QChar('\\')) ? QChar('\'') : c;
    }
    return QStringLiteral("\"") + out + QStringLiteral("\"");
}

// One warning per entity and property for the server's life, whatever the viewers, and at most this many in all
std::mutex unsendableWarningsMutex;
std::set<std::pair<QUuid, int>> unsendableWarnings;
const size_t MAX_UNSENDABLE_WARNINGS = 1000;

}

void EntityTreeSendThread::dropUnsendableProperties(const EntityItem& entity, bool destinationCanGetAndSetPrivateUserData) {
    const EntityItemID entityID = entity.getEntityItemID();
    const EntityPropertyFlags leftover = _extraEncodeData->entities.value(entityID);
    _extraEncodeData->entities.remove(entityID);
    ++_diagDroppedProperties;

    for (int flag = (int)leftover.firstFlag(); flag <= (int)leftover.lastFlag(); flag++) {
        const EntityPropertyList property = (EntityPropertyList)flag;
        if (!leftover.getHasProperty(property)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(unsendableWarningsMutex);
            if (unsendableWarnings.size() > MAX_UNSENDABLE_WARNINGS) {
                return;
            }
            if (!unsendableWarnings.insert({ entityID, flag }).second) {
                continue;
            }
            if (unsendableWarnings.size() > MAX_UNSENDABLE_WARNINGS) {
                qCWarning(entities) << "stream: more properties too big to send; no more of these warnings until the"
                    << "entity server restarts";
                return;
            }
        }
        // the room it would need, found by the stream's own encoder
        const int needed = entity.streamRoomNeededAlone(property, destinationCanGetAndSetPrivateUserData);
        qCWarning(entities).noquote() << "stream: entity" << entityID.toString() << loggableName(entity.getName())
            << "(" + EntityTypes::getEntityTypeName(entity.getType()) + ")" << "property" << EntityItem::propertyName(property)
            << "needs" << (needed < 0 ? QString("more than %1").arg(2 * MAX_APPENDED_VALUE_LENGTH) : QString::number(needed))
            << "bytes alone, more than the" << EntityItem::streamSectionRoom()
            << "a stream packet holds: it is not sent to other viewers; the entity's other properties are";
    }
}

void EntityTreeSendThread::editingEntityPointer(const EntityItemPointer& entity) {
    if (entity) {
        if (!_sendQueue.contains(entity.get()) && _knownState.find(entity.get()) != _knownState.end()) {
            const auto& view = _traversal.getCurrentView();
            float priority = view.computePriority(entity);

            // We can force a removal from _knownState if the current view is used and entity is out of view
            if (priority == PrioritizedEntity::DO_NOT_SEND) {
                _sendQueue.emplace(entity, PrioritizedEntity::FORCE_REMOVE, true);
            } else if (priority == PrioritizedEntity::WHEN_IN_DOUBT_PRIORITY) {
                _sendQueue.emplace(entity, PrioritizedEntity::WHEN_IN_DOUBT_PRIORITY, true);
            }
        }
    }
}

void EntityTreeSendThread::deletingEntityPointer(EntityItem* entity) {
    _knownState.erase(entity);
}
