//
//  EntityTreeSendThread.h
//  assignment-client/src/entities
//
//  Created by Stephen Birarda on 2/15/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_EntityTreeSendThread_h
#define hifi_EntityTreeSendThread_h

#include <unordered_set>

#include "../octree/OctreeSendThread.h"

#include <DiffTraversal.h>
#include <EntityPriorityQueue.h>
#include <shared/ConicalViewFrustum.h>


class EntityNodeData;
class EntityItem;

class EntityTreeSendThread : public OctreeSendThread {
    Q_OBJECT

public:
    EntityTreeSendThread(OctreeServer* myServer, const SharedNodePointer& node);

protected:
    bool traverseTreeAndSendContents(SharedNodePointer node, OctreeQueryNode* nodeData,
            bool viewFrustumChanged, bool isFullScene) override;

private slots:
    void resetState(); // clears our known state forcing entities to appear unsent

private:
    // the following two methods return booleans to indicate if any extra flagged entities were new additions to set
    bool addAncestorsToExtraFlaggedEntities(const QUuid& filteredEntityID, EntityItem& entityItem, EntityNodeData& nodeData);
    bool addDescendantsToExtraFlaggedEntities(const QUuid& filteredEntityID, EntityItem& entityItem, EntityNodeData& nodeData);

    void logMisplacedEntities(const EntityTreeElementPointer& root);
    DiffTraversal::Type startNewTraversal(const DiffTraversal::View& viewFrustum, EntityTreeElementPointer root, bool forceFirstPass = false);
    bool traverseTreeAndBuildNextPacketPayload(EncodeBitstreamParams& params, const QJsonObject& jsonFilters) override;

    void preDistributionProcessing() override;
    bool hasSomethingToSend(OctreeQueryNode* nodeData) override { return !_sendQueue.empty(); }
    bool shouldStartNewTraversal(OctreeQueryNode* nodeData, bool viewFrustumChanged) override { return viewFrustumChanged || _traversal.finished(); }
    size_t queuedEntityCount() const override { return _sendQueue.size(); }

    DiffTraversal _traversal;
    quint64 _traversalStartTime { 0 };   // diagnostics: when the current traversal began and the entities sent since
    int _entitiesSinceMark { 0 };
    quint64 _lastCompletionLog { 0 };
    EntityPriorityQueue _sendQueue;
    std::unordered_map<EntityItem*, uint64_t> _knownState;
    // ancestors and descendants flagged since the last traversal start; the next traversal start puts them on the queue
    std::vector<EntityItemPointer> _newExtraEntities;
    bool _tracksFamilies { false };   // the receiver is one of the domain's own servers (the constructor)
    quint64 _diagLastLog { 0 };       // TIVOLI_STREAM_DIAG: the last periodic line's time
    int _diagSentSinceLog { 0 };      // TIVOLI_STREAM_DIAG: entities written out since that line
    int _diagDroppedProperties { 0 }; // TIVOLI_STREAM_DIAG: entities whose last properties were too big to send, so far

    // an entity whose remaining properties don't fit even an empty packet: they are dropped for this viewer, once
    void dropUnsendableProperties(const EntityItem& entity, bool destinationCanGetAndSetPrivateUserData);

    // packet construction stuff
    EntityTreeElementExtraEncodeDataPointer _extraEncodeData { new EntityTreeElementExtraEncodeData() };
    int32_t _numEntitiesOffset { 0 };
    uint16_t _numEntities { 0 };

private slots:
    void editingEntityPointer(const EntityItemPointer& entity);
    void deletingEntityPointer(EntityItem* entity);
};

#endif // hifi_EntityTreeSendThread_h
