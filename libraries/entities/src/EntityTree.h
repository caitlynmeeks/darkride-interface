//
//  EntityTree.h
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 12/4/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_EntityTree_h
#define hifi_EntityTree_h

#include <QSet>
#include <QMutex>
#include <atomic>
#include <QVector>

#include <Octree.h>
#include <SpatialParentFinder.h>

#include "AddEntityOperator.h"
#include "EntityTreeElement.h"
#include "DeleteEntityOperator.h"
#include "MovingEntitiesOperator.h"

class EntityTree;
using EntityTreePointer = std::shared_ptr<EntityTree>;

class EntitySimulation;

namespace EntityQueryFilterSymbol {
    static const QString NonDefault = "+";
}

class NewlyCreatedEntityHook {
public:
    virtual void entityCreated(const EntityItem& newEntity, const SharedNodePointer& senderNode) = 0;
};

class SendEntitiesOperationArgs {
public:
    glm::vec3 root;
    QString entityHostType;
    EntityTree* ourTree;
    EntityTreePointer otherTree;
    QHash<EntityItemID, EntityItemID>* map;
};

class EntityTree : public Octree, public SpatialParentTree {
    Q_OBJECT
public:
    enum FilterType {
        Add,
        Edit,
        Physics,
        Delete
    };
    EntityTree(bool shouldReaverage = false);

    // How many adds and edits the entity server has measured against the stream's room (processEditPacketData: only
    // those of streamCheckSize() or more), for TIVOLI_STREAM_DIAG
    static uint64_t streamChecksRun() { return _streamChecksRun; }
    virtual ~EntityTree();

    void createRootElement();


    void setEntityMaxTmpLifetime(float maxTmpEntityLifetime) { _maxTmpEntityLifetime = maxTmpEntityLifetime; }
    void setEntityScriptSourceWhitelist(const QString& entityScriptSourceWhitelist);

    /// Implements our type specific root element factory
    virtual OctreeElementPointer createNewElement(unsigned char* octalCode = NULL) override;

    /// Type safe version of getRoot()
    EntityTreeElementPointer getRoot() {
        if (!_rootElement) {
            createRootElement();
        }
        return std::static_pointer_cast<EntityTreeElement>(_rootElement);
    }


    virtual void eraseDomainAndNonOwnedEntities() override;
    /// The visitor's avatar entities a world's script made: they go when the visitor leaves the world.
    QVector<EntityItemID> getWorldsAvatarEntityIDs() const;
    /// The entities kept when the visitor leaves a world (EntityItem::isKeptOnWorldChange) that have a script.
    QVector<EntityItemID> getKeptEntityIDsWithScripts() const;
    virtual void eraseAllOctreeElements(bool createNewRoot = true) override;

    virtual void readBitstreamToTree(const unsigned char* bitstream,
            uint64_t bufferSizeBytes, ReadBitstreamToTreeParams& args) override;
    int readEntityDataFromBuffer(const unsigned char* data, int bytesLeftToRead, ReadBitstreamToTreeParams& args);

    // These methods will allow the OctreeServer to send your tree inbound edit packets of your
    // own definition. Implement these to allow your octree based server to support editing
    virtual PacketType expectedDataPacketType() const override { return PacketType::EntityData; }
    virtual bool handlesEditPacketType(PacketType packetType) const override;
    void fixupTerseEditLogging(EntityItemProperties& properties, QList<QString>& changedProperties);
    virtual int processEditPacketData(ReceivedMessage& message, const unsigned char* editData, int maxLength,
                                      const SharedNodePointer& senderNode) override;

    virtual EntityItemID evalRayIntersection(const glm::vec3& origin, const glm::vec3& direction,
        QVector<EntityItemID> entityIdsToInclude, QVector<EntityItemID> entityIdsToDiscard,
        PickFilter searchFilter, OctreeElementPointer& element, float& distance,
        BoxFace& face, glm::vec3& surfaceNormal, QVariantMap& extraInfo,
        Octree::lockType lockType = Octree::TryLock, bool* accurateResult = NULL);

    virtual EntityItemID evalParabolaIntersection(const PickParabola& parabola,
        QVector<EntityItemID> entityIdsToInclude, QVector<EntityItemID> entityIdsToDiscard,
        PickFilter searchFilter, OctreeElementPointer& element, glm::vec3& intersection,
        float& distance, float& parabolicDistance, BoxFace& face, glm::vec3& surfaceNormal, QVariantMap& extraInfo,
        Octree::lockType lockType = Octree::TryLock, bool* accurateResult = NULL);

    virtual bool rootElementHasData() const override { return true; }

    virtual void releaseSceneEncodeData(OctreeElementExtraEncodeData* extraEncodeData) const override;

    // Why preUpdate() and update()?
    // Because sometimes we need to do stuff between the two.
    void preUpdate() override;
    void update(bool simulate = true) override;

    // The newer API...
    void postAddEntity(EntityItemPointer entityItem);

    EntityItemPointer addEntity(const EntityItemID& entityID, const EntityItemProperties& properties, bool isClone = false, const bool isImport = false);

    // use this method if you only know the entityID
    // isPhysics: the edit is a simulation owner's update (PacketType::EntityPhysics), not an explicit edit (round 7)
    bool updateEntity(const EntityItemID& entityID, const EntityItemProperties& properties, const SharedNodePointer& senderNode = SharedNodePointer(nullptr),
                      bool isPhysics = false);

    // check if the avatar is a child of this entity, If so set the avatar parentID to null
    void unhookChildAvatar(const EntityItemID entityID);
    void cleanupCloneIDs(const EntityItemID& entityID);
    void deleteEntity(const EntityItemID& entityID, bool force = false, bool ignoreWarnings = true);

    void deleteEntitiesByID(const std::vector<EntityItemID>& entityIDs, bool force = false, bool ignoreWarnings = true);
    // removes, on this computer only, those of the ids that are the given avatar's own avatar entities (with their local
    // and my-avatar children, as any delete here); other ids are left as they are and nothing goes to the entity server
    void deleteAvatarEntitiesOf(const QUuid& avatarID, const std::vector<EntityItemID>& entityIDs);
    void deleteEntitiesByPointer(const std::vector<EntityItemPointer>& entities);

    EntityItemPointer findEntityByID(const QUuid& id) const;
    EntityItemPointer findEntityByEntityItemID(const EntityItemID& entityID) const;
    virtual SpatiallyNestablePointer findByID(const QUuid& id) const override { return findEntityByID(id); }

    EntityItemID assignEntityID(const EntityItemID& entityItemID); /// Assigns a known ID for a creator token ID

    QUuid evalClosestEntity(const glm::vec3& position, float targetRadius, PickFilter searchFilter);
    void evalEntitiesInSphere(const glm::vec3& center, float radius, PickFilter searchFilter, QVector<QUuid>& foundEntities);
    void evalEntitiesInSphereWithType(const glm::vec3& center, float radius, EntityTypes::EntityType type, PickFilter searchFilter, QVector<QUuid>& foundEntities);
    void evalEntitiesInSphereWithName(const glm::vec3& center, float radius, const QString& name, bool caseSensitive, PickFilter searchFilter, QVector<QUuid>& foundEntities);
    void evalEntitiesInSphereWithTag(const glm::vec3& center, float radius, const QString& tagName, PickFilter searchFilter, QVector<QUuid>& foundEntities);
    void evalEntitiesInCube(const AACube& cube, PickFilter searchFilter, QVector<QUuid>& foundEntities);
    void evalEntitiesInBox(const AABox& box, PickFilter searchFilter, QVector<QUuid>& foundEntities);
    void evalEntitiesInFrustum(const ViewFrustum& frustum, PickFilter searchFilter, QVector<QUuid>& foundEntities);

    void addNewlyCreatedHook(NewlyCreatedEntityHook* hook);
    void removeNewlyCreatedHook(NewlyCreatedEntityHook* hook);

    bool hasAnyDeletedEntities() const { 
        QReadLocker locker(&_recentlyDeletedEntitiesLock);
        return _recentlyDeletedEntityItemIDs.size() > 0;
    }

    bool hasEntitiesDeletedSince(quint64 sinceTime);
    static quint64 getAdjustedConsiderSince(quint64 sinceTime);

    QMultiMap<quint64, QUuid> getRecentlyDeletedEntityIDs() const { 
        QReadLocker locker(&_recentlyDeletedEntitiesLock);
        return _recentlyDeletedEntityItemIDs;
    }

    void forgetEntitiesDeletedBefore(quint64 sinceTime);

    int processEraseMessage(ReceivedMessage& message, const SharedNodePointer& sourceNode);
    int processEraseMessageDetails(const QByteArray& buffer, const SharedNodePointer& sourceNode);

    // whether a node's rights cover changing an entity (a temporary rez right: temporary entities only), and deleting it
    // with its children (unlocked too)
    static bool senderMayChange(const SharedNodePointer& senderNode, const EntityItemPointer& entity);
    static bool senderMayDelete(const SharedNodePointer& senderNode, const EntityItemPointer& entity);
    bool shouldEraseEntity(EntityItemID entityID, const SharedNodePointer& sourceNode);


    EntityTreeElementPointer getContainingElement(const EntityItemID& entityItemID)  /*const*/;
    void addEntityMapEntry(EntityItemPointer entity);
    void clearEntityMapEntry(const EntityItemID& id);
    void debugDumpMap();
    virtual void dumpTree() override;
    virtual void pruneTree() override;

    static QByteArray remapActionDataIDs(QByteArray actionData, QHash<EntityItemID, EntityItemID>& map);

    QVector<EntityItemID> sendEntities(EntityEditPacketSender* packetSender, EntityTreePointer localTree,
                                       const QString& entityHostType, float x, float y, float z);

    void entityChanged(EntityItemPointer entity);

    void emitEntityScriptChanging(const EntityItemID& entityItemID, bool reload);
    void emitEntityServerScriptChanging(const EntityItemID& entityItemID, bool reload);

    void setSimulation(EntitySimulationPointer simulation);
    EntitySimulationPointer getSimulation() const { return _simulation; }

    bool wantEditLogging() const { return _wantEditLogging; }
    void setWantEditLogging(bool value) { _wantEditLogging = value; }

    bool wantTerseEditLogging() const { return _wantTerseEditLogging; }
    void setWantTerseEditLogging(bool value) { _wantTerseEditLogging = value; }

    virtual bool writeToMap(QVariantMap& entityDescription, OctreeElementPointer element, bool skipDefaultValues,
                            bool skipThoseWithBadParents) override;
    virtual bool readFromMap(QVariantMap& entityDescription, const bool isImport = false) override;
    virtual bool writeToJSON(QString& jsonString, const OctreeElementPointer& element) override;


    glm::vec3 getContentsDimensions();
    float getContentsLargestDimension();

    virtual void resetEditStats() override {
        _totalEditMessages = 0;
        _totalUpdates = 0;
        _totalCreates = 0;
        _totalDecodeTime = 0;
        _totalLookupTime = 0;
        _totalUpdateTime = 0;
        _totalCreateTime = 0;
        _totalLoggingTime = 0;
    }

    virtual quint64 getAverageDecodeTime() const override { return _totalEditMessages == 0 ? 0 : _totalDecodeTime / _totalEditMessages; }
    virtual quint64 getAverageLookupTime() const override { return _totalEditMessages == 0 ? 0 : _totalLookupTime / _totalEditMessages; }
    virtual quint64 getAverageUpdateTime() const override { return _totalUpdates == 0 ? 0 : _totalUpdateTime / _totalUpdates; }
    virtual quint64 getAverageCreateTime() const override { return _totalCreates == 0 ? 0 : _totalCreateTime / _totalCreates; }
    virtual quint64 getAverageLoggingTime() const override { return _totalEditMessages == 0 ? 0 : _totalLoggingTime / _totalEditMessages; }
    virtual quint64 getAverageFilterTime() const override { return _totalEditMessages == 0 ? 0 : _totalFilterTime / _totalEditMessages; }

    void trackIncomingEntityLastEdited(quint64 lastEditedTime, int bytesRead);
    quint64 getAverageEditDeltas() const
        { return _totalTrackedEdits == 0 ? 0 : _totalEditDeltas / _totalTrackedEdits; }
    quint64 getAverageEditBytes() const
        { return _totalTrackedEdits == 0 ? 0 : _totalEditBytes / _totalTrackedEdits; }
    quint64 getMaxEditDelta() const { return _maxEditDelta; }
    quint64 getTotalTrackedEdits() const { return _totalTrackedEdits; }

    EntityTreePointer getThisPointer() { return std::static_pointer_cast<EntityTree>(shared_from_this()); }

    bool isDeletedEntity(const QUuid& id) {
        QReadLocker locker(&_deletedEntitiesLock);
        return _deletedEntityItemIDs.contains(id);
    }

    // these are used to call through to EntityItems
    Q_INVOKABLE int getJointIndex(const QUuid& entityID, const QString& name) const;
    Q_INVOKABLE QStringList getJointNames(const QUuid& entityID) const;
    Q_INVOKABLE QVariantMap getModelExtras(const QUuid& entityID) const;

    void knowAvatarID(const QUuid& avatarID);
    void forgetAvatarID(const QUuid& avatarID);
    void deleteDescendantsOfAvatar(const QUuid& avatarID);
    void removeFromChildrenOfAvatars(EntityItemPointer entity);

    void addToNeedsParentFixupList(EntityItemPointer entity);

    void notifyNewCollisionSoundURL(const QString& newCollisionSoundURL, const EntityItemID& entityID);

    static const float DEFAULT_MAX_TMP_ENTITY_LIFETIME;

    QUuid getMyAvatarSessionUUID() { return _myAvatar ? _myAvatar->getSessionUUID() : QUuid(); }
    void setMyAvatar(std::shared_ptr<AvatarData> myAvatar) { _myAvatar = myAvatar; }

    void swapStaleProxies(std::vector<int>& proxies) { proxies.swap(_staleProxies); }

    void setIsServerlessMode(bool value) { _serverlessDomain = value; }
    bool isServerlessMode() const { return _serverlessDomain; }

    // The client's tree refuses content addresses that name a file on this computer (a model "file:///Users/me/car.glb"
    // built on its maker's computer reads a file on the visitor's) when the world isn't local: the entity readers empty
    // them as they arrive from the wire, readFromMap as a world file from a link is read, and the avatar paths through
    // EntityContentAddresses::clearThisComputer. Servers, the baker and tools never set it. Any thread sets it, the
    // packet thread reads it.
    void setRefusesContentFromThisComputer(bool refuses);
    bool refusesContentFromThisComputer() const { return _refusesContentFromThisComputer.load(); }
    // one warning per entity per property per world, naming what was refused; the set is cleared with the flag
    void warnRefusedAddresses(const EntityItemID& entityID, const QStringList& propertyNames);
    // how many addresses were refused since the last take (every one, warned or not): an import's status line
    int takeRefusedAddressesCount() { return _refusedAddressesCount.exchange(0); }

    static void setGetEntityObjectOperator(std::function<QObject*(const QUuid&)> getEntityObjectOperator) { _getEntityObjectOperator = getEntityObjectOperator; }
    static QObject* getEntityObject(const QUuid& id);

    static void setTextSizeOperator(std::function<QSizeF(const QUuid&, const QString&)> textSizeOperator) { _textSizeOperator = textSizeOperator; }
    static QSizeF textSize(const QUuid& id, const QString& text);

    static void setEntityClicksCapturedOperator(std::function<bool()> areEntityClicksCapturedOperator) { _areEntityClicksCapturedOperator = areEntityClicksCapturedOperator; }
    static bool areEntityClicksCaptured();

    static void setEmitScriptEventOperator(std::function<void(const QUuid&, const QVariant&)> emitScriptEventOperator) { _emitScriptEventOperator = emitScriptEventOperator; }
    static void emitScriptEvent(const QUuid& id, const QVariant& message);
    
    static void setSendToQmlOperator(std::function<void(const QUuid&, const QVariant&)> sendToQmlOperator) { _sendToQmlOperator = sendToQmlOperator; }
    static void sendToQml(const QUuid& id, const QVariant& message);

    static void setGetUnscaledDimensionsForIDOperator(std::function<glm::vec3(const QUuid&)> getUnscaledDimensionsForIDOperator) { _getUnscaledDimensionsForIDOperator = getUnscaledDimensionsForIDOperator; }
    static glm::vec3 getUnscaledDimensionsForID(const QUuid& id);

    std::map<QString, QString> getNamedPaths() const { return _namedPaths; }

    void updateEntityQueryAACube(SpatiallyNestablePointer object, EntityEditPacketSender* packetSender,
                                 bool force, bool tellServer);

signals:
    void deletingEntity(const EntityItemID& entityID);
    void deletingEntityPointer(EntityItem* entityID);
    void addingEntity(const EntityItemID& entityID);
    void addingEntityPointer(EntityItem* entityID);
    void editingEntityPointer(const EntityItemPointer& entityID);
    void entityScriptChanging(const EntityItemID& entityItemID, const bool reload);
    void entityServerScriptChanging(const EntityItemID& entityItemID, const bool reload);
    void newCollisionSoundURL(const QUrl& url, const EntityItemID& entityID);
    void clearingEntities();

protected:
    static std::atomic<uint64_t> _streamChecksRun;

    void recursivelyFilterAndCollectForDelete(const EntityItemPointer& entity, std::vector<EntityItemPointer>& entitiesToDelete, bool force) const;
    void sendBackRefusedEdit(const EntityItemPointer& entity, const EntityItemProperties& refused,
                             const SharedNodePointer& senderNode);
    void sendBackRefusedTooBigEdit(const EntityItemPointer& entity, const EntityItemProperties& refused);
    void warnTooBigToStream(const EntityItem& probe, const EntityPropertyFlags& tooBig, const EntityItemID& entityID,
                            bool isAdd, const SharedNodePointer& senderNode);
    void processRemovedEntities(const DeleteEntityOperator& theOperator);
    bool updateEntity(EntityItemPointer entity, const EntityItemProperties& properties,
            const SharedNodePointer& senderNode = SharedNodePointer(nullptr), bool isPhysics = false);
    static bool sendEntitiesOperation(const OctreeElementPointer& element, void* extraData);
    static void bumpTimestamp(EntityItemProperties& properties);

    void notifyNewlyCreatedEntity(const EntityItem& newEntity, const SharedNodePointer& senderNode);

    bool isScriptInWhitelist(const QString& scriptURL);

    QReadWriteLock _newlyCreatedHooksLock;
    QVector<NewlyCreatedEntityHook*> _newlyCreatedHooks;

    mutable QReadWriteLock _recentlyDeletedEntitiesLock; /// lock of server side recent deletes
    QMultiMap<quint64, QUuid> _recentlyDeletedEntityItemIDs; /// server side recent deletes

    mutable QReadWriteLock _deletedEntitiesLock; /// lock of client side recent deletes
    QSet<QUuid> _deletedEntityItemIDs; /// client side recent deletes

    void clearDeletedEntities() {
        QWriteLocker locker(&_deletedEntitiesLock);
        _deletedEntityItemIDs.clear();
    }

    void trackDeletedEntity(const QUuid& id) {
        QWriteLocker locker(&_deletedEntitiesLock);
        _deletedEntityItemIDs << id;
    }

    mutable QReadWriteLock _entityMapLock;
    QHash<EntityItemID, EntityItemPointer> _entityMap;

    EntitySimulationPointer _simulation;

    bool _wantEditLogging = false;
    bool _wantTerseEditLogging = false;


    // some performance tracking properties - only used in server trees
    int _totalEditMessages = 0;
    int _totalUpdates = 0;
    int _totalCreates = 0;
    mutable quint64 _totalDecodeTime = 0;
    mutable quint64 _totalLookupTime = 0;
    mutable quint64 _totalUpdateTime = 0;
    mutable quint64 _totalCreateTime = 0;
    mutable quint64 _totalLoggingTime = 0;
    mutable quint64 _totalFilterTime = 0;

    // these performance statistics are only used in the client
    void resetClientEditStats();
    int _totalTrackedEdits = 0;
    quint64 _totalEditBytes = 0;
    quint64 _totalEditDeltas = 0;
    quint64 _maxEditDelta = 0;
    quint64 _treeResetTime = 0;

    void fixupNeedsParentFixups(); // try to hook members of _needsParentFixup to parent instances
    QVector<EntityItemWeakPointer> _needsParentFixup; // entites with a parentID but no (yet) known parent instance
    mutable QReadWriteLock _needsParentFixupLock;

    std::mutex _avatarIDsLock;
    // we maintain a list of avatarIDs to notice when an entity is a child of one.
    QSet<QUuid> _avatarIDs; // IDs of avatars connected to entity server
    std::mutex _childrenOfAvatarsLock;
    QHash<QUuid, QSet<EntityItemID>> _childrenOfAvatars;  // which entities are children of which avatars

    float _maxTmpEntityLifetime { DEFAULT_MAX_TMP_ENTITY_LIFETIME };

    bool filterProperties(const EntityItemPointer& existingEntity, EntityItemProperties& propertiesIn, EntityItemProperties& propertiesOut, bool& wasChanged, FilterType filterType) const;
    bool _hasEntityEditFilter{ false };
    QStringList _entityScriptSourceWhitelist;

    MovingEntitiesOperator _entityMover;
    QHash<EntityItemID, EntityItemPointer> _entitiesToAdd;

private:
    std::shared_ptr<AvatarData> _myAvatar{ nullptr };

    static std::function<QObject*(const QUuid&)> _getEntityObjectOperator;
    static std::function<QSizeF(const QUuid&, const QString&)> _textSizeOperator;
    static std::function<bool()> _areEntityClicksCapturedOperator;
    static std::function<void(const QUuid&, const QVariant&)> _emitScriptEventOperator;
    static std::function<void(const QUuid&, const QVariant&)> _sendToQmlOperator;
    static std::function<glm::vec3(const QUuid&)> _getUnscaledDimensionsForIDOperator;

    std::vector<int32_t> _staleProxies;

    bool _serverlessDomain { false };
    std::atomic<bool> _refusesContentFromThisComputer { false };
public:
    // for TIVOLI_STREAM_DIAG: edits that came stamped ahead of this server's clock (and taken as now), the largest
    // such lead, and refused edits not sent back because their sender was over its budget
    static uint64_t editsStampedAhead() { return _editsStampedAhead; }
    static uint64_t largestStampLeadUsec() { return _largestStampLeadUsec; }
    static uint64_t sendBacksSkipped() { return _sendBacksSkipped; }
private:
    static std::atomic<uint64_t> _editsStampedAhead;
    static std::atomic<uint64_t> _largestStampLeadUsec;
    static std::atomic<uint64_t> _sendBacksSkipped;
    std::atomic<int> _refusedAddressesCount { 0 };
    QMutex _refusedAddressesWarnedLock;
    QSet<QString> _refusedAddressesWarned;   // "<entity id>/<property>"

    std::map<QString, QString> _namedPaths;

    // Return an AACube containing object and all its entity descendants
    AACube updateEntityQueryAACubeWorker(SpatiallyNestablePointer object, EntityEditPacketSender* packetSender,
                                         MovingEntitiesOperator& moveOperator, bool force, bool tellServer);
};

void convertGrabUserDataToProperties(EntityItemProperties& properties);

#endif // hifi_EntityTree_h
