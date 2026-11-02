//
//  EntityTreeRenderer.h
//  interface/src
//
//  Created by Brad Hefta-Gaub on 12/6/13.
//
//  Modified by Caitlyn Meeks on 12/25/19
//  Copyright 2019 Tivoli Cloud VR
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: updateChangedEntities, the render-update budget's counters and getters (getPrevNumEntityUpdates through
// getLastMovingAssemblyInViewTime) and the members the budget keeps (_avgRenderableUpdateCost through
// _priorityRenderablesToUpdate).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.

#ifndef hifi_EntityTreeRenderer_h
#define hifi_EntityTreeRenderer_h

#include <atomic>

#include <QtCore/QSet>
#include <QtCore/QStack>
#include <QtGui/QMouseEvent>

#include <AudioInjectorManager.h>
#include <EntityScriptingInterface.h> // for RayToEntityIntersectionResult
#include <EntityTree.h>
#include <PointerEvent.h>
#include <ScriptCache.h>
#include <TextureCache.h>
#include <OctreeProcessor.h>
#include <render/Forward.h>
#include <workload/Space.h>
#include "MeshPartPayload.h"

class AbstractScriptingServicesInterface;
class AbstractViewStateInterface;
class Model;
class ScriptEngine;
class ZoneEntityItem;
class EntityItem;

namespace render { namespace entities {
    class EntityRenderer;
    using EntityRendererPointer = std::shared_ptr<EntityRenderer>;
    using EntityRendererWeakPointer = std::weak_ptr<EntityRenderer>;

} }

using EntityRenderer = render::entities::EntityRenderer;
using EntityRendererPointer = render::entities::EntityRendererPointer;
using EntityRendererWeakPointer = render::entities::EntityRendererWeakPointer;
class Model;
using ModelPointer = std::shared_ptr<Model>;
using ModelWeakPointer = std::weak_ptr<Model>;

using CalculateEntityLoadingPriority = std::function<float(const EntityItem& item)>;

// Generic client side Octree renderer class.
class EntityTreeRenderer : public OctreeProcessor, public Dependency {
    Q_OBJECT
public:

    QList<EntityItemID> _currentlySelectedEntities; // selected by edit

    bool _isEditMode = false;
    bool _ghostingAllowed = true;
    bool _disableWebEntities = false;

    void setCurrentlySelectedItems(QList<EntityItemID>& value) { _currentlySelectedEntities = value;}
    bool checkIfEntityIsSelected(const EntityItemID& value) { return _currentlySelectedEntities.contains(value); }
    bool isAnythingSelected() { return !_currentlySelectedEntities.isEmpty(); }
    void clearEntitySelections() { _currentlySelectedEntities.clear(); }

    void setSceneIsReady(bool value) { _sceneIsReady = value; }
    bool getSceneIsReady() { return _sceneIsReady; }

    void setDisableWebEntities(bool value) { _disableWebEntities = value; }
    bool getDisableWebEntities() { return _disableWebEntities; }
    
    void setIsEditMode(bool modeState) { _isEditMode = modeState; }
    // while suspended nothing is zone-culled (build mode shows everything); the decision is made again when this
    // changes, so the culled come back, and are culled again once it ends
    void setZoneCullingSuspended(bool suspended);
    bool getGhostingAllowed() { return _ghostingAllowed; }

    // Zone culling (ZONE-CULLING.md; master plan entry 13). While the avatar stands in a zone whose culling mode is
    // "outside", the world's entities outside that zone are not drawn. The innermost zone whose mode is not "inherit"
    // decides, as for a zone's other settings. The decision is made once, when that changes (decideZoneCulling sets a
    // flag on each renderer); the renderers whose flag flipped are refreshed through a time-boxed lane beside the
    // render-update budget (drainZoneCullFlips, nearest first); and an entity that updates while a zone is active
    // (it moved, or it is new) tests itself against the zone's box. A world without a culling zone pays nothing.
    struct ZoneCullState {
        bool active { false };      // entities outside the zone are hidden
        bool dontLoad { false };    // and the entity query stops at the zone (the load zone, entry 13 (g))
        QUuid zoneId;
        glm::vec3 position { 0.0f };        // the zone's box: its world centre, orientation and half dimensions
        glm::quat rotation;
        glm::vec3 halfDimensions { 0.0f };
        // whether a sphere touches the box: the test in the zone's own frame, so a turned zone culls by its true shape
        bool touches(const glm::vec3& center, float radius) const;
        // how far the box reaches from a point: the distance to its farthest corner (the load zone's query limit)
        float reachFrom(const glm::vec3& point) const;
    };
    const ZoneCullState& getZoneCullState() const { return _zoneCullState; }
    // the entities zone culling may hide: the world's own, never the app's local ones, worn ones, or a zone, gizmo,
    // grid or material
    static bool isZoneCullable(const EntityItemPointer& entity);
    // a zone's culling mode, place or shape changed: the decision is made again at the next check
    void zoneCullingZoneChanged(const EntityItemID& id);

    bool _bypassPrioritySorting = false;
    bool getBypassPrioritySorting() { return _bypassPrioritySorting; }
    void setBypassPrioritySorting(bool enabled) {
        qDebug() << "CPM - SET BPS " << enabled;
        _bypassPrioritySorting = enabled;
    }

    static void setEntitiesShouldFadeFunction(std::function<bool()> func) { _entitiesShouldFadeFunction = func; }
    static std::function<bool()> getEntitiesShouldFadeFunction() { return _entitiesShouldFadeFunction; }

    EntityTreeRenderer(bool wantScripts, AbstractViewStateInterface* viewState,
                                AbstractScriptingServicesInterface* scriptingServices);
    virtual ~EntityTreeRenderer();

    QSharedPointer<EntityTreeRenderer> getSharedFromThis() {
        return qSharedPointerCast<EntityTreeRenderer>(sharedFromThis());
    }

    virtual char getMyNodeType() const override { return NodeType::EntityServer; }
    virtual PacketType getMyQueryMessageType() const override { return PacketType::EntityQuery; }
    virtual PacketType getExpectedPacketType() const override { return PacketType::EntityData; }

    // Returns the priority at which an entity should be loaded. Higher values indicate higher priority.
    static CalculateEntityLoadingPriority getEntityLoadingPriorityOperator() { return _calculateEntityLoadingPriorityFunc; }
    static float getEntityLoadingPriority(const EntityItem& item) { return _calculateEntityLoadingPriorityFunc(item); }
    static void setEntityLoadingPriorityFunction(CalculateEntityLoadingPriority fn) { _calculateEntityLoadingPriorityFunc = fn; }

    void setMouseRayPickID(unsigned int rayPickID) { _mouseRayPickID = rayPickID; }
    unsigned int getMouseRayPickID() { return _mouseRayPickID; }
    void setMouseRayPickResultOperator(std::function<RayToEntityIntersectionResult(unsigned int)> getPrevRayPickResultOperator) { _getPrevRayPickResultOperator = getPrevRayPickResultOperator;  }
    void setSetPrecisionPickingOperator(std::function<void(unsigned int, bool)> setPrecisionPickingOperator) { _setPrecisionPickingOperator = setPrecisionPickingOperator; }

    //void forceUpdateStaticEntities(const render::ScenePointer& scene, render::Transaction& transaction);
    void shutdown();
    void preUpdate();
    void update(bool simulate);
    // after the avatars' update: the entities they just moved (a held tablet's screen, held things, avatar entities)
    // updated in the scene now, so they're drawn this frame with their parents; only Model entities redraw as they're
    // moved, the rest waited for the next frame's update and trailed (Caity's Quest test, 10-28)
    void updateAvatarChildren();

    EntityTreePointer getTree() { return std::static_pointer_cast<EntityTree>(_tree); }

    void processEraseMessage(ReceivedMessage& message, const SharedNodePointer& sourceNode);

    virtual void init() override;

    /// clears the tree
    virtual void clearDomainAndNonOwnedEntities() override;
    virtual void clear() override;

    /// reloads the entity scripts, calling unload and preload
    void reloadEntityScripts();

    void fadeOutRenderable(const EntityRendererPointer& renderable);

    // event handles which may generate entity related events
    QUuid mousePressEvent(QMouseEvent* event);
    void mouseReleaseEvent(QMouseEvent* event);
    void mouseDoublePressEvent(QMouseEvent* event);
    void mouseMoveEvent(QMouseEvent* event);

    /// connect our signals to anEntityScriptingInterface for firing of events related clicking,
    /// hovering over, and entering entities
    void connectSignalsToSlots(EntityScriptingInterface* entityScriptingInterface);

    // For Scene.shouldRenderEntities
    QList<EntityItemID>& getEntitiesLastInScene() { return _entityIDsLastInScene; }

    std::pair<bool, bool> getZoneInteractionProperties();

    bool wantsKeyboardFocus(const EntityItemID& id) const;
    QObject* getEventHandler(const EntityItemID& id);
    bool wantsHandControllerPointerEvents(const EntityItemID& id) const;
    void setProxyWindow(const EntityItemID& id, QWindow* proxyWindow);
    void setCollisionSound(const EntityItemID& id, const SharedSoundPointer& sound);
    EntityItemPointer getEntity(const EntityItemID& id);
    void deleteEntity(const EntityItemID& id) const;
    void onEntityChanged(const EntityItemID& id);

    // Model meshes waiting behind their impostor cards (ENTITY-STREAMING.md 3.1): a model's renderer puts its entity
    // here, with the sphere of the card it waits behind, while the sphere looks smaller than the load angle from the
    // main camera; each update, the ones that have come to look bigger (or that physics wants the mesh of, or all of
    // them when the angle is 0) update, and fetch it. Main thread, but for wakeDeferredModelMesh.
    void deferModelMesh(const EntityItemID& id, const glm::vec3& centerWS, float radius);
    void undeferModelMesh(const EntityItemID& id);
    void wakeDeferredModelMesh(const EntityItemID& id);
    // the main camera's position and the load angle, as the squared tangent of half of it (0: no mesh waits), set by
    // the application each frame
    void setImpostorLoadView(const glm::vec3& cameraPosition, float loadHalfTanSq);
    const glm::vec3& getImpostorLoadPosition() const { return _impostorLoadPosition; }
    float getImpostorLoadHalfTanSq() const { return _impostorLoadHalfTanSq; }

    // Access the workload Space
    workload::SpacePointer getWorkloadSpace() const { return _space; }

    EntityEditPacketSender* getPacketSender();

    static void setAddMaterialToEntityOperator(std::function<bool(const QUuid&, graphics::MaterialLayer, const std::string&)> addMaterialToEntityOperator) { _addMaterialToEntityOperator = addMaterialToEntityOperator; }
    static void setRemoveMaterialFromEntityOperator(std::function<bool(const QUuid&, graphics::MaterialPointer, const std::string&)> removeMaterialFromEntityOperator) { _removeMaterialFromEntityOperator = removeMaterialFromEntityOperator; }
    static bool addMaterialToEntity(const QUuid& entityID, graphics::MaterialLayer material, const std::string& parentMaterialName);
    static bool removeMaterialFromEntity(const QUuid& entityID, graphics::MaterialPointer material, const std::string& parentMaterialName);

    static void setAddMaterialToAvatarOperator(std::function<bool(const QUuid&, graphics::MaterialLayer, const std::string&)> addMaterialToAvatarOperator) { _addMaterialToAvatarOperator = addMaterialToAvatarOperator; }
    static void setRemoveMaterialFromAvatarOperator(std::function<bool(const QUuid&, graphics::MaterialPointer, const std::string&)> removeMaterialFromAvatarOperator) { _removeMaterialFromAvatarOperator = removeMaterialFromAvatarOperator; }
    static bool addMaterialToAvatar(const QUuid& avatarID, graphics::MaterialLayer material, const std::string& parentMaterialName);
    static bool removeMaterialFromAvatar(const QUuid& avatarID, graphics::MaterialPointer material, const std::string& parentMaterialName);

    // FROZEN (vehicle path): see the note at the top of this file.
    int getPrevNumEntityUpdates() const { return _prevNumEntityUpdates; }
    int getPrevTotalNeededEntityUpdates() const { return _prevTotalNeededEntityUpdates; }

    // The render-update budget's counters (team/performance/INSTRUMENTS-PLAN.md, item 2), a few integers kept every
    // frame: the last pass's values, and the maxima over the last whole second for a Stats line that holds still.
    // "Needed" and "done" above are the budgeted lane (AUTOMATIC entities, with the last frame's leftovers); the
    // carried lane (a moving entity's parts, refreshed with it outside the budget), the avatars' lane and the
    // PRIORITIZED lane are counted apart. Main thread.
    uint64_t getRenderUpdateFrame() const { return _renderUpdateFrame; }
    int getPrevNumCarriedEntityUpdates() const { return _prevNumCarriedEntityUpdates; }
    int getPrevNumAvatarChildUpdates() const { return _prevNumAvatarChildUpdates; }
    int getPrevNumPriorityEntityUpdates() const { return _prevNumPriorityEntityUpdates; }
    int getPrevNumDeferredEntityUpdates() const { return _prevNumDeferredEntityUpdates; }
    int getPrevOldestDeferredFrames() const { return _prevOldestDeferredFrames; }
    int getMaxDeferredEntityUpdatesLastSecond() const { return _maxDeferredLastSecond; }
    int getMaxDeferredFramesLastSecond() const { return _maxDeferredFramesLastSecond; }
    // An entity's staleness for the inspector: how many frames it has waited for its render update, 0 when it is
    // drawn as its entity is, -1 when it has no renderable (not drawn); and how many frames late its last refresh
    // came. Main thread, no lock (the map is the main thread's own); cheap enough to poll.
    int getRenderUpdateFramesBehind(const EntityItemID& id) const;
    int getRenderUpdateLastLateFrames(const EntityItemID& id) const;
    // when a moving assembly (a vehicle, a train car, a frame, a platform: a moving entity with parts, the carried
    // lane) was last refreshed inside the view, usecTimestampNow(); 0 when never. The refresh regime reads it: a ride
    // in view keeps the full rate while the window is unfocused (Caity, 2026-10-31). Main thread.
    uint64_t getLastMovingAssemblyInViewTime() const { return _lastMovingAssemblyInView; }

signals:
    /// The world `world` (its entities engine's id, ScriptCaller::world() on its thread) was left, at a world change or
    /// a content reload, before its scripts unload and its engine stops: its code gets nothing more done
    /// (ScriptCaller::retireWorld), and what its scripts made outside the engine (tagged with that id) goes now, each
    /// owner removing its own.
    void worldLeft(int world);
    /// The entities engine of the world `world` has stopped (or was given up on): nothing of that world's scripts runs
    /// any more. Its owners remove what they hold of the world again (nothing, as the world's calls are refused since
    /// worldLeft).
    void worldScriptsStopped(int world);
    void enterEntity(const EntityItemID& entityItemID);
    void leaveEntity(const EntityItemID& entityItemID);
    void collisionWithEntity(const EntityItemID& idA, const EntityItemID& idB, const Collision& collision);

public slots:
    void addingEntity(const EntityItemID& entityID);
    void deletingEntity(const EntityItemID& entityID);
    void entityScriptChanging(const EntityItemID& entityID, const bool reload);
    void entityCollisionWithEntity(const EntityItemID& idA, const EntityItemID& idB, const Collision& collision);
    void updateEntityRenderStatus(bool shouldRenderEntities);
    void updateZone(const EntityItemID& id);
    void connectedToDomain();
    // the tree refuses content addresses that name a file on this computer unless the world is local
    // (ScriptNetworkPolicy::isWorldLocal, the connected socket's address); decided again at every domain change
    void updateContentFromThisComputer();

    // optional slots that can be wired to menu items
    void setDisplayModelBounds(bool value) { _displayModelBounds = value; }
    void setPrecisionPicking(bool value) { _setPrecisionPickingOperator(_mouseRayPickID, value); }
    EntityRendererPointer renderableForEntityId(const EntityItemID& id) const;
    render::ItemID renderableIdForEntityId(const EntityItemID& id) const;

    void handleSpaceUpdate(std::pair<int32_t, glm::vec4> proxyUpdate);
    void forceRecheckEntities();

protected:
    virtual OctreePointer createTree() override {
        EntityTreePointer newTree = EntityTreePointer(new EntityTree(true));
        newTree->createRootElement();
        return newTree;
    }

private:
    void addPendingEntities(const render::ScenePointer& scene, render::Transaction& transaction);
    // FROZEN (vehicle path): see the note at the top of this file.
    void updateChangedEntities(const render::ScenePointer& scene, render::Transaction& transaction);
    EntityRendererPointer renderableForEntity(const EntityItemPointer& entity) const { return renderableForEntityId(entity->getID()); }
    render::ItemID renderableIdForEntity(const EntityItemPointer& entity) const { return renderableIdForEntityId(entity->getID()); }

    void resetEntitiesScriptEngine();
    void replaceEntitiesScriptEngine();
    void retireEntitiesScriptEngine(const ScriptEnginePointer& old);
    void entitiesEngineStopped(int world);
    void interruptIfStillRunning(QWeakPointer<ScriptEngine> old, int attempt);
    void restartKeptEntityScripts();

    void findBestZoneAndMaybeContainingEntities(QSet<EntityItemID>& entitiesContainingAvatar);

    bool applyLayeredZones();
    void stopDomainAndNonOwnedEntities();

    void checkAndCallPreload(const EntityItemID& entityID, bool reload = false, bool unloadFirst = false);

    EntityItemID _currentHoverOverEntityID;
    EntityItemID _currentClickingOnEntityID;

    QScriptValueList createEntityArgs(const EntityItemID& entityID);
    void checkEnterLeaveEntities();
    void leaveDomainAndNonOwnedEntities();
    void leaveAllEntities();

    glm::vec3 _avatarPosition { 0.0f };
    bool _forceRecheckEntities { true };
    bool _zoneCullingSuspended { false };
    ZoneCullState _zoneCullState;
    bool _zoneCullDirty { false };                    // the deciding zone moved, turned, resized or changed mode
    std::vector<EntityRendererPointer> _zoneCullFlips; // renderers whose flag flipped, nearest first, not yet refreshed
    size_t _zoneCullFlipsNext { 0 };
    void updateZoneCullState();
    void decideZoneCulling();
    void drainZoneCullFlips(const render::ScenePointer& scene, render::Transaction& transaction);
    void resetZoneCulling();
    bool _sceneIsReady = false;
    QSet<EntityItemID> _currentEntitiesInside;

    bool _wantScripts;
    ScriptEnginePointer _entitiesScriptEngine;
    QSet<int> _entitiesEnginesStopping;  // the world ids of engines of worlds left not stopped yet (retireEntitiesScriptEngine)

    void playEntityCollisionSound(const EntityItemPointer& entity, const Collision& collision);

    bool _lastPointerEventValid;
    PointerEvent _lastPointerEvent;
    AbstractViewStateInterface* _viewState;
    AbstractScriptingServicesInterface* _scriptingServices;
    bool _displayModelBounds;

    bool _shuttingDown { false };

    void updateDeferredModelMeshes(const render::ScenePointer& scene, render::Transaction& transaction);
    struct DeferredModelMesh {
        glm::vec3 center;
        float radius;
        bool wake;
    };
    mutable std::mutex _deferredModelMeshesLock;
    std::unordered_map<EntityItemID, DeferredModelMesh> _deferredModelMeshes;
    glm::vec3 _impostorLoadPosition { 0.0f };
    float _impostorLoadHalfTanSq { 0.0f };

    QMultiMap<QUrl, EntityItemID> _waitingOnPreload;

    unsigned int _mouseRayPickID;
    std::function<RayToEntityIntersectionResult(unsigned int)> _getPrevRayPickResultOperator;
    std::function<void(unsigned int, bool)> _setPrecisionPickingOperator;

    class LayeredZone {
    public:
        LayeredZone(std::shared_ptr<ZoneEntityItem> zone) : zone(zone), id(zone->getID()), volume(zone->getVolumeEstimate()) {}

        // We need to sort on volume AND id so that different clients sort zones with identical volumes the same way
        bool operator<(const LayeredZone& r) const { return volume < r.volume || (volume == r.volume && id < r.id); }
        bool operator==(const LayeredZone& r) const { return zone.lock() && zone.lock() == r.zone.lock(); }
        bool operator!=(const LayeredZone& r) const { return !(*this == r); }
        bool operator<=(const LayeredZone& r) const { return (*this < r) || (*this == r); }

        std::weak_ptr<ZoneEntityItem> zone;
        QUuid id;
        float volume;
    };

    class LayeredZones : public std::vector<LayeredZone> {
    public:
        bool clearDomainAndNonOwnedZones();

        void sort() { std::sort(begin(), end(), std::less<LayeredZone>()); }
        bool equals(const LayeredZones& other) const;
        bool update(std::shared_ptr<ZoneEntityItem> zone, const glm::vec3& position, EntityTreeRenderer* entityTreeRenderer);

        void appendRenderIDs(render::ItemIDs& list, EntityTreeRenderer* entityTreeRenderer) const;
        std::pair<bool, bool> getZoneInteractionProperties() const;
        //ZoneCullingMode getZoneCullingProperties() const;
    };

    LayeredZones _layeredZones;
    uint64_t _lastZoneCheck { 0 };
    const uint64_t ZONE_CHECK_INTERVAL = USECS_PER_MSEC * 1000; // ~100hz
    const float ZONE_CHECK_DISTANCE = 0.001f;

    // FROZEN (vehicle path): see the note at the top of this file.
    float _avgRenderableUpdateCost { 0.0f };

    ReadWriteLockable _changedEntitiesGuard;
    std::unordered_set<EntityItemID> _changedEntities;
    int _prevNumEntityUpdates { 0 };
    int _prevTotalNeededEntityUpdates { 0 };
    uint64_t _renderUpdateFrame { 0 };   // one per updateChangedEntities pass (one a frame)
    int _prevNumCarriedEntityUpdates { 0 };
    int _prevNumAvatarChildUpdates { 0 };
    int _prevNumPriorityEntityUpdates { 0 };
    int _prevNumDeferredEntityUpdates { 0 };
    int _prevOldestDeferredFrames { 0 };
    int _maxDeferredThisSecond { 0 };
    int _maxDeferredFramesThisSecond { 0 };
    int _maxDeferredLastSecond { 0 };
    int _maxDeferredFramesLastSecond { 0 };
    uint64_t _countersSecondStart { 0 };
    uint64_t _lastMovingAssemblyInView { 0 };
    std::atomic<int> _contentFromThisComputerLogged { -1 };   // the last decision logged (-1 none, 0 allowed, 1 refused)
    uint64_t _lastMovingAssemblyTest { 0 };

    std::unordered_set<EntityRendererPointer> _renderablesToUpdate;
    std::unordered_set<EntityRendererPointer> _priorityRenderablesToUpdate;
    QSet<EntityRendererPointer> _staticRenderablesToUpdate;
    std::unordered_map<EntityItemID, EntityRendererPointer> _entitiesInScene;
    std::unordered_map<EntityItemID, EntityItemWeakPointer> _entitiesToAdd;

    // For Scene.shouldRenderEntities
    QList<EntityItemID> _entityIDsLastInScene;

    static int _entitiesScriptEngineCount;
    static CalculateEntityLoadingPriority _calculateEntityLoadingPriorityFunc;
    static std::function<bool()> _entitiesShouldFadeFunction;

    mutable std::mutex _spaceLock;
    workload::SpacePointer _space{ new workload::Space() };
    workload::Transaction::Updates _spaceUpdates;

    static std::function<bool(const QUuid&, graphics::MaterialLayer, const std::string&)> _addMaterialToEntityOperator;
    static std::function<bool(const QUuid&, graphics::MaterialPointer, const std::string&)> _removeMaterialFromEntityOperator;
    static std::function<bool(const QUuid&, graphics::MaterialLayer, const std::string&)> _addMaterialToAvatarOperator;
    static std::function<bool(const QUuid&, graphics::MaterialPointer, const std::string&)> _removeMaterialFromAvatarOperator;

};


#endif // hifi_EntityTreeRenderer_h
