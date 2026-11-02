//
//  RenderableEntityItem.cpp
//  interface/src
//


#include "RenderableEntityItem.h"

#include <ObjectMotionState.h>

#include "RenderableShapeEntityItem.h"
#include "RenderableModelEntityItem.h"
#include "RenderableTextEntityItem.h"
#include "RenderableImageEntityItem.h"
#include "RenderableWebEntityItem.h"
#include "RenderableParticleEffectEntityItem.h"
#include "RenderableLineEntityItem.h"
#include "RenderablePolyLineEntityItem.h"
#include "RenderableGridEntityItem.h"
#include "RenderableGizmoEntityItem.h"
#include "RenderableLightEntityItem.h"
#include "RenderableZoneEntityItem.h"
#include "RenderableMaterialEntityItem.h"
#include "RenderableTerrainEntityItem.h"
#include "RenderableTrackEntityItem.h"
#include "ImpostorCard.h"

using namespace render;
using namespace render::entities;

void EntityRenderer::initEntityRenderers() {
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Model, RenderableModelEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Terrain, RenderableTerrainEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Track, RenderableTrackEntityItem::factory)
}

const Transform& EntityRenderer::getModelTransform() const {
    return _modelTransform;
}

void EntityRenderer::makeStatusGetters(const EntityItemPointer& entity, Item::Status::Getters& statusGetters) {
    auto nodeList = DependencyManager::get<NodeList>();
    // DANGER: nodeList->getSessionUUID() will return null id when not connected to domain.
    const QUuid& myNodeID = nodeList->getSessionUUID();

    statusGetters.push_back([entity]() -> render::Item::Status::Value {
        uint64_t delta = usecTimestampNow() - entity->getLastEditedFromRemote();
        const float WAIT_THRESHOLD_INV = 1.0f / (0.2f * USECS_PER_SECOND);
        float normalizedDelta = delta * WAIT_THRESHOLD_INV;
        // Status icon will scale from 1.0f down to 0.0f after WAIT_THRESHOLD
        // Color is red if last update is after WAIT_THRESHOLD, green otherwise (120 deg is green)
        return render::Item::Status::Value(1.0f - normalizedDelta, (normalizedDelta > 1.0f ?
            render::Item::Status::Value::GREEN :
            render::Item::Status::Value::RED),
            (unsigned char)render::Item::Status::Icon::PACKET_RECEIVED);
    });

    statusGetters.push_back([entity] () -> render::Item::Status::Value {
        uint64_t delta = usecTimestampNow() - entity->getLastBroadcast();
        const float WAIT_THRESHOLD_INV = 1.0f / (0.4f * USECS_PER_SECOND);
        float normalizedDelta = delta * WAIT_THRESHOLD_INV;
        // Status icon will scale from 1.0f down to 0.0f after WAIT_THRESHOLD
        // Color is Magenta if last update is after WAIT_THRESHOLD, cyan otherwise (180 deg is green)
        return render::Item::Status::Value(1.0f - normalizedDelta, (normalizedDelta > 1.0f ?
            render::Item::Status::Value::MAGENTA :
            render::Item::Status::Value::CYAN),
            (unsigned char)render::Item::Status::Icon::PACKET_SENT);
    });

    statusGetters.push_back([entity] () -> render::Item::Status::Value {
        ObjectMotionState* motionState = static_cast<ObjectMotionState*>(entity->getPhysicsInfo());
        if (motionState && motionState->isActive()) {
            return render::Item::Status::Value(1.0f, render::Item::Status::Value::BLUE,
                (unsigned char)render::Item::Status::Icon::ACTIVE_IN_BULLET);
        }
        return render::Item::Status::Value(0.0f, render::Item::Status::Value::BLUE,
            (unsigned char)render::Item::Status::Icon::ACTIVE_IN_BULLET);
    });

    statusGetters.push_back([entity, myNodeID] () -> render::Item::Status::Value {
        bool weOwnSimulation = entity->getSimulationOwner().matchesValidID(myNodeID);
        bool otherOwnSimulation = !weOwnSimulation && !entity->getSimulationOwner().isNull();

        if (weOwnSimulation) {
            return render::Item::Status::Value(1.0f, render::Item::Status::Value::BLUE,
                (unsigned char)render::Item::Status::Icon::SIMULATION_OWNER);
        } else if (otherOwnSimulation) {
            return render::Item::Status::Value(1.0f, render::Item::Status::Value::RED,
                (unsigned char)render::Item::Status::Icon::OTHER_SIMULATION_OWNER);
        }
        return render::Item::Status::Value(0.0f, render::Item::Status::Value::BLUE,
            (unsigned char)render::Item::Status::Icon::SIMULATION_OWNER);
    });

    statusGetters.push_back([entity] () -> render::Item::Status::Value {
        if (entity->hasActions()) {
            return render::Item::Status::Value(1.0f, render::Item::Status::Value::GREEN,
                (unsigned char)render::Item::Status::Icon::HAS_ACTIONS);
        }
        return render::Item::Status::Value(0.0f, render::Item::Status::Value::GREEN,
            (unsigned char)render::Item::Status::Icon::HAS_ACTIONS);
    });

    statusGetters.push_back([entity] () -> render::Item::Status::Value {
        if (entity->isAvatarEntity()) {
            if (entity->isMyAvatarEntity()) {
                return render::Item::Status::Value(1.0f, render::Item::Status::Value::GREEN,
                    (unsigned char)render::Item::Status::Icon::ENTITY_HOST_TYPE);
            } else {
                return render::Item::Status::Value(1.0f, render::Item::Status::Value::RED,
                    (unsigned char)render::Item::Status::Icon::ENTITY_HOST_TYPE);
            }
        } else if (entity->isLocalEntity()) {
            return render::Item::Status::Value(1.0f, render::Item::Status::Value::BLUE,
                (unsigned char)render::Item::Status::Icon::ENTITY_HOST_TYPE);
        }
        return render::Item::Status::Value(0.0f, render::Item::Status::Value::GREEN,
            (unsigned char)render::Item::Status::Icon::ENTITY_HOST_TYPE);
    });
}


template <typename T> 
std::shared_ptr<T> make_renderer(const EntityItemPointer& entity) {
    // We want to use deleteLater so that renderer destruction gets pushed to the main thread
    return std::shared_ptr<T>(new T(entity), [](T* ptr) { ptr->deleteLater(); });
}

EntityRenderer::EntityRenderer(const EntityItemPointer& entity) : _created(entity->getCreated()), _entity(entity) {}

EntityRenderer::~EntityRenderer() {}

//
// Smart payload proxy members, implementing the payload interface
//

Item::Bound EntityRenderer::getBound() {
    return _bound;
}

ShapeKey EntityRenderer::getShapeKey() {
    if (_primitiveMode == PrimitiveMode::LINES) {
        return ShapeKey::Builder().withOwnPipeline().withWireframe();
    }
    return ShapeKey::Builder().withOwnPipeline();
}

render::hifi::Tag EntityRenderer::getTagMask() const {
    render::hifi::Tag mask = render::hifi::TAG_NONE;
    mask = (render::hifi::Tag)(mask | (!_cauterized * render::hifi::TAG_MAIN_VIEW));
    mask = (render::hifi::Tag)(mask | (_isVisibleInSecondaryCamera * render::hifi::TAG_SECONDARY_VIEW));
    return mask;
}

render::hifi::Layer EntityRenderer::getHifiRenderLayer() const {
    switch (_renderLayer) {
        case RenderLayer::WORLD:
            return render::hifi::LAYER_3D;
        case RenderLayer::FRONT:
            return render::hifi::LAYER_3D_FRONT;
        case RenderLayer::HUD:
            return render::hifi::LAYER_3D_HUD;
        default:
            return render::hifi::LAYER_3D;
    }
}

ItemKey EntityRenderer::getKey() {
    ItemKey::Builder builder =
        ItemKey::Builder().withTypeShape().withTypeMeta().withTagBits(getTagMask()).withLayer(getHifiRenderLayer());

    bool transparent = isTransparent();
    _keyIsTransparent = transparent;
    if (transparent) {
        builder.withTransparent();
    }
    // translucent entities cast too (the shadow pass decides which can: those with a material), as a model's parts do
    if (_canCastShadow) {
        builder.withShadowCaster();
    }

    if (_cullWithParent) {
        builder.withSubMetaCulled();
    }

    if (!_visible) {  
        builder.withInvisible();
    }

    return builder;
}

uint32_t EntityRenderer::metaFetchMetaSubItems(ItemIDs& subItems) const {
    uint32_t count = fetchImpostorCardSubItem(subItems);
    if (Item::isValidID(_renderItemID)) {
        subItems.emplace_back(_renderItemID);
        count++;
    }
    return count;
}

uint32_t EntityRenderer::fetchImpostorCardSubItem(ItemIDs& subItems) const {
    if (Item::isValidID(_impostorCardItemID)) {
        subItems.emplace_back(_impostorCardItemID);
        return 1;
    }
    return 0;
}

void EntityRenderer::render(RenderArgs* args) {
    if (!isValidRenderItem()) {
        return;
    }

    if (_visible && (args->_renderMode != RenderArgs::RenderMode::DEFAULT_RENDER_MODE || !_cauterized)) {
        doRender(args);
    }
}

//
// Methods called by the EntityTreeRenderer
EntityRenderer::Pointer EntityRenderer::addToScene(EntityTreeRenderer& renderer,
                                                   const EntityItemPointer& entity,
                                                   const ScenePointer& scene,
                                                   Transaction& transaction) {
    EntityRenderer::Pointer result;
    if (!entity) {
        return result;
    }

    using Type = EntityTypes::EntityType_t;
    auto type = entity->getType();
    switch (type) {
        case Type::Shape:
        case Type::Box:
        case Type::Sphere:
            result = make_renderer<ShapeEntityRenderer>(entity);
            break;

        case Type::Model:
            result = make_renderer<ModelEntityRenderer>(entity);
            break;

        case Type::Text:
            result = make_renderer<TextEntityRenderer>(entity);
            break;

        case Type::Image:
            result = make_renderer<ImageEntityRenderer>(entity);
            break;

        case Type::Web:
            if (!nsightActive()) {
                result = make_renderer<WebEntityRenderer>(entity);
            }
            break;

        case Type::ParticleEffect:
            result = make_renderer<ParticleEffectEntityRenderer>(entity);
            break;

        case Type::Line:
            result = make_renderer<LineEntityRenderer>(entity);
            break;

        case Type::PolyLine:
            result = make_renderer<PolyLineEntityRenderer>(entity);
            break;

        case Type::Grid:
            result = make_renderer<GridEntityRenderer>(entity);
            break;

        case Type::Gizmo:
            result = make_renderer<GizmoEntityRenderer>(entity);
            break;

        case Type::Light:
            result = make_renderer<LightEntityRenderer>(entity);
            break;

        case Type::Zone:
            result = make_renderer<ZoneEntityRenderer>(entity);
            break;

        case Type::Material:
            result = make_renderer<MaterialEntityRenderer>(entity);
            break;

        case Type::Terrain:
            result = make_renderer<TerrainEntityRenderer>(entity);
            break;

        case Type::Track:
            result = make_renderer<TrackEntityRenderer>(entity);
            break;

        default:
            break;
    }

    if (result) {
        result->addToScene(scene, transaction);  
    }

    return result;
}


bool EntityRenderer::addToScene(const ScenePointer& scene, Transaction& transaction) {
    _renderItemID = scene->allocateID();
    // Complicated series of trusses
    auto renderPayload = std::make_shared<PayloadProxyInterface::ProxyPayload>(shared_from_this());
    Item::Status::Getters statusGetters;
    makeStatusGetters(_entity, statusGetters);
    renderPayload->addStatusGetters(statusGetters);
    transaction.resetItem(_renderItemID, renderPayload);
    onAddToScene(_entity);
    updateInScene(scene, transaction);
    _entity->bumpAncestorChainRenderableVersion();
    return true;
}

void EntityRenderer::removeFromScene(const ScenePointer& scene, Transaction& transaction) {
    onRemoveFromScene(_entity);
    removeImpostorCard(transaction);
    transaction.removeItem(_renderItemID);
    Item::clearID(_renderItemID);
    _entity->bumpAncestorChainRenderableVersion();
}

void EntityRenderer::updateInScene(const ScenePointer& scene, Transaction& transaction, bool changed) {
    DETAILED_PROFILE_RANGE(simulation_physics, __FUNCTION__);
    if (this == nullptr || !isValidRenderItem()) {
        return;
    }
    _updateTime = usecTimestampNow();

    doRenderUpdateSynchronous(scene, transaction, _entity);
    transaction.updateItem<PayloadProxyInterface>(_renderItemID, [this](PayloadProxyInterface& self) {
        if (!isValidRenderItem()) {
            return;
        }
        // Happens on the render thread.  Classes should use
        doRenderUpdateAsynchronous(_entity);
    }, changed);
}

//
// Internal methods
//

// Returns true if the item needs to have updateInscene called because of internal rendering
// changes (animation, fading, etc)
bool EntityRenderer::needsRenderUpdate() const {
    if (isFading()) {
        return true;
    }

    if (_prevIsTransparent != isTransparent()) {
        return true;
    }
    return needsRenderUpdateFromEntity(_entity);
}

// Returns true if the item in question needs to have updateInScene called because of changes in the entity
bool EntityRenderer::needsRenderUpdateFromEntity(const EntityItemPointer& entity) const {
    if (entity->needsRenderUpdate()) {
        return true;
    }

    if (!entity->isVisuallyReady()) {
        return true;
    }

    bool success = false;
    auto bound = _entity->getAABox(success);
    if (success && _bound != bound) {
        return true;
    }

    auto newModelTransform = _entity->getTransformToCenter(success);
    // FIXME can we use a stale model transform here?
    if (success && newModelTransform != _modelTransform) {
        return true;
    }

    // userData changes raise no flag of their own: the impostor entry may have come or gone with it
    if (entity->getUserData() != _impostorUserData) {
        return true;
    }

    if (entity->getEntityPriority() != EntityPriority::STATIC)
    {
        if (_moving != entity->isMovingRelativeToParent()) {
            return true;
        }
    }

    return false;
}

void EntityRenderer::updateModelTransformAndBound() {
    bool success = false;
    auto newModelTransform = _entity->getTransformToCenter(success);
    if (success) {
        _modelTransform = newModelTransform;
    }

    success = false;
    auto bound = _entity->getAABox(success);
    if (success) {
        _bound = bound;
    }
}

// Tivoli zone culling and locally hidden happens here.
void EntityRenderer::doRenderUpdateSynchronous(const ScenePointer& scene,
                                               Transaction& transaction,
                                               const EntityItemPointer& entity) {
    DETAILED_PROFILE_RANGE(simulation_physics, __FUNCTION__);
    const bool wasVisible = _visible;
    withWriteLock([&] {
        auto transparent = isTransparent();
        auto fading = isFading();
        if (fading || _prevIsTransparent != transparent || !entity->isVisuallyReady()) {
            emit requestRenderUpdate();
        }
        if (fading) {
            _isFading = Interpolate::calculateFadeRatio(_fadeStartTime) < 1.0f;
        }

        _prevIsTransparent = transparent;

        updateModelTransformAndBound();

        _moving = entity->isMovingRelativeToParent();

        //if (entity->getEntityPriority() == EntityPriority::STATIC) _moving = false; // statics can't move

        _visible = entity->getVisible();

        if (_visible) {   
            EntityTypes::EntityType checkType = entity->getType();
            if (checkType == EntityTypes::Zone) {
                // a zone lights every view that fetches its item, and the mirrors' views and the secondary camera's
                // fetch only items tagged for the secondary view: without its flag a reflection had no zone light,
                // sky or haze (MIRRORS.md section 5)
                setIsVisibleInSecondaryCamera(entity->isVisibleInSecondaryCamera());
                return;
            }
            if (checkType == EntityTypes::Gizmo) return;
            if (checkType == EntityTypes::Grid) return;
            if (checkType == EntityTypes::Material) return;
        }

        // zone culling: the flag was decided when the zones around the avatar changed (EntityTreeRenderer); while a
        // zone is active an entity that updates (it moved, or it is new) tests its bound against the zone's box
        {
            auto treeRenderer = DependencyManager::get<EntityTreeRenderer>();
            if (treeRenderer && treeRenderer->getZoneCullState().active && EntityTreeRenderer::isZoneCullable(entity)) {
                _zoneCulled = !treeRenderer->getZoneCullState().touches(_bound.calcCenter(),
                                                                       0.5f * _bound.getLargestDimension() * SQRT_THREE);
            }
        }
        if (!entity->getLocallyVisible() || _zoneCulled) {
            _visible = false;
        }
        setIsVisibleInSecondaryCamera(entity->isVisibleInSecondaryCamera());
        setRenderLayer(entity->getRenderLayer());
        setPrimitiveMode(entity->getPrimitiveMode());
        _canCastShadow = entity->getCanCastShadow();
        setCullWithParent(entity->getCullWithParent());
        _cauterized = entity->getCauterized();
        // the entity's own change flag is cleared by this update; a locally invisible or culled entity no longer asks
        // for another update every frame, as the old zone-culling bookkeeping had it do
        entity->setNeedsRenderUpdate(false);
    });

    // A group's card shows while any of its Model children is visible (impostorCardVisible), which the group's own
    // update decides: a model that was shown or hidden (its visible flag, locally, by zone culling) asks the nearest
    // ancestor with a card to update, or the card would stay up with its models hidden
    if (wasVisible != _visible && entity->getType() == EntityTypes::Model) {
        updateImpostorAncestor(entity);
    }
    updateImpostorCard(scene, transaction, entity);
}

void EntityRenderer::updateImpostorAncestor(const EntityItemPointer& entity) {
    auto entityTreeRenderer = DependencyManager::get<EntityTreeRenderer>();
    if (!entityTreeRenderer || !entity) {
        return;
    }
    const int MAX_DEPTH = 16;
    bool success = false;
    SpatiallyNestablePointer nestable = entity->getParentPointer(success);
    for (int depth = 0; success && nestable && depth < MAX_DEPTH; depth++) {
        auto item = std::dynamic_pointer_cast<EntityItem>(nestable);
        auto renderer = item ? entityTreeRenderer->renderableForEntityId(item->getID()) : nullptr;
        if (renderer && renderer->getImpostorSwitch()) {
            entityTreeRenderer->onEntityChanged(item->getID());
            return;
        }
        nestable = nestable->getParentPointer(success);
    }
}

// The impostor card (IMPOSTORS.md 2.3): a second render item that stands in for the entity's models at a distance,
// made from the "impostor" entry of the entity's userData (the baker's), kept at the entity's world position and
// rotation (the entry's centre and radius are in the entity's frame at bake time), and removed with the entry.
// The models it stands for, the entity's own and its descendants', follow its switch (findImpostorSwitch), so
// when the entry comes or goes they are asked to update and fetch it again.
void EntityRenderer::updateImpostorCard(const ScenePointer& scene, Transaction& transaction, const EntityItemPointer& entity) {
    QString userData = entity->getUserData();
    if (userData != _impostorUserData) {
        _impostorUserData = userData;
        ImpostorEntry entry = ImpostorEntry::parse(userData);
        QString spec = entry.valid ? entry.spec : QString();
        if (spec != _impostorSpec) {
            _impostorSpec = spec;
            bool hadCard = _impostorCard != nullptr;
            if (entry.valid) {
                if (!_impostorCard) {
                    _impostorCard = std::make_shared<ImpostorCard>();
                    _impostorCardItemID = scene->allocateID();
                    transaction.resetItem(_impostorCardItemID, std::make_shared<PayloadProxyInterface::ProxyPayload>(_impostorCard));
                }
                _impostorCard->setEntry(entry);
            } else {
                removeImpostorCard(transaction);
            }
            if (hadCard != (_impostorCard != nullptr)) {
                // the models under this entity take or drop the switch at their next update
                auto renderer = DependencyManager::get<EntityTreeRenderer>();
                if (renderer) {
                    if (entity->getType() == EntityTypes::Model) {
                        renderer->onEntityChanged(entity->getID());
                    }
                    entity->forEachDescendant([&](const SpatiallyNestablePointer& descendant) {
                        auto item = std::dynamic_pointer_cast<EntityItem>(descendant);
                        if (item && item->getType() == EntityTypes::Model) {
                            renderer->onEntityChanged(item->getID());
                        }
                    });
                }
            }
        }
    }
    if (!_impostorCard) {
        return;
    }
    bool success = false;
    glm::quat rotation = entity->getWorldOrientation(success);
    _impostorCard->setFrame(entity->getWorldPosition(), success ? rotation : glm::quat());
    _impostorCard->setVisible(impostorCardVisible(entity));
    _impostorCard->setStandIn(impostorCardStandsIn(entity));
    _impostorCard->setKeyBits(getTagMask(), getHifiRenderLayer(), _cullWithParent, _canCastShadow);
    if (_impostorCard->waitingForTextures()) {
        emit requestRenderUpdate();
    }
    auto card = _impostorCard;
    transaction.updateItem<PayloadProxyInterface>(_impostorCardItemID, [card](PayloadProxyInterface& self) {
        card->update();
    });
}

void EntityRenderer::removeImpostorCard(Transaction& transaction) {
    if (_impostorCard) {
        transaction.removeItem(_impostorCardItemID);
        Item::clearID(_impostorCardItemID);
        _impostorCard.reset();
    }
}

// The card shows where the entity does; and where the entity is one hidden by its own visible flag that groups
// Model children (a Unity export's empty parent), it shows for them, while any of them is visible, and while none of
// them has arrived (the card stands in for them, ENTITY-STREAMING.md 3.2).
bool EntityRenderer::impostorCardVisible(const EntityItemPointer& entity) const {
    if (_visible) {
        return true;
    }
    if (!entity->getLocallyVisible() || _zoneCulled || entity->getVisible() ||
        entity->getType() == EntityTypes::Model) {
        return false;
    }
    bool anyModel = false;
    bool anyVisibleModel = false;
    entity->forEachDescendantTest([&](const SpatiallyNestablePointer& descendant) {
        auto item = std::dynamic_pointer_cast<EntityItem>(descendant);
        if (item && item->getType() == EntityTypes::Model) {
            anyModel = true;
            if (item->getVisible() && item->getLocallyVisible()) {
                anyVisibleModel = true;
                return false;
            }
        }
        return true;
    });
    return anyVisibleModel || !anyModel;
}

// The card stands in for the models it is the card of (ENTITY-STREAMING.md 3.2) while one of those shown does not
// draw yet (needsImpostorStandIn: its mesh waits behind the card, or it or its textures are on their way; a model
// whose renderer is not made yet counts with them) or, for a group, while none of them has arrived.
bool EntityRenderer::impostorCardStandsIn(const EntityItemPointer& entity) const {
    auto entityTreeRenderer = DependencyManager::get<EntityTreeRenderer>();
    if (!entityTreeRenderer) {
        return false;
    }
    bool anyModel = false;
    bool waiting = false;
    auto check = [&](const EntityItemPointer& item) {
        anyModel = true;
        if (item->getVisible() && item->getLocallyVisible()) {
            auto renderer = entityTreeRenderer->renderableForEntityId(item->getID());
            if (!renderer || renderer->needsImpostorStandIn()) {
                waiting = true;
            }
        }
    };
    if (entity->getType() == EntityTypes::Model) {
        anyModel = true;
        waiting = needsImpostorStandIn();
    }
    if (!waiting) {
        entity->forEachDescendantTest([&](const SpatiallyNestablePointer& descendant) {
            auto item = std::dynamic_pointer_cast<EntityItem>(descendant);
            if (item && item->getType() == EntityTypes::Model) {
                check(item);
            }
            return !waiting;
        });
    }
    return waiting || !anyModel;
}

void EntityRenderer::updateImpostorOwner(const EntityItemPointer& entity) {
    if (_impostorCard) {
        emit requestRenderUpdate();
    } else {
        updateImpostorAncestor(entity);
    }
}

std::shared_ptr<ImpostorSwitch> EntityRenderer::getImpostorSwitch() const {
    return _impostorCard ? _impostorCard->getSwitch() : nullptr;
}

std::shared_ptr<ImpostorSwitch> EntityRenderer::findImpostorSwitch(const EntityItemPointer& entity) {
    auto entityTreeRenderer = DependencyManager::get<EntityTreeRenderer>();
    if (!entityTreeRenderer || !entity) {
        return nullptr;
    }
    const int MAX_DEPTH = 16;
    SpatiallyNestablePointer nestable = entity;
    for (int depth = 0; nestable && depth < MAX_DEPTH; depth++) {
        auto item = std::dynamic_pointer_cast<EntityItem>(nestable);
        if (item) {
            auto renderer = entityTreeRenderer->renderableForEntityId(item->getID());
            if (renderer) {
                auto impostorSwitch = renderer->getImpostorSwitch();
                if (impostorSwitch) {
                    return impostorSwitch;
                }
            }
        }
        bool success = false;
        nestable = nestable->getParentPointer(success);
        if (!success) {
            break;
        }
    }
    return nullptr;
}

EntityRenderer::ImpostorStandIn EntityRenderer::findImpostorStandIn(const EntityItemPointer& entity) {
    ImpostorStandIn standIn;
    auto entityTreeRenderer = DependencyManager::get<EntityTreeRenderer>();
    if (!entityTreeRenderer || !entity) {
        return standIn;
    }
    const int MAX_DEPTH = 16;
    SpatiallyNestablePointer nestable = entity;
    for (int depth = 0; nestable && depth < MAX_DEPTH; depth++) {
        auto item = std::dynamic_pointer_cast<EntityItem>(nestable);
        if (item) {
            auto renderer = entityTreeRenderer->renderableForEntityId(item->getID());
            if (renderer && renderer->_impostorCard) {
                if (renderer->_impostorCard->getSphere(standIn.center, standIn.radius)) {
                    standIn.found = true;
                    standIn.atlasesLoaded = renderer->_impostorCard->atlasesLoaded();
                    standIn.atlasesFailed = renderer->_impostorCard->atlasesFailed();
                    return standIn;
                }
            } else if (item->getUserData().contains(QStringLiteral("impostor"))) {
                // the entry is here, its card not yet: the sphere as the card will take it (updateImpostorCard)
                ImpostorEntry entry = ImpostorEntry::parse(item->getUserData());
                if (entry.valid) {
                    bool success = false;
                    glm::quat rotation = item->getWorldOrientation(success);
                    standIn.center = item->getWorldPosition() + (success ? rotation : glm::quat()) * entry.center;
                    standIn.radius = entry.radius;
                    standIn.found = true;
                    return standIn;
                }
            }
        }
        bool success = false;
        nestable = nestable->getParentPointer(success);
        if (!success) {
            break;
        }
    }
    return standIn;
}

void EntityRenderer::onAddToScene(const EntityItemPointer& entity) {
    QObject::connect(this, &EntityRenderer::requestRenderUpdate, this,
        [this] {
            auto renderer = DependencyManager::get<EntityTreeRenderer>();
            if (renderer) {
                renderer->onEntityChanged(_entity->getID());
            }
        },
    Qt::QueuedConnection);
    _changeHandlerId = entity->registerChangeHandler([](const EntityItemID& changedEntity) {
        auto renderer = DependencyManager::get<EntityTreeRenderer>();
        if (renderer) {
            auto renderable = renderer->renderableForEntityId(changedEntity);
            if (renderable && renderable->needsRenderUpdate()) {
                renderer->onEntityChanged(changedEntity);
            }
        }
    });
}

void EntityRenderer::onRemoveFromScene(const EntityItemPointer& entity) {
    entity->deregisterChangeHandler(_changeHandlerId);
    QObject::disconnect(this, &EntityRenderer::requestRenderUpdate, this, nullptr);
}

void EntityRenderer::addMaterial(graphics::MaterialLayer material, const std::string& parentMaterialName) {
    {
        std::lock_guard<std::mutex> lock(_materialsLock);
        _materials[parentMaterialName].push(material);
    }
    // the material may change the pass this is drawn in (an opacity): the item's key is rebuilt next frame, not
    // whenever something else happens to update this entity
    emit requestRenderUpdate();
}

void EntityRenderer::removeMaterial(graphics::MaterialPointer material, const std::string& parentMaterialName) {
    {
        std::lock_guard<std::mutex> lock(_materialsLock);
        _materials[parentMaterialName].remove(material);
    }
    emit requestRenderUpdate();
}

bool EntityRenderer::applyBillboard(Transform& transform, BillboardMode billboardMode, RenderArgs* args) {
    transform.setRotation(EntityItem::getBillboardRotation(transform.getTranslation(), transform.getRotation(), billboardMode,
                                                           args->getViewFrustum().getPosition()));
    if (billboardMode == BillboardMode::NONE || !args->_mirrored) {
        return false;
    }
    transform.postScale(glm::vec3(-1.0f, 1.0f, 1.0f));   // about the entity's own vertical axis, through its centre
    return true;
}

glm::vec4 EntityRenderer::calculatePulseColor(const glm::vec4& color,
                                              const PulsePropertyGroup& pulseProperties,
                                              quint64 start) {
    if (pulseProperties.getPeriod() == 0.0f ||
        (pulseProperties.getColorMode() == PulseMode::NONE && pulseProperties.getAlphaMode() == PulseMode::NONE)) {
        return color;
    }

    float t = ((float)(usecTimestampNow() - start)) / ((float)USECS_PER_SECOND);
    float pulse = 0.5f * (cosf(t * (2.0f * (float)M_PI) / pulseProperties.getPeriod()) + 1.0f) *
                      (pulseProperties.getMax() - pulseProperties.getMin()) +
                  pulseProperties.getMin();
    float outPulse = (1.0f - pulse);

    glm::vec4 result = color;
    if (pulseProperties.getColorMode() == PulseMode::IN_PHASE) {
        result.r *= pulse;
        result.g *= pulse;
        result.b *= pulse;
    } else if (pulseProperties.getColorMode() == PulseMode::OUT_PHASE) {
        result.r *= outPulse;
        result.g *= outPulse;
        result.b *= outPulse;
    }

    if (pulseProperties.getAlphaMode() == PulseMode::IN_PHASE) {
        result.a *= pulse;
    } else if (pulseProperties.getAlphaMode() == PulseMode::OUT_PHASE) {
        result.a *= outPulse;
    }

    return result;
}

glm::vec3 EntityRenderer::calculatePulseColor(const glm::vec3& color,
                                              const PulsePropertyGroup& pulseProperties,
                                              quint64 start) {
    if (pulseProperties.getPeriod() == 0.0f ||
        (pulseProperties.getColorMode() == PulseMode::NONE && pulseProperties.getAlphaMode() == PulseMode::NONE)) {
        return color;
    }

    float t = ((float)(usecTimestampNow() - start)) / ((float)USECS_PER_SECOND);
    float pulse = 0.5f * (cosf(t * (2.0f * (float)M_PI) / pulseProperties.getPeriod()) + 1.0f) *
                      (pulseProperties.getMax() - pulseProperties.getMin()) +
                  pulseProperties.getMin();
    float outPulse = (1.0f - pulse);

    glm::vec3 result = color;
    if (pulseProperties.getColorMode() == PulseMode::IN_PHASE) {
        result.r *= pulse;
        result.g *= pulse;
        result.b *= pulse;
    } else if (pulseProperties.getColorMode() == PulseMode::OUT_PHASE) {
        result.r *= outPulse;
        result.g *= outPulse;
        result.b *= outPulse;
    }

    return result;
}
