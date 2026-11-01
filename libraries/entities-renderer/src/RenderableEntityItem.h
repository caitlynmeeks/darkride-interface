//
//  RenderableEntityItem.h
//  interface/src
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the render-update budget's stamps: getUpdateTime (the budget's sort reads it), the deferred-since and
// late-frames stamps (getDeferredSinceFrame, setDeferredSinceFrame, getLastLateFrames, setLastLateFrames) and their
// members.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.


#ifndef hifi_RenderableEntityItem_h
#define hifi_RenderableEntityItem_h

#include <atomic>

#include <render/Scene.h>
#include <EntityItem.h>
#include <Sound.h>
#include "AbstractViewStateInterface.h"
#include "EntitiesRendererLogging.h"
#include <graphics-scripting/Forward.h>
#include <RenderHifi.h>
#include "EntityTreeRenderer.h"

class EntityTreeRenderer;
class ImpostorCard;
class ImpostorSwitch;

namespace render { namespace entities {

// Base class for all renderable entities
class EntityRenderer : public QObject, public std::enable_shared_from_this<EntityRenderer>, public PayloadProxyInterface, protected ReadWriteLockable, public scriptable::ModelProvider {
    Q_OBJECT

    using Pointer = std::shared_ptr<EntityRenderer>;

public:
    static void initEntityRenderers();
    static Pointer addToScene(EntityTreeRenderer& renderer, const EntityItemPointer& entity, const ScenePointer& scene, Transaction& transaction);

    // Allow classes to override this to interact with the user
    virtual bool wantsHandControllerPointerEvents() const { return false; }
    virtual bool wantsKeyboardFocus() const { return false; }
    virtual void setProxyWindow(QWindow* proxyWindow) {}
    virtual QObject* getEventHandler() { return nullptr; }
    virtual void emitScriptEvent(const QVariant& message) {}
    virtual void sendToQml(const QVariant& message) {}
    const EntityItemPointer& getEntity() const { return _entity; }
    const ItemID& getRenderItemID() const { return _renderItemID; }

    const SharedSoundPointer& getCollisionSound() { return _collisionSound; }
    void setCollisionSound(const SharedSoundPointer& sound) { _collisionSound = sound; }

    // Handlers for rendering events... executed on the main thread, only called by EntityTreeRenderer, 
    // cannot be overridden or accessed by subclasses
    // `changed` false: nothing the renderer shows has changed (an update forced by the entity server re-sending an
    // unchanged entity), so the scene's item keeps its change stamp
    virtual void updateInScene(const ScenePointer& scene, Transaction& transaction, bool changed = true) final;
    // whether anything the entity shows has changed since the last update: its properties, bound, transform, or what
    // its renderer compares; without the zone-culling bookkeeping needsRenderUpdate adds, which says yes for every
    // visible entity while zone culling is off
    bool entityHasChanged() const { return needsRenderUpdateFromEntity(_entity); }
    virtual bool addToScene(const ScenePointer& scene, Transaction& transaction) final;
    virtual void removeFromScene(const ScenePointer& scene, Transaction& transaction);

    // FROZEN (vehicle path): see the note at the top of this file.
    const uint64_t& getUpdateTime() const { return _updateTime; }

    // FROZEN (vehicle path): see the note at the top of this file.
    // The render-update budget's staleness stamps, kept by EntityTreeRenderer::updateChangedEntities (the
    // instruments plan, item 2): the frame this renderable was first left over by the time budget, 0 while it is
    // drawn as its entity is; and how many frames late its last refresh came (0 when it came the frame the entity
    // changed). Main thread only; the inspector shows them for the selected entity.
    uint64_t getDeferredSinceFrame() const { return _deferredSinceFrame; }
    void setDeferredSinceFrame(uint64_t frame) { _deferredSinceFrame = frame; }
    uint32_t getLastLateFrames() const { return _lastLateFrames; }
    void setLastLateFrames(uint32_t frames) { _lastLateFrames = frames; }

    virtual void addMaterial(graphics::MaterialLayer material, const std::string& parentMaterialName);
    virtual void removeMaterial(graphics::MaterialPointer material, const std::string& parentMaterialName);

    virtual scriptable::ScriptableModelBase getScriptableModel() override { return scriptable::ScriptableModelBase(); }

    static glm::vec4 calculatePulseColor(const glm::vec4& color, const PulsePropertyGroup& pulseProperties, quint64 start);
    static glm::vec3 calculatePulseColor(const glm::vec3& color, const PulsePropertyGroup& pulseProperties, quint64 start);

    // The transform a billboarded entity draws with: turned to the camera (EntityItem::getBillboardRotation) and, while
    // the finished picture is shown flipped left to right (RenderArgs::_mirrored: the selfie view on a flat screen, the
    // mirror camera), mirrored about its own vertical axis too, so that it reads the right way round on the screen:
    // the name tags over people's heads in View > Selfie (Caity, 2026-10-29). What is fixed in the world stays as a
    // mirror shows it. Returns whether the transform was mirrored (its X scale negated, which turns the faces inside
    // out: a draw that culls back faces must cull front faces then, as Font does).
    static bool applyBillboard(Transform& transform, BillboardMode billboardMode, RenderArgs* args);

    virtual uint32_t metaFetchMetaSubItems(ItemIDs& subItems) const override;
    virtual Item::Bound getBound() override;

    // the impostor switch of this entity's card (ImpostorCard, IMPOSTORS.md 2.3-2.4): null without an "impostor" entry
    std::shared_ptr<ImpostorSwitch> getImpostorSwitch() const;
    // what this entity could not load (its model, image or textures), one line each with the reason, for build mode's
    // Hierarchy and Inspector; empty when nothing has failed. Read on the main thread, where the renderer is updated
    virtual QStringList loadFailures() const { return QStringList(); }
    // the switch an entity's models follow: its own card's, or the nearest ancestor's (a group's card, Unity's empty
    // parent with Model children, stands in for its children's models); null when none has a card
    static std::shared_ptr<ImpostorSwitch> findImpostorSwitch(const EntityItemPointer& entity);
    // asks the nearest ancestor with a card to update (its card's visibility follows its Model descendants')
    static void updateImpostorAncestor(const EntityItemPointer& entity);

    // What a Model entity's mesh may wait behind (ENTITY-STREAMING.md 3.1): the impostor card of the entity or of its
    // nearest ancestor with an "impostor" entry, the sphere it stands in (world space) and its atlases' state. An
    // entry whose card is not made yet (its entity's renderer has not updated since) is found with its atlases not
    // loaded, on their way. Main thread.
    struct ImpostorStandIn {
        bool found { false };
        glm::vec3 center { 0.0f };
        float radius { 0.0f };
        bool atlasesLoaded { false };
        bool atlasesFailed { false };
    };
    static ImpostorStandIn findImpostorStandIn(const EntityItemPointer& entity);
    // a model that does not draw yet, for which its card stands in (ModelEntityRenderer: its mesh waits behind the
    // card, or it or its textures are on their way)
    virtual bool needsImpostorStandIn() const { return false; }

    enum ZoneCullingState {
        ZoneCull_Inactive,
        ZoneCull_Culled,
        ZoneCull_Skipped
    };

    ZoneCullingState _zoneCullState{ ZoneCullingState::ZoneCull_Inactive };
    ZoneCullingState _prevZoneCullState{ ZoneCullingState::ZoneCull_Inactive };
    virtual bool evaluateEntityZoneCullState(const EntityItemPointer& entity);//, QVector<QUuid>& skiplist);

    virtual void setStaticUpdateTime(quint64 time) { _staticUpdateTime = time; }
    virtual quint64 getStaticUpdateTime() { return _staticUpdateTime; }

public slots:
    // If an entity is static, it will not be receiving regular updates from the ETR update loop
    // specialUpdateRequest listens in case the entity sits outside the ETR loop & needs a state change 
   void handleSpecialUpdate(); // so the renderer can request the entity to update itself.


protected:
    virtual bool needsRenderUpdateFromEntity() const final { return needsRenderUpdateFromEntity(_entity); }
    virtual void onAddToScene(const EntityItemPointer& entity);
    virtual void onRemoveFromScene(const EntityItemPointer& entity);

    EntityRenderer(const EntityItemPointer& entity);
    ~EntityRenderer();

    // Implementing the PayloadProxyInterface methods
    virtual ItemKey getKey() override;
    virtual ShapeKey getShapeKey() override;
    virtual void render(RenderArgs* args) override final;
    virtual render::hifi::Tag getTagMask() const;
    virtual render::hifi::Layer getHifiRenderLayer() const;

    // Returns true if the item in question needs to have updateInScene called because of internal rendering state changes
    virtual bool needsRenderUpdate() const;

    // Returns true if the item in question needs to have updateInScene called because of changes in the entity
    virtual bool needsRenderUpdateFromEntity(const EntityItemPointer& entity) const;

    // Will be called on the main thread from updateInScene.  This can be used to fetch things like 
    // network textures or model geometry from resource caches
    virtual void doRenderUpdateSynchronous(const ScenePointer& scene, Transaction& transaction, const EntityItemPointer& entity);

    // Will be called by the lambda posted to the scene in updateInScene.  
    // This function will execute on the rendering thread, so you cannot use network caches to fetch
    // data in this method if using multi-threaded rendering
    
    virtual void doRenderUpdateAsynchronous(const EntityItemPointer& entity) { }

    // Called by the `render` method after `needsRenderUpdate`
    virtual void doRender(RenderArgs* args) = 0;

    virtual bool isFading() const { return _isFading; }
    virtual void updateModelTransformAndBound();
    virtual bool isTransparent() const { return _isFading ? Interpolate::calculateFadeRatio(_fadeStartTime) < 1.0f : false; }
    inline bool isValidRenderItem() const { return _renderItemID ? !Item::INVALID_ITEM_ID : false; }

    virtual void setIsVisibleInSecondaryCamera(bool value) { _isVisibleInSecondaryCamera = value; }
    virtual void setRenderLayer(RenderLayer value) { _renderLayer = value; }
    virtual void setPrimitiveMode(PrimitiveMode value) { _primitiveMode = value; }
    virtual void setCullWithParent(bool value) { _cullWithParent = value; }

    virtual bool wasPreviouslyVisible() const { return _previouslyVisible; }
    virtual void setPreviouslyVisible(bool value) { _previouslyVisible = value; }

signals:
    void requestRenderUpdate(); // so entity can call out to the renderer to re-render it



protected:
    template<typename T>
    std::shared_ptr<T> asTypedEntity() { return std::static_pointer_cast<T>(_entity); }

    static void makeStatusGetters(const EntityItemPointer& entity, Item::Status::Getters& statusGetters);
    const Transform& getModelTransform() const;

    // the impostor card, a second render item beside the entity's own, made when the entity's userData gains an
    // "impostor" entry and removed when it loses it (main thread, from the synchronous update)
    void updateImpostorCard(const ScenePointer& scene, Transaction& transaction, const EntityItemPointer& entity);
    void removeImpostorCard(Transaction& transaction);
    bool impostorCardVisible(const EntityItemPointer& entity) const;
    bool impostorCardStandsIn(const EntityItemPointer& entity) const;
    // a Model's own card or its nearest ancestor's updates: whether the card stands in for it has changed
    void updateImpostorOwner(const EntityItemPointer& entity);
    uint32_t fetchImpostorCardSubItem(ItemIDs& subItems) const;
    std::shared_ptr<ImpostorCard> _impostorCard;
    ItemID _impostorCardItemID { Item::INVALID_ITEM_ID };
    QString _impostorUserData;   // the userData the entry was last read from
    QString _impostorSpec;       // the entry it held

    Item::Bound _bound;
    SharedSoundPointer _collisionSound;
    QUuid _changeHandlerId;
    ItemID _renderItemID{ Item::INVALID_ITEM_ID };
    uint64_t _fadeStartTime{ usecTimestampNow() };
    // FROZEN (vehicle path): see the note at the top of this file.
    uint64_t _updateTime{ 0 }; // used when sorting/throttling render updates
    uint64_t _staticUpdateTime{ 0 }; 
    // FROZEN (vehicle path): see the note at the top of this file.
    uint64_t _deferredSinceFrame { 0 };
    uint32_t _lastLateFrames { 0 };


    bool _isFading { EntityTreeRenderer::getEntitiesShouldFadeFunction()() };
    bool _prevIsTransparent { false };
    // the transparency the render item's key was last built with (getKey): what a shape key must agree with, since
    // the key decides the pass the item is drawn in and the shape key the pipeline it is drawn with
    std::atomic<bool> _keyIsTransparent { false };
    bool _visible{ false };
    bool _previouslyVisible{ false };
    bool _isVisibleInSecondaryCamera { false };
    bool _canCastShadow { false };
    

    bool _cullWithParent { false };
    RenderLayer _renderLayer { RenderLayer::WORLD };
    PrimitiveMode _primitiveMode { PrimitiveMode::SOLID };
    bool _cauterized { false };
    bool _moving { false };
    Transform _renderTransform;

    std::unordered_map<std::string, graphics::MultiMaterial> _materials;
    std::mutex _materialsLock;

    quint64 _created;

    // The base class relies on comparing the model transform to the entity transform in order 
    // to trigger an update, so the member must not be visible to derived classes as a modifiable
    // transform
    Transform _modelTransform;
    // The rendering code only gets access to the entity in very specific circumstances
    // i.e. to see if the rendering code needs to update because of a change in state of the 
    // entity.  This forces all the rendering code itself to be independent of the entity
    const EntityItemPointer _entity;
};

template <typename T>
class TypedEntityRenderer : public EntityRenderer {
    using Parent = EntityRenderer;

public:
    TypedEntityRenderer(const EntityItemPointer& entity) : Parent(entity), _typedEntity(asTypedEntity<T>()) {}

protected:
    using TypedEntityPointer = std::shared_ptr<T>;

    virtual void onAddToScene(const EntityItemPointer& entity) override final {
        Parent::onAddToScene(entity);
        onAddToSceneTyped(_typedEntity);
    }

    virtual void onRemoveFromScene(const EntityItemPointer& entity) override final {
        Parent::onRemoveFromScene(entity);
        onRemoveFromSceneTyped(_typedEntity);
    }

    using Parent::needsRenderUpdateFromEntity;
    // Returns true if the item in question needs to have updateInScene called because of changes in the entity
    virtual bool needsRenderUpdateFromEntity(const EntityItemPointer& entity) const override final {
        return Parent::needsRenderUpdateFromEntity(entity) || needsRenderUpdateFromTypedEntity(_typedEntity);
    }

    virtual void doRenderUpdateSynchronous(const ScenePointer& scene, Transaction& transaction, const EntityItemPointer& entity) override final {
        Parent::doRenderUpdateSynchronous(scene, transaction, entity);
        doRenderUpdateSynchronousTyped(scene, transaction, _typedEntity);
    }

    virtual void doRenderUpdateAsynchronous(const EntityItemPointer& entity) override final {
        Parent::doRenderUpdateAsynchronous(entity);
        doRenderUpdateAsynchronousTyped(_typedEntity);
    }

    virtual bool needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const { return false; }
    virtual void doRenderUpdateSynchronousTyped(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) { }
    virtual void doRenderUpdateAsynchronousTyped(const TypedEntityPointer& entity) { }
    virtual void onAddToSceneTyped(const TypedEntityPointer& entity) { }
    virtual void onRemoveFromSceneTyped(const TypedEntityPointer& entity) { }

private:
    const TypedEntityPointer _typedEntity;
};

} } // namespace render::entities

#endif // hifi_RenderableEntityItem_h
