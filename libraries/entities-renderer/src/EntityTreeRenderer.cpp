//
//  EntityTreeRenderer.cpp
//  interface/src
//
//  Created by Brad Hefta-Gaub on 12/6/13.
//
//  Modified by Caitlyn Meeks on 12/25/19
//  Copyright 2019 Tivoli Cloud VR
//
//  Renders Entity Tree contents.
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: updateChangedEntities whole (the carried pass that refreshes a moving assembly's parts with it, the
// priority pass, the 2 ms render-update budget and its sorted pass, the moving-assembly view test, the budget's
// counters and leftovers) and the staleness stamps it keeps (markRenderableRefreshed, getRenderUpdateFramesBehind,
// getRenderUpdateLastLateFrames).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.

#include "EntityTreeRenderer.h"

#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <queue>

#include <QEventLoop>
#include <QProcessEnvironment>
#include <QScriptSyntaxCheckResult>
#include <QThreadPool>
#include <QSet>
#include <QStringList>

#include <shared/QtHelpers.h>
#include <AbstractScriptingServicesInterface.h>
#include <AbstractViewStateInterface.h>
#include <AddressManager.h>
#include <ScriptNetworkPolicy.h>
#include <ColorUtils.h>
#include <FrameTrace.h>
#include <Model.h>
#include <NetworkAccessManager.h>
#include <PerfStat.h>
#include <PrioritySortUtil.h>
#include <Rig.h>
#include <SceneScriptingInterface.h>
#include <ScriptEngines.h>
#include <EntitySimulation.h>
#include <ZoneRenderer.h>
#include <PhysicalEntitySimulation.h>

#include "EntitiesRendererLogging.h"
#include "RenderableEntityItem.h"
#include "EntityTree.h"

#include "RenderableWebEntityItem.h"

#include <PointerManager.h>

std::function<bool()> EntityTreeRenderer::_entitiesShouldFadeFunction = []() { return true; };

QString resolveScriptURL(const QString& scriptUrl) {
    auto normalizedScriptUrl = DependencyManager::get<ResourceManager>()->normalizeURL(scriptUrl);
    QUrl url { normalizedScriptUrl };
    if (url.isLocalFile()) {
        // Outside of the ScriptEngine, /~/ resolves to the /resources directory.
        // Inside of the ScriptEngine, /~/ resolves to the /scripts directory.
        // Here we expand local paths in case they are /~/ paths, so they aren't
        // incorrectly recognized as being located in /scripts when utilized in ScriptEngine.
        return PathUtils::expandToLocalDataAbsolutePath(url).toString();
    }
    return normalizedScriptUrl;
}

EntityTreeRenderer::EntityTreeRenderer(bool wantScripts,
                                       AbstractViewStateInterface* viewState,
                                       AbstractScriptingServicesInterface* scriptingServices) :
    _wantScripts(wantScripts),
    _lastPointerEventValid(false),
    _viewState(viewState),
    _scriptingServices(scriptingServices),
    _displayModelBounds(false)
{
    setMouseRayPickResultOperator([](unsigned int rayPickID) {
        RayToEntityIntersectionResult entityResult;
        return entityResult;
    });
    setSetPrecisionPickingOperator([](unsigned int rayPickID, bool value) {});
    EntityRenderer::initEntityRenderers();
    _currentHoverOverEntityID = UNKNOWN_ENTITY_ID;
    _currentClickingOnEntityID = UNKNOWN_ENTITY_ID;

    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>().data();
    auto pointerManager = DependencyManager::get<PointerManager>();
    connect(pointerManager.data(), &PointerManager::hoverBeginEntity, entityScriptingInterface, &EntityScriptingInterface::hoverEnterEntity);
    connect(pointerManager.data(), &PointerManager::hoverContinueEntity, entityScriptingInterface, &EntityScriptingInterface::hoverOverEntity);
    connect(pointerManager.data(), &PointerManager::hoverEndEntity, entityScriptingInterface, &EntityScriptingInterface::hoverLeaveEntity);
    connect(pointerManager.data(), &PointerManager::triggerBeginEntity, entityScriptingInterface, &EntityScriptingInterface::mousePressOnEntity);
    connect(pointerManager.data(), &PointerManager::triggerContinueEntity, entityScriptingInterface, &EntityScriptingInterface::mouseMoveOnEntity);
    connect(pointerManager.data(), &PointerManager::triggerEndEntity, entityScriptingInterface, &EntityScriptingInterface::mouseReleaseOnEntity);

    // Forward mouse events to web entities
    auto handlePointerEvent = [&](const QUuid& entityID, const PointerEvent& event) {
        std::shared_ptr<render::entities::WebEntityRenderer> thisEntity;
        auto entity = getEntity(entityID);
        if (entity && entity->isVisible() && entity->getType() == EntityTypes::Web) {
            thisEntity = std::static_pointer_cast<render::entities::WebEntityRenderer>(renderableForEntityId(entityID));
        }
        if (thisEntity) {
            QMetaObject::invokeMethod(thisEntity.get(), "handlePointerEvent", Q_ARG(const PointerEvent&, event));
        }
    };
    connect(entityScriptingInterface, &EntityScriptingInterface::mousePressOnEntity, this, handlePointerEvent);
    connect(entityScriptingInterface, &EntityScriptingInterface::mouseMoveOnEntity, this, handlePointerEvent);
    connect(entityScriptingInterface, &EntityScriptingInterface::mouseReleaseOnEntity, this, handlePointerEvent);
    // Handle mouse-clicking or laser-clicking on entities with the `href` property set
    connect(entityScriptingInterface, &EntityScriptingInterface::mousePressOnEntity, this, [&](const QUuid& entityID, const PointerEvent& event) {
        if (!EntityTree::areEntityClicksCaptured() && (event.getButtons() & PointerEvent::PrimaryButton)) {
            auto entity = getEntity(entityID);
            if (!entity) {
                return;
            }
            auto properties = entity->getProperties();
            QString urlString = properties.getHref();
            QUrl url = QUrl(urlString, QUrl::StrictMode);
            if (url.isValid() && !url.isEmpty()) {
                DependencyManager::get<AddressManager>()->handleLookupString(urlString);
            }
        }
    });
    connect(entityScriptingInterface, &EntityScriptingInterface::hoverEnterEntity, this, [&](const QUuid& entityID, const PointerEvent& event) {
        std::shared_ptr<render::entities::WebEntityRenderer> thisEntity;
        auto entity = getEntity(entityID);
        if (entity && entity->isVisible() && entity->getType() == EntityTypes::Web) {
            thisEntity = std::static_pointer_cast<render::entities::WebEntityRenderer>(renderableForEntityId(entityID));
        }
        if (thisEntity) {
            QMetaObject::invokeMethod(thisEntity.get(), "hoverEnterEntity", Q_ARG(const PointerEvent&, event));
        }
    });
    connect(entityScriptingInterface, &EntityScriptingInterface::hoverOverEntity, this, handlePointerEvent);
    connect(entityScriptingInterface, &EntityScriptingInterface::hoverLeaveEntity, this, [&](const QUuid& entityID, const PointerEvent& event) {
        std::shared_ptr<render::entities::WebEntityRenderer> thisEntity;
        auto entity = getEntity(entityID);
        if (entity && entity->isVisible() && entity->getType() == EntityTypes::Web) {
            thisEntity = std::static_pointer_cast<render::entities::WebEntityRenderer>(renderableForEntityId(entityID));
        }
        if (thisEntity) {
            QMetaObject::invokeMethod(thisEntity.get(), "hoverLeaveEntity", Q_ARG(const PointerEvent&, event));
        }
    });
}

EntityTreeRenderer::~EntityTreeRenderer() {
    // NOTE: We don't need to delete _entitiesScriptEngine because
    //       it is registered with ScriptEngines, which will call deleteLater for us.
}

EntityRendererPointer EntityTreeRenderer::renderableForEntityId(const EntityItemID& id) const {
    auto itr = _entitiesInScene.find(id); // ? how is this list built to asssure its only renderable
    if (itr == _entitiesInScene.end()) {
        return EntityRendererPointer();
    }
    return itr->second;
}

render::ItemID EntityTreeRenderer::renderableIdForEntityId(const EntityItemID& id) const {
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        return renderable->getRenderItemID();
    } else {
        return render::Item::INVALID_ITEM_ID;
    }
}

int EntityTreeRenderer::_entitiesScriptEngineCount = 0;

void EntityTreeRenderer::resetEntitiesScriptEngine() {
    _entitiesScriptEngine = scriptEngineFactory(ScriptEngine::ENTITY_CLIENT_SCRIPT, NO_SCRIPT,
                                                QString("about:Entities %1").arg(++_entitiesScriptEngineCount));
    DependencyManager::get<ScriptEngines>()->runScriptInitializers(_entitiesScriptEngine);
    _entitiesScriptEngine->runInThread();
    auto entitiesScriptEngineProvider = qSharedPointerCast<EntitiesScriptEngineProvider>(_entitiesScriptEngine);
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    entityScriptingInterface->setEntitiesScriptEngine(entitiesScriptEngineProvider);

    connect(_entitiesScriptEngine.data(), &ScriptEngine::entityScriptPreloadFinished, [&](const EntityItemID& entityID) {
        EntityItemPointer entity = getTree()->findEntityByID(entityID);
        if (entity) {
            entity->setScriptHasFinishedPreload(true);
        }
    });

    // Connect mouse events to entity script callbacks
    if (!_mouseAndPreloadSignalHandlersConnected) {
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::mousePressOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "mousePressOnEntity", event);
        });
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseDoublePressOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseDoublePressOnEntity", event);
        });
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseMoveOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseMoveOnEntity", event);
            // FIXME: this is a duplicate of mouseMoveOnEntity, but it seems like some scripts might use this naming
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseMoveEvent", event);
        });
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseReleaseOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseReleaseOnEntity", event);
        });

        connect(entityScriptingInterface.data(), &EntityScriptingInterface::clickDownOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "clickDownOnEntity", event);
        });
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::holdingClickOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "holdingClickOnEntity", event);
        });
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::clickReleaseOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "clickReleaseOnEntity", event);
        });

        connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverEnterEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "hoverEnterEntity", event);
        });
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverOverEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "hoverOverEntity", event);
        });
        connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverLeaveEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "hoverLeaveEntity", event);
        });

        _mouseAndPreloadSignalHandlersConnected = true;
    }
}

void EntityTreeRenderer::stopDomainAndNonOwnedEntities() {
    leaveDomainAndNonOwnedEntities();
    // unload and stop the engine
    if (_entitiesScriptEngine) {
        QList<EntityItemID> entitiesWithEntityScripts = _entitiesScriptEngine->getListOfEntityScriptIDs();
        int unloaded = 0, unloadedGone = 0, kept = 0;

        foreach (const EntityItemID& entityID,  entitiesWithEntityScripts) {
            EntityItemPointer entityItem = getTree()->findEntityByEntityItemID(entityID);

            // a script whose entity is no longer in the tree is a domain entity's (local and avatar entities stay): it
            // goes too, or it runs on with no entity (Tivoli Town's hideByDistance.js after a Cancel, 10-22)
            if (!entityItem) {
                _entitiesScriptEngine->unloadEntityScript(entityID, true);
                unloadedGone++;
                continue;
            }
            if (!entityItem->getScript().isEmpty()) {
                if (!(entityItem->isLocalEntity() || entityItem->isMyAvatarEntity())) {
                    if (_currentEntitiesInside.contains(entityID)) {
                        _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
                    }
                    _entitiesScriptEngine->unloadEntityScript(entityID, true);
                    unloaded++;
                } else {
                    kept++;
                }
            }
        }
        qCDebug(entitiesrenderer) << "leaving the world's entities: of" << entitiesWithEntityScripts.size()
                                 << "entity scripts listed," << unloaded << "unloaded," << unloadedGone
                                 << "unloaded whose entities were already gone," << kept << "kept (local or avatar)";
    }
}

void EntityTreeRenderer::clearDomainAndNonOwnedEntities() {
    stopDomainAndNonOwnedEntities();
    resetZoneCulling();

    std::unordered_map<EntityItemID, EntityRendererPointer> savedEntities;
    std::unordered_set<EntityRendererPointer> savedRenderables;
    auto scene = _viewState->getMain3DScene();

    if (scene) {
        for (const auto& entry :  _entitiesInScene) {
            const auto& renderer = entry.second;
            const EntityItemPointer& entityItem = renderer->getEntity();
            if (entityItem && !(entityItem->isLocalEntity() || entityItem->isMyAvatarEntity())) {
                fadeOutRenderable(renderer);
            } else {
                savedEntities[entry.first] = entry.second;
                savedRenderables.insert(entry.second);
            }
        }
    }

    _renderablesToUpdate = savedRenderables;
    _entitiesInScene = savedEntities;

    if (_layeredZones.clearDomainAndNonOwnedZones()) {
        applyLayeredZones();
    }

    OctreeProcessor::clearDomainAndNonOwnedEntities();

    // the world's entities are erased now: a script that started on the script thread since the unloads above were
    // asked for (a load queued before the world was left) is unloaded after them
    if (_entitiesScriptEngine) {
        _entitiesScriptEngine->unloadEntityScriptsOfGoneEntities();
    }
}

void EntityTreeRenderer::clear() {
    leaveAllEntities();
    resetZoneCulling();
    // unload and stop the engine
    if (_entitiesScriptEngine) {
        // do this here (instead of in deleter) to avoid marshalling unload signals back to this thread
        _entitiesScriptEngine->unloadAllEntityScripts(true);
        _entitiesScriptEngine->stop();
    }

    // reset the engine
    auto scene = _viewState->getMain3DScene();
    if (_shuttingDown) {
        if (scene) {
            render::Transaction transaction;
            for (const auto& entry : _entitiesInScene) {
                const auto& renderer = entry.second;
                renderer->removeFromScene(scene, transaction);
            }
            scene->enqueueTransaction(transaction);
        }
    } else {
        if (_wantScripts) {
            resetEntitiesScriptEngine();
        }
        if (scene) {
            for (const auto& entry : _entitiesInScene) {
                const auto& renderer = entry.second;
                fadeOutRenderable(renderer);
            }
        } else {
            qCWarning(entitiesrenderer) << "EntitityTreeRenderer::clear(), Unexpected null scene";
        }
    }

    _entitiesInScene.clear();
    _renderablesToUpdate.clear();
    _priorityRenderablesToUpdate.clear();
    _staticRenderablesToUpdate.clear();
    resetZoneCulling();
    {
        std::lock_guard<std::mutex> lock(_deferredModelMeshesLock);
        _deferredModelMeshes.clear();
    }
    _layeredZones.clear();

    if (!_shuttingDown) {
        applyLayeredZones();
    }

    OctreeProcessor::clear();
}

void EntityTreeRenderer::reloadEntityScripts() {
    _entitiesScriptEngine->unloadAllEntityScripts();
    _entitiesScriptEngine->resetModuleCache();
    for (const auto& entry : _entitiesInScene) {
        const auto& renderer = entry.second;
        const auto& entity = renderer->getEntity();
        QString scriptUrl = entity->getScriptToRun();
        if (!scriptUrl.isEmpty()) {
            _entitiesScriptEngine->loadEntityScript(entity->getEntityItemID(), resolveScriptURL(scriptUrl), true);
        }
    }
}

void EntityTreeRenderer::init() {
    OctreeProcessor::init();
    EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);

    DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    connect(&domainHandler, &DomainHandler::settingsReceived, this, &EntityTreeRenderer::connectedToDomain);

    // content from this computer is refused from the start, so a world's first packets can't carry it in before the
    // world is known, and decided again at every domain change. The decision runs right on the domain handler's
    // thread (direct): entity packets are read on the packet thread, and a decision queued to the main thread could
    // run after a new world's first packets; the flag is atomic and the decision touches nothing else.
    entityTree->setRefusesContentFromThisComputer(true);
    auto decide = &EntityTreeRenderer::updateContentFromThisComputer;
    connect(&domainHandler, &DomainHandler::connectedToDomain, this, decide, Qt::DirectConnection);
    connect(&domainHandler, &DomainHandler::settingsReceived, this, decide, Qt::DirectConnection);
    connect(&domainHandler, &DomainHandler::domainURLChanged, this, decide, Qt::DirectConnection);
    connect(&domainHandler, &DomainHandler::disconnectedFromDomain, this, decide, Qt::DirectConnection);

    if (_wantScripts) resetEntitiesScriptEngine();

    forceRecheckEntities(); // setup our state to force checking our inside/outsideness of entities

    connect(entityTree.get(), &EntityTree::deletingEntity, this, &EntityTreeRenderer::deletingEntity, Qt::QueuedConnection);
    connect(entityTree.get(), &EntityTree::addingEntity, this, &EntityTreeRenderer::addingEntity, Qt::QueuedConnection);
    connect(entityTree.get(), &EntityTree::entityScriptChanging, this, &EntityTreeRenderer::entityScriptChanging, Qt::QueuedConnection);

    resetZoneCulling();
}

void EntityTreeRenderer::updateContentFromThisComputer() {
    EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);
    if (!entityTree) {
        return;
    }
    const DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    // between worlds (no address yet) nothing is local; a serverless world file decides for its own tree
    // (DomainSession::prepareServerlessDomainContents), so this tree keeps refusing while it is the one; under the
    // strict network rule (DARKRIDE_SCRIPT_NETWORK_STRICT=1) every world counts as remote, as for the scripts
    const bool local = domainHandler.isConnected() && !domainHandler.isServerless() &&
                       !ScriptNetworkPolicy::isStrict() && ScriptNetworkPolicy::isWorldLocal();
    entityTree->setRefusesContentFromThisComputer(!local);
    const int decision = local ? 0 : 1;
    if (_contentFromThisComputerLogged.exchange(decision) != decision) {
        qCDebug(entitiesrenderer) << "Content addresses naming a file on this computer:"
                                  << (local ? "allowed (a local world)" : "refused (the world isn't local)")
                                  << domainHandler.getHostname() << domainHandler.getIP().toString();
    }
}

void EntityTreeRenderer::connectedToDomain() {
    setSceneIsReady(false);
    MeshPartPayload::sceneIsReady = false;
    resetZoneCulling();
    _staticRenderablesToUpdate.clear();
    forceRecheckEntities();
}

void EntityTreeRenderer::shutdown() {
    if (_entitiesScriptEngine) {
        _entitiesScriptEngine->disconnectNonEssentialSignals(); // disconnect all slots/signals from the script engine, except essential
    }
    {
        std::unique_lock<std::mutex> lock(_spaceLock);
        _shuttingDown = true;
    }
    // the space-update connections are direct calls (addPendingEntities): an entity still held elsewhere after the
    // tree is cleared must not call into a renderer whose members are going, so every one is cut here, before clear()
    if (_tree) {
        _tree->withReadLock([&] {
            _tree->recurseTreeWithOperation([&](const OctreeElementPointer& element, void*) {
                std::static_pointer_cast<EntityTreeElement>(element)->forEachEntity([&](const EntityItemPointer& entity) {
                    QObject::disconnect(entity.get(), &EntityItem::spaceUpdate, this, &EntityTreeRenderer::handleSpaceUpdate);
                });
                return true;
            });
        });
    }

    clear(); // always clear() on shutdown
}

void EntityTreeRenderer::addPendingEntities(const render::ScenePointer& scene, render::Transaction& transaction) {
    PROFILE_RANGE_EX(simulation_physics, "AddToScene", 0xffff00ff, (uint64_t)_entitiesToAdd.size());
    PerformanceTimer pt("add");
    // Clear any expired entities
    // FIXME should be able to use std::remove_if, but it fails due to some
    // weird compilation error related to EntityItemID assignment operators
    for (auto itr = _entitiesToAdd.begin(); _entitiesToAdd.end() != itr; ) {
        if (itr->second.expired()) {
            _entitiesToAdd.erase(itr++);
        } else {
            ++itr;
        }
    }

    if (!_entitiesToAdd.empty()) {
        std::unordered_set<EntityItemID> processedIds;
        for (const auto& entry : _entitiesToAdd) {
            auto entity = entry.second.lock();
            if (!entity) {
                continue;
            }

            // Path to the parent transforms is not valid,
            // don't add to the scene graph yet
            if (!entity->isParentPathComplete()) {
                continue;
            }
            if (entity->getSpaceIndex() == -1) {
                // the sphere is read before _spaceLock is taken: a parent lookup reads the tree, and the packet thread
                // can hold the tree's write lock while it waits for _spaceLock in handleSpaceUpdate (master plan entry 10)
                workload::Sphere sphere(entity->getWorldPosition(), entity->getBoundingRadius());
                std::unique_lock<std::mutex> lock(_spaceLock);
                auto spaceIndex = _space->allocateID();
                workload::Transaction transaction;
                SpatiallyNestablePointer nestable = std::static_pointer_cast<SpatiallyNestable>(entity);
                transaction.reset(spaceIndex, sphere, workload::Owner(nestable));
                _space->enqueueTransaction(transaction);
                entity->setSpaceIndex(spaceIndex);
                // a direct call, not a queued one: a queued delivery of the custom-type argument copied and destroyed it
                // through Qt's global metatype registry lock twice per moving entity per frame, and while loaders parsed
                // models the ride's frame waited behind them (the lock convoy, master plan entry 10). The slot only
                // appends under _spaceLock, whichever thread emits
                connect(entity.get(), &EntityItem::spaceUpdate, this, &EntityTreeRenderer::handleSpaceUpdate, Qt::DirectConnection);
            }

            auto entityID = entity->getEntityItemID();
            // One renderer per entity: an entity can be asked for again after its renderer was made (SafeLanding asks
            // for a renderer for a nearby entity that has none yet, and then the tree's own queued addingEntity comes
            // in). A second renderer was not kept in _entitiesInScene, yet its first update gave the entity its own
            // model, so the kept renderer's model was never readied or added and neither drew: Tivoli Town's ground,
            // cliffs and grass, one arrival in three (2026-10-22)
            auto existing = _entitiesInScene.find(entityID);
            if (existing != _entitiesInScene.end() && existing->second && existing->second->getEntity() == entity) {
                qCDebug(entitiesrenderer) << "not making a second renderer for" << entity->getName() << entityID;
                processedIds.insert(entityID);
                continue;
            }
            auto renderable = EntityRenderer::addToScene(*this, entity, scene, transaction);
            if (renderable) {
                _entitiesInScene.insert({ entityID, renderable });
                processedIds.insert(entityID);
            }
        }

        if (!processedIds.empty()) {
            for (const auto& processedId : processedIds) {
                _entitiesToAdd.erase(processedId);
            }
            forceRecheckEntities();
        }
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
// the staleness stamps (the inspector's "frames behind" and the deferred-age counter): a refresh clears the wait and
// records how late it came, in frames
static void markRenderableRefreshed(const EntityRendererPointer& renderable, uint64_t frame) {
    uint64_t since = renderable->getDeferredSinceFrame();
    renderable->setLastLateFrames(since ? (uint32_t)(frame - since) : 0u);
    renderable->setDeferredSinceFrame(0);
}

// FROZEN (vehicle path): see the note at the top of this file.
int EntityTreeRenderer::getRenderUpdateFramesBehind(const EntityItemID& id) const {
    EntityRendererPointer renderable = renderableForEntityId(id);
    if (!renderable) {
        return -1;
    }
    // behind only while it is actually waiting in the budget's set: a stamp a path outside the three lanes left
    // behind (performance's review, 2026-10-31) cannot then read as a wait that grows
    uint64_t since = renderable->getDeferredSinceFrame();
    if (since == 0 || _renderablesToUpdate.count(renderable) == 0) {
        return 0;
    }
    return (int)(_renderUpdateFrame - since + 1);
}

// FROZEN (vehicle path): see the note at the top of this file.
int EntityTreeRenderer::getRenderUpdateLastLateFrames(const EntityItemID& id) const {
    EntityRendererPointer renderable = renderableForEntityId(id);
    return renderable ? (int)renderable->getLastLateFrames() : -1;
}

// FROZEN (vehicle path): see the note at the top of this file.
void EntityTreeRenderer::updateChangedEntities(const render::ScenePointer& scene, render::Transaction& transaction) {
    PROFILE_RANGE_EX(simulation_physics, "ChangeInScene", 0xffff00ff, (uint64_t)_changedEntities.size());
    PerformanceTimer pt("change");
    std::unordered_set<EntityItemID> changedEntities;
    _changedEntitiesGuard.withWriteLock([&] {
        changedEntities.swap(_changedEntities);
    });
    const uint64_t frame = ++_renderUpdateFrame;
    _prevNumCarriedEntityUpdates = 0;
    // The parts a moving entity carries (a vehicle's walls and rails, a frame's pieces: its children and their
    // children) land here every frame, told by the parent's locationChanged, and went through the time budget below
    // with the rest of the world. In a heavy world the budget runs out; the big parent under the camera ranks high in
    // the priority sort and is refreshed every frame while a thin part ranks low and is refreshed every few frames,
    // so it is drawn one to four frames behind the body it rides and snaps forward (Port Amarelle's monorail car, a
    // wall creeping 2-3 px a frame then jumping 10 px, several times a second; performance, 2026-10-31: "6 of 8 left
    // for the next frame" every second of the ride). They are refreshed here, outside the budget, as
    // updateAvatarChildren does for what an avatar carries: a changed entity with a changed ancestor, and that
    // ancestor with it, so the parts and the body move as one. The budget stays for the rest (models and textures
    // arriving). A time ceiling of their own keeps a pathological mover (thousands of parts) from taking the frame:
    // past it the rest go back on the budget, as before.
    std::vector<EntityRendererPointer> carried;
    if (changedEntities.size() > 1) {
        const int MAX_ANCESTORS = 8;
        std::unordered_set<EntityItemID> carriedIds;
        for (const EntityItemID& entityId : changedEntities) {
            EntityItemPointer entity = getEntity(entityId);
            if (!entity) {
                continue;
            }
            QUuid parentID = entity->getParentID();
            for (int depth = 0; depth < MAX_ANCESTORS && !parentID.isNull(); ++depth) {
                if (changedEntities.count(EntityItemID(parentID))) {
                    carriedIds.insert(entityId);
                    carriedIds.insert(EntityItemID(parentID));
                    break;
                }
                EntityItemPointer parent = getEntity(parentID);
                if (!parent) {
                    break;
                }
                parentID = parent->getParentID();
            }
        }
        for (const EntityItemID& entityId : carriedIds) {
            EntityItemPointer entity = getEntity(entityId);
            if (entity && !entity->isDead()) {
                if (EntityRendererPointer renderable = renderableForEntityId(entityId)) {
                    carried.push_back(renderable);
                }
            }
            changedEntities.erase(entityId);
        }
    }
    if (!carried.empty()) {
        PROFILE_RANGE_EX(simulation_physics, "UpdateCarriedChildren", 0xffff00ff, (uint64_t)carried.size());
        const uint64_t MAX_CARRIED_TIME_US = 3000;
        const uint64_t carriedStart = usecTimestampNow();
        size_t refreshed = 0;
        uint64_t slowest = 0;
        for (const EntityRendererPointer& renderable : carried) {
            if (refreshed > 0 && usecTimestampNow() - carriedStart > MAX_CARRIED_TIME_US) {
                changedEntities.insert(renderable->getEntity()->getID());   // back on the budget
                continue;
            }
            const uint64_t start = usecTimestampNow();
            // as the budget's pass does it (the change stamp before the forced update: the shadow cache)
            bool changed = renderable->entityHasChanged();
            renderable->getEntity()->setNeedsRenderUpdate(true);
            renderable->updateInScene(scene, transaction, changed);
            markRenderableRefreshed(renderable, frame);
            slowest = std::max(slowest, usecTimestampNow() - start);
            refreshed++;
        }
        _prevNumCarriedEntityUpdates = (int)refreshed;
        // a moving assembly in view, for the refresh regime (getLastMovingAssemblyInViewTime): the carried parts'
        // bounds against the view, four times a second while something with parts moves, whatever the answer
        // (performance's review: throttled by the last test, so an assembly moving out of view costs the same)
        if (carriedStart - _lastMovingAssemblyTest > USECS_PER_SECOND / 4) {
            _lastMovingAssemblyTest = carriedStart;
            const auto& views = _viewState->getConicalViews();
            for (const EntityRendererPointer& renderable : carried) {
                const EntityItemPointer& entity = renderable->getEntity();
                const glm::vec3 position = entity->getWorldPosition();
                const float radius = 0.5f * entity->getQueryAACube().getScale();
                bool inView = false;
                for (const auto& view : views) {
                    const glm::vec3 relative = position - view.getPosition();
                    if (view.intersects(relative, glm::length(relative), radius)) {
                        inView = true;
                        break;
                    }
                }
                if (inView) {
                    _lastMovingAssemblyInView = carriedStart;
                    break;
                }
            }
        }
        // TIVOLI_UPDATE_DIAG in the environment: a line a second with the count and the cost (a Box part's refresh
        // measured about a millisecond on the budget's pass, performance 2026-10-31, when it should be microseconds)
        static const bool CARRIED_DIAG = QProcessEnvironment::systemEnvironment().contains("TIVOLI_UPDATE_DIAG");
        static uint64_t lastCarriedDiag = 0;
        if (CARRIED_DIAG && carriedStart - lastCarriedDiag > USECS_PER_SECOND) {
            lastCarriedDiag = carriedStart;
            qCDebug(entitiesrenderer) << "carried updates:" << refreshed << "of" << carried.size() << "refreshed outside the budget in"
                << (usecTimestampNow() - carriedStart) << "us, slowest" << slowest << "us;" << changedEntities.size() << "other changed";
        }
    }
    // bool selectionPriority = false;

    // the renderables the last frame's time budget left over stay for this one, as High Fidelity had it (the sort below
    // ages them up): cleared here from 2020 on, they were lost, and a far model put off while its textures loaded was
    // never updated again (Tivoli Town's far trees on arrival, 2026-10-21)
    _priorityRenderablesToUpdate.clear();

    {   // build list of renderables and priority renderables. 
        PROFILE_RANGE_EX(simulation_physics, "CopyRenderables", 0xffff00ff, (uint64_t)changedEntities.size());
        for (const EntityItemID& entityId : changedEntities) {
            // quint16 t = 0; 
            const EntityRendererPointer& renderable = renderableForEntityId(entityId);
            if (getSceneIsReady()) {
                // no priority optimization until everything's loaded
                EntityItemPointer _entity = getEntity(entityId);
            
                if (!_entity || _entity->isDead()) continue;
                if (!renderable) continue;

                EntityPriority _entityPriority = _entity->getEntityPriority();

                if (_isEditMode || getBypassPrioritySorting()) {
                    _entityPriority = EntityPriority::AUTOMATIC;
                }

                // make sure avatar entities and shapes aren't somehow set to static
                if (
                    _entityPriority == EntityPriority::STATIC && 
                    (_entity->isAvatarEntity() || _entity->isPrimitiveShapeType())
                ) {
                    _entityPriority = EntityPriority::AUTOMATIC;
                }

                if (_entityPriority == EntityPriority::AUTOMATIC) { 
                    _renderablesToUpdate.insert(renderable);
                } else if (_entityPriority == EntityPriority::PRIORITIZED) {
                    _priorityRenderablesToUpdate.insert(renderable);
                } else if (_entityPriority == EntityPriority::STATIC) {
                    _staticRenderablesToUpdate.insert(renderable); 
                }
             }
             else 
             {
                 // a change can come in for an entity whose renderer is gone (deleted, or the last world's after a
                 // world change: the renderer's request is queued) or not made yet; as the branch above, no null
                 if (renderable) {
                     _renderablesToUpdate.insert(renderable);
                 }
             }
            
        }
    }

    {
        float expectedUpdateCost = _avgRenderableUpdateCost * _renderablesToUpdate.size();
        _prevTotalNeededEntityUpdates = _renderablesToUpdate.size();
       
        // Update all the priority items first
        // if (!_isEditMode && _priorityRenderablesToUpdate.size()>0)
        if (_priorityRenderablesToUpdate.size() > 0) {
            for (const auto& renderable : _priorityRenderablesToUpdate) {
                assert(renderable); // only valid renderables are added to _renderablesToUpdate
                if (renderable != nullptr) {                    
                    renderable->getEntity()->setNeedsRenderUpdate(true);
                    renderable->updateInScene(scene, transaction);
                    markRenderableRefreshed(renderable, frame);
                }
            }
        }
        _prevNumPriorityEntityUpdates = (int)_priorityRenderablesToUpdate.size();

        if (expectedUpdateCost < MAX_UPDATE_RENDERABLES_TIME_BUDGET) { 
            PROFILE_RANGE_EX(simulation_physics, "UpdateRenderables", 0xffff00ff, (uint64_t)_renderablesToUpdate.size());

            uint64_t updateStart = usecTimestampNow();

            for (const EntityRendererPointer& renderable : _renderablesToUpdate) {
                assert(renderable);  // only valid renderables are added to _renderablesToUpdate
                if (renderable != nullptr) {
                    // whether anything the renderer shows has changed, before the update is forced: the entity
                    // server re-sends every entity in an octree cell when one of them moves, and the shadow cache
                    // must not take the others for moving (the scene's change stamps)
                    bool changed = renderable->entityHasChanged();
                    renderable->getEntity()->setNeedsRenderUpdate(true);
                    renderable->updateInScene(scene, transaction, changed);
                    markRenderableRefreshed(renderable, frame);
                }
            }

            _prevNumEntityUpdates = _renderablesToUpdate.size();
            _prevNumDeferredEntityUpdates = 0;
            _prevOldestDeferredFrames = 0;
            size_t numRenderables = _prevNumEntityUpdates + 1;  // add one to avoid divide by zero
            _renderablesToUpdate.clear();
            float cost = (float)(usecTimestampNow() - updateStart) / (float)(numRenderables); // compute average per-renderable update cost
            const float BLEND = 0.1f;
            _avgRenderableUpdateCost = (1.0f - BLEND) * _avgRenderableUpdateCost + BLEND * cost;
        } else {
            // we expect the cost to updating all renderables to exceed available time budget
            // so we first sort by priority and update in order until out of time
            class SortableRenderer : public PrioritySortUtil::Sortable {
            public:
                SortableRenderer(const EntityRendererPointer& renderer) : _renderer(renderer) {}
                glm::vec3 getPosition() const override {
                    if (!_renderer) return glm::vec3(0.0f, 0.0f, 0.0f); // HACK: prevent a crash by returning 0,0,0 but this is probably not a good solution
                    return _renderer->getEntity()->getWorldPosition();
                }
                float getRadius() const override { return 0.5f * _renderer->getEntity()->getQueryAACube().getScale(); }
                uint64_t getTimestamp() const override { return _renderer->getUpdateTime(); }

                EntityRendererPointer getRenderer() const { return _renderer; }
            private:
                EntityRendererPointer _renderer;
            };

            // prioritize and sort the renderables
            uint64_t sortStart = usecTimestampNow();

            const auto& views = _viewState->getConicalViews();
            PrioritySortUtil::PriorityQueue<SortableRenderer> sortedRenderables(views);
            sortedRenderables.reserve(_renderablesToUpdate.size());
            {
                PROFILE_RANGE_EX(simulation_physics, "BuildSortedRenderables", 0xffff00ff, (uint64_t)_renderablesToUpdate.size());
                for (const auto& renderable : _renderablesToUpdate) {
                    assert(renderable); // only valid renderables are added to _renderablesToUpdate
                    if (renderable) sortedRenderables.push(SortableRenderer(renderable));
                }
            }
            {
                PROFILE_RANGE_EX(simulation_physics, "SortAndUpdateRenderables", 0xffff00ff, sortedRenderables.size());

                // compute remaining time budget
                const auto& sortedRenderablesVector = sortedRenderables.getSortedVector();
                uint64_t updateStart = usecTimestampNow();
                uint64_t sortCost = updateStart - sortStart;
                uint64_t timeBudget = MIN_SORTED_UPDATE_RENDERABLES_TIME_BUDGET;
                if (sortCost < MAX_UPDATE_RENDERABLES_TIME_BUDGET - MIN_SORTED_UPDATE_RENDERABLES_TIME_BUDGET) {
                    timeBudget = MAX_UPDATE_RENDERABLES_TIME_BUDGET - sortCost;
                }
                uint64_t expiry = updateStart + timeBudget;

                // process the sorted renderables
                for (const auto& sortedRenderable : sortedRenderablesVector) {
                    if (usecTimestampNow() > expiry) {
                        break;
                    }
                    const auto& renderable = sortedRenderable.getRenderer();
                    if (renderable != nullptr) {
                        renderable->getEntity()->setNeedsRenderUpdate(true);
                        renderable->updateInScene(scene, transaction);
                        markRenderableRefreshed(renderable, frame);
                    }
                    _renderablesToUpdate.erase(renderable);
                }
                // a null is never sorted, so it was never erased above: drop it before the leftovers are read (the
                // loop below crashed on one, 2026-10-31) and counted
                _renderablesToUpdate.erase(EntityRendererPointer());

                // the ones left for the next frame are stamped with the frame they were first left over, so their
                // age in frames is known (the counters, the inspector's frames behind, the log line); a loop over
                // the leftovers only, and only when the budget ran out
                int oldestFrames = 0;
                for (const auto& renderable : _renderablesToUpdate) {
                    if (renderable->getDeferredSinceFrame() == 0) {
                        renderable->setDeferredSinceFrame(frame);
                    }
                    oldestFrames = std::max(oldestFrames, (int)(frame - renderable->getDeferredSinceFrame() + 1));
                }
                _prevNumDeferredEntityUpdates = (int)_renderablesToUpdate.size();
                _prevOldestDeferredFrames = oldestFrames;

                // TIVOLI_UPDATE_DIAG in the environment: a log line a second while the budget leaves renderables over,
                // naming the ones left over (the oldest first, at most a dozen) with their age in frames
                static const bool UPDATE_DIAG = QProcessEnvironment::systemEnvironment().contains("TIVOLI_UPDATE_DIAG");
                static uint64_t lastUpdateDiag = 0;
                if (UPDATE_DIAG && !_renderablesToUpdate.empty() && updateStart - lastUpdateDiag > USECS_PER_SECOND) {
                    lastUpdateDiag = updateStart;
                    std::vector<EntityRendererPointer> leftovers(_renderablesToUpdate.begin(), _renderablesToUpdate.end());
                    std::sort(leftovers.begin(), leftovers.end(), [](const EntityRendererPointer& a, const EntityRendererPointer& b) {
                        return a->getDeferredSinceFrame() < b->getDeferredSinceFrame();
                    });
                    const size_t MAX_NAMED = 12;
                    QStringList named;
                    for (size_t i = 0; i < leftovers.size() && i < MAX_NAMED; i++) {
                        const EntityItemPointer& entity = leftovers[i]->getEntity();
                        QString name = entity->getName();
                        if (name.isEmpty()) {
                            name = entity->getID().toString().mid(1, 8);
                        }
                        named << QString("%1 (%2, %3 frames)").arg(name, EntityTypes::getEntityTypeName(entity->getType()))
                            .arg(frame - leftovers[i]->getDeferredSinceFrame() + 1);
                    }
                    if (leftovers.size() > MAX_NAMED) {
                        named << QString("and %1 more").arg(leftovers.size() - MAX_NAMED);
                    }
                    qCDebug(entitiesrenderer).noquote() << "renderable updates:" << _renderablesToUpdate.size() << "of"
                        << sortedRenderables.size() << "left for the next frame (budget" << timeBudget << "us), oldest"
                        << oldestFrames << "frames:" << named.join(", ");
                }

                // compute average per-renderable update cost
                _prevNumEntityUpdates = sortedRenderables.size() - _renderablesToUpdate.size();
                size_t numUpdated = _prevNumEntityUpdates + 1; // add one to avoid divide by zero
                float cost = (float)(usecTimestampNow() - updateStart) / (float)(numUpdated);
                const float BLEND = 0.1f;
                _avgRenderableUpdateCost = (1.0f - BLEND) * _avgRenderableUpdateCost + BLEND * cost;
            }
        }
        MeshPartPayload::sceneIsReady = true;
    }

    // the maxima over the last whole second, for a Stats line that holds still (a per-frame value flickers)
    _maxDeferredThisSecond = std::max(_maxDeferredThisSecond, _prevNumDeferredEntityUpdates);
    _maxDeferredFramesThisSecond = std::max(_maxDeferredFramesThisSecond, _prevOldestDeferredFrames);
    const uint64_t now = usecTimestampNow();
    if (now - _countersSecondStart >= USECS_PER_SECOND) {
        _countersSecondStart = now;
        _maxDeferredLastSecond = _maxDeferredThisSecond;
        _maxDeferredFramesLastSecond = _maxDeferredFramesThisSecond;
        _maxDeferredThisSecond = 0;
        _maxDeferredFramesThisSecond = 0;
    }

    // the frame trace's columns (FrameTrace.h, the instruments plan): seven set() calls, each one atomic load while
    // the trace is off
    FrameTrace::set("update.needed", _prevTotalNeededEntityUpdates);
    FrameTrace::set("update.done", _prevNumEntityUpdates);
    FrameTrace::set("update.carried", _prevNumCarriedEntityUpdates);
    FrameTrace::set("update.avatar", _prevNumAvatarChildUpdates);
    FrameTrace::set("update.priority", _prevNumPriorityEntityUpdates);
    FrameTrace::set("update.deferred", _prevNumDeferredEntityUpdates);
    FrameTrace::set("update.oldest_frames", _prevOldestDeferredFrames);
}

void EntityTreeRenderer::deferModelMesh(const EntityItemID& id, const glm::vec3& centerWS, float radius) {
    std::lock_guard<std::mutex> lock(_deferredModelMeshesLock);
    auto& entry = _deferredModelMeshes[id];
    entry.center = centerWS;
    entry.radius = radius;
    entry.wake = false;
}

void EntityTreeRenderer::undeferModelMesh(const EntityItemID& id) {
    std::lock_guard<std::mutex> lock(_deferredModelMeshesLock);
    _deferredModelMeshes.erase(id);
}

void EntityTreeRenderer::wakeDeferredModelMesh(const EntityItemID& id) {
    std::lock_guard<std::mutex> lock(_deferredModelMeshesLock);
    auto itr = _deferredModelMeshes.find(id);
    if (itr != _deferredModelMeshes.end()) {
        itr->second.wake = true;
    }
}

void EntityTreeRenderer::setImpostorLoadView(const glm::vec3& cameraPosition, float loadHalfTanSq) {
    _impostorLoadPosition = cameraPosition;
    _impostorLoadHalfTanSq = loadHalfTanSq;
}

// The waiting meshes whose time has come update now, whatever their entity's update priority; each one's renderer
// decides again, starts its download or puts it back here (ModelEntityRenderer::doRenderUpdateSynchronousTyped).
// One distance test each per frame.
void EntityTreeRenderer::updateDeferredModelMeshes(const render::ScenePointer& scene, render::Transaction& transaction) {
    std::vector<EntityItemID> due;
    {
        std::lock_guard<std::mutex> lock(_deferredModelMeshesLock);
        for (auto itr = _deferredModelMeshes.begin(); itr != _deferredModelMeshes.end();) {
            const auto& entry = itr->second;
            glm::vec3 toCamera = _impostorLoadPosition - entry.center;
            float distanceSq = glm::dot(toCamera, toCamera);
            float radiusSq = entry.radius * entry.radius;
            if (entry.wake || _impostorLoadHalfTanSq <= 0.0f || distanceSq <= radiusSq ||
                radiusSq >= _impostorLoadHalfTanSq * distanceSq) {
                due.push_back(itr->first);
                itr = _deferredModelMeshes.erase(itr);
            } else {
                ++itr;
            }
        }
    }
    if (!due.empty()) {
        qCDebug(entitiesrenderer) << "impostor stand-in:" << due.size() << "waiting meshes woken";
    }
    for (const auto& id : due) {
        auto renderable = renderableForEntityId(id);
        if (renderable) {
            renderable->getEntity()->setNeedsRenderUpdate(true);
            renderable->updateInScene(scene, transaction);
        }
    }
}

void EntityTreeRenderer::preUpdate() {
    if (_tree && !_shuttingDown) {
        _tree->preUpdate();
    }
}

void EntityTreeRenderer::updateAvatarChildren() {
    PROFILE_RANGE(simulation_physics, "ETR::updateAvatarChildren");
    if (!_tree || _shuttingDown) {
        return;
    }
    auto scene = _viewState->getMain3DScene();
    if (!scene) {
        return;
    }
    std::unordered_set<EntityItemID> changedEntities;
    _changedEntitiesGuard.withWriteLock([&] {
        changedEntities.swap(_changedEntities);
    });
    _prevNumAvatarChildUpdates = 0;
    if (changedEntities.empty()) {
        return;
    }

    // the ones under an avatar are updated now; the world's other changes go back for the next frame's pass, with its
    // time budget and priority sort (the dead and those without a renderable are dropped, as that pass drops them)
    std::vector<EntityRendererPointer> underAvatars;
    for (auto it = changedEntities.begin(); it != changedEntities.end();) {
        EntityItemPointer entity = getEntity(*it);
        if (entity && entity->hasAncestorOfType(NestableType::Avatar)) {
            if (!entity->isDead()) {
                if (EntityRendererPointer renderable = renderableForEntityId(*it)) {
                    underAvatars.push_back(renderable);
                }
            }
            it = changedEntities.erase(it);
        } else {
            ++it;
        }
    }
    if (!changedEntities.empty()) {
        _changedEntitiesGuard.withWriteLock([&] {
            _changedEntities.insert(changedEntities.begin(), changedEntities.end());
        });
    }
    if (underAvatars.empty()) {
        return;
    }

    // as the AUTOMATIC pass updates them; the stats and the budget's average cost are left to that pass. Outside the
    // budget: in a crowd every moving avatar's children land here each frame (avatars, 10-28), so the count is shown
    {
        PROFILE_RANGE_EX(simulation_physics, "UpdateAvatarChildren", 0xffff00ff, (uint64_t)underAvatars.size());
        render::Transaction transaction;
        for (const EntityRendererPointer& renderable : underAvatars) {
            bool changed = renderable->entityHasChanged();
            renderable->getEntity()->setNeedsRenderUpdate(true);
            renderable->updateInScene(scene, transaction, changed);
            markRenderableRefreshed(renderable, _renderUpdateFrame);
        }
        scene->enqueueTransaction(transaction);
        _prevNumAvatarChildUpdates = (int)underAvatars.size();
    }
}

void EntityTreeRenderer::update(bool simulate) {
    PROFILE_RANGE(simulation_physics, "ETR::update");
    PerformanceTimer perfTimer("ETRupdate");
    if (_tree && !_shuttingDown) {
        EntityTreePointer tree = std::static_pointer_cast<EntityTree>(_tree);

        // here we update _currentFrame and _lastAnimated and sync with the server properties.
        {
            PerformanceTimer perfTimer("tree::update");
            tree->update(simulate);
        }

        {  // Update the rendereable entities as needed
            PROFILE_RANGE(simulation_physics, "Scene");
            PerformanceTimer sceneTimer("scene");
            auto scene = _viewState->getMain3DScene();
            if (scene) { // ->getTransactionQueueSize() > 0) {
                render::Transaction transaction;
                addPendingEntities(scene, transaction);
                updateChangedEntities(scene, transaction);
                drainZoneCullFlips(scene, transaction);
                updateDeferredModelMeshes(scene, transaction);
                scene->enqueueTransaction(transaction);
            }
        }

        {
            PerformanceTimer perfTimer("workload::transaction");
            workload::Transaction spaceTransaction;
            {  // update proxies in the workload::Space
                std::unique_lock<std::mutex> lock(_spaceLock);
                spaceTransaction.update(_spaceUpdates);
                _spaceUpdates.clear();
            }
            {
                std::vector<int32_t> staleProxies;
                tree->swapStaleProxies(staleProxies);
                spaceTransaction.remove(staleProxies);
                {
                    std::unique_lock<std::mutex> lock(_spaceLock);
                    _space->enqueueTransaction(spaceTransaction);
                }
            }
        }

        if (simulate) {
            // Handle enter/leave entity logic
            checkEnterLeaveEntities();

            // Even if we're not moving the mouse, if we started clicking on an entity and we have
            // not yet released the hold then this is still considered a holdingClickOnEntity event
            // and we want to simulate this message here as well as in mouse move
            if (_lastPointerEventValid && !_currentClickingOnEntityID.isInvalidID()) {
                emit DependencyManager::get<EntityScriptingInterface>()->holdingClickOnEntity(_currentClickingOnEntityID, _lastPointerEvent);
            }
        }
    }
}

void EntityTreeRenderer::handleSpaceUpdate(std::pair<int32_t, glm::vec4> proxyUpdate) {
    std::unique_lock<std::mutex> lock(_spaceLock);
    // a direct call can come from the packet thread after shutdown() (an entity that outlives this renderer): nothing
    // is touched past the lock and the flag, which shutdown() set under the same lock
    if (_shuttingDown) {
        return;
    }
    _spaceUpdates.emplace_back(proxyUpdate.first, proxyUpdate.second);
}

void EntityTreeRenderer::findBestZoneAndMaybeContainingEntities(QSet<EntityItemID>& entitiesContainingAvatar) {
    float radius = 0.01f; // for now, assume 0.01 meter radius, because we actually check the point inside later
    QVector<QUuid> entityIDs;

    // find the entities near us
    // don't let someone else change our tree while we search
    _tree->withReadLock([&] {
        auto entityTree = std::static_pointer_cast<EntityTree>(_tree);

        // FIXME - if EntityTree had a findEntitiesContainingPoint() this could theoretically be a little faster
        entityTree->evalEntitiesInSphere(_avatarPosition, radius, PickFilter(), entityIDs);

        LayeredZones oldLayeredZones(_layeredZones);
        _layeredZones.clear();

        // create a list of entities that actually contain the avatar's position
        for (auto& entityID : entityIDs) {
            auto entity = entityTree->findEntityByID(entityID);
            if (!entity) {
                continue;
            }

            auto isZone = entity->getType() == EntityTypes::Zone;
            auto hasScript = !entity->getScriptToRun().isEmpty();

            // only consider entities that are zones or have scripts, all other entities can
            // be ignored because they can't have events fired on them.
            // FIXME - this could be optimized further by determining if the script is loaded
            // and if it has either an enterEntity or leaveEntity method
            //
            // also, don't flag a scripted entity as containing the avatar until the script is loaded,
            // so that the script is awake in time to receive the "entityEntity" call (even if the entity is a zone).
            bool contains = false;
            bool scriptHasLoaded = hasScript && entity->isScriptPreloadFinished();
            if (isZone || scriptHasLoaded) {
                contains = entity->contains(_avatarPosition);
            }

            if (contains) {
                // if this entity is a zone and visible, add it to our layered zones
                if (isZone && entity->getVisible() && renderableIdForEntity(entity) != render::Item::INVALID_ITEM_ID) {
                    _layeredZones.emplace_back(std::dynamic_pointer_cast<ZoneEntityItem>(entity));
                }

                if ((!hasScript && isZone) || scriptHasLoaded) {
                    entitiesContainingAvatar << entity->getEntityItemID();
                }
            }
        }

        _layeredZones.sort();
        if (!_layeredZones.equals(oldLayeredZones)) {
            applyLayeredZones();
        }
    });
}

void EntityTreeRenderer::setZoneCullingSuspended(bool suspended) {
    if (_zoneCullingSuspended == suspended) {
        return;
    }
    _zoneCullingSuspended = suspended;
    _forceRecheckEntities = true;
}

bool EntityTreeRenderer::ZoneCullState::touches(const glm::vec3& center, float radius) const {
    const glm::vec3 local = glm::conjugate(rotation) * (center - position);
    const glm::vec3 closest = glm::clamp(local, -halfDimensions, halfDimensions);
    const glm::vec3 away = local - closest;
    return glm::dot(away, away) <= radius * radius;
}

float EntityTreeRenderer::ZoneCullState::reachFrom(const glm::vec3& point) const {
    float reach = 0.0f;
    for (int corner = 0; corner < 8; corner++) {
        const glm::vec3 local((corner & 1) ? halfDimensions.x : -halfDimensions.x,
                              (corner & 2) ? halfDimensions.y : -halfDimensions.y,
                              (corner & 4) ? halfDimensions.z : -halfDimensions.z);
        reach = glm::max(reach, glm::distance(position + rotation * local, point));
    }
    return reach;
}

bool EntityTreeRenderer::isZoneCullable(const EntityItemPointer& entity) {
    if (!entity || entity->isLocalEntity() || entity->isAvatarEntity()) {
        return false;
    }
    switch (entity->getType()) {
        case EntityTypes::Zone:
        case EntityTypes::Gizmo:
        case EntityTypes::Grid:
        case EntityTypes::Material:
            return false;
        default:
            return true;
    }
}

void EntityTreeRenderer::zoneCullingZoneChanged(const EntityItemID& id) {
    if (id == _zoneCullState.zoneId) {
        _zoneCullDirty = true;
    }
    _forceRecheckEntities = true;
}

void EntityTreeRenderer::resetZoneCulling() {
    _zoneCullState = ZoneCullState();
    _zoneCullDirty = false;
    _zoneCullFlips.clear();
    _zoneCullFlipsNext = 0;
}

// The deciding zone, from the zones the avatar stands in (sorted smallest first): the innermost whose mode is not
// "inherit". Compared by its id, mode and the suspension, plus the dirty mark the zone's own renderer sets when the
// zone moves, turns, resizes or changes mode: no transform is read here on the frames nothing changed (on a ride
// this runs every frame).
void EntityTreeRenderer::updateZoneCullState() {
    std::shared_ptr<ZoneEntityItem> deciding;
    uint32_t mode = ZONE_CULLING_INHERIT;
    for (const auto& layered : _layeredZones) {
        auto zone = layered.zone.lock();
        if (!zone) {
            continue;
        }
        uint32_t zoneMode = zone->getZoneCullingMode();
        if (zoneMode != ZONE_CULLING_INHERIT) {
            deciding = zone;
            mode = zoneMode;
            break;
        }
    }
    if (!_layeredZones.empty()) {
        _ghostingAllowed = _layeredZones.getZoneInteractionProperties().second;
    }
    // a mode this client does not know (one added later) culls as "outside": a newer world degrades to plain culling
    const bool active = deciding && !_zoneCullingSuspended && mode != ZONE_CULLING_DISABLED;
    const QUuid zoneId = active ? deciding->getID() : QUuid();
    const bool dontLoad = active && mode == ZONE_CULLING_OUTSIDE_NO_LOAD;
    if (zoneId == _zoneCullState.zoneId && active == _zoneCullState.active && dontLoad == _zoneCullState.dontLoad &&
        !_zoneCullDirty) {
        return;
    }
    _zoneCullDirty = false;
    _zoneCullState.active = active;
    _zoneCullState.dontLoad = dontLoad;
    _zoneCullState.zoneId = zoneId;
    if (active) {
        bool success = false;
        Transform toCenter = deciding->getTransformToCenter(success);
        _zoneCullState.position = success ? toCenter.getTranslation() : deciding->getWorldPosition();
        _zoneCullState.rotation = success ? toCenter.getRotation() : deciding->getWorldOrientation();
        _zoneCullState.halfDimensions = deciding->getScaledDimensions() * 0.5f;
    }
    decideZoneCulling();
}

// Once per change of the deciding zone: every renderer in the scene gets its flag from the zone's box and the
// entity's bound (no octree query; one sphere test each). The renderers whose flag flipped, and any still waiting
// from the last decision, are queued nearest first for the lane.
void EntityTreeRenderer::decideZoneCulling() {
    const uint64_t start = usecTimestampNow();
    std::unordered_set<EntityRendererPointer> pending(_zoneCullFlips.begin() + _zoneCullFlipsNext, _zoneCullFlips.end());
    for (const auto& entry : _entitiesInScene) {
        const auto& renderable = entry.second;
        if (!renderable) {
            continue;
        }
        const auto& entity = renderable->getEntity();
        bool culled = false;
        if (_zoneCullState.active && isZoneCullable(entity)) {
            const auto& bound = renderable->getWorldBound();
            culled = !_zoneCullState.touches(bound.calcCenter(), 0.5f * bound.getLargestDimension() * SQRT_THREE);
        }
        if (renderable->isZoneCulled() != culled) {
            renderable->setZoneCulled(culled);
            pending.insert(renderable);
        }
    }
    std::vector<std::pair<float, EntityRendererPointer>> sorted;
    sorted.reserve(pending.size());
    for (const auto& renderable : pending) {
        sorted.emplace_back(glm::distance2(renderable->getWorldBound().calcCenter(), _avatarPosition), renderable);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    _zoneCullFlips.clear();
    _zoneCullFlipsNext = 0;
    _zoneCullFlips.reserve(sorted.size());
    for (const auto& entry : sorted) {
        _zoneCullFlips.push_back(entry.second);
    }
    const double decideMs = (usecTimestampNow() - start) / 1000.0;
    FrameTrace::set("zonecull.decide_ms", decideMs);
    qCDebug(entitiesrenderer) << "zone culling" << (_zoneCullState.active ? "on" : "off") << "for" << _entitiesInScene.size()
                              << "entities:" << sorted.size() << "change, decided in" << decideMs << "ms";
}

// The lane (entry 13 (c), (e)): after the budget, the flipped renderers are refreshed nearest first inside 0.5 ms a
// frame; then, with the queue empty and time left, up to 8 changed Static entities (the budget's own loop leaves them
// in _staticRenderablesToUpdate: Static means applied late, not per frame). On a frame where the budget overran (it
// left renderables for the next frame) the lane yields entirely, so a crossing never stacks on a frame with no room.
void EntityTreeRenderer::drainZoneCullFlips(const render::ScenePointer& scene, render::Transaction& transaction) {
    static const uint64_t LANE_BUDGET_USECS = 500;
    static const int STATICS_PER_FRAME = 8;
    int flips = 0;
    int statics = 0;
    double laneMs = 0.0;
    const bool pendingFlips = _zoneCullFlipsNext < _zoneCullFlips.size();
    if ((pendingFlips || !_staticRenderablesToUpdate.isEmpty()) && _prevNumDeferredEntityUpdates == 0) {
        const uint64_t start = usecTimestampNow();
        const uint64_t expiry = start + LANE_BUDGET_USECS;
        while (_zoneCullFlipsNext < _zoneCullFlips.size() && usecTimestampNow() <= expiry) {
            const EntityRendererPointer renderable = _zoneCullFlips[_zoneCullFlipsNext++];
            if (renderable && renderable->getEntity() && !renderable->getEntity()->isDead()) {
                renderable->getEntity()->setNeedsRenderUpdate(true);
                renderable->updateInScene(scene, transaction);
                flips++;
            }
        }
        if (_zoneCullFlipsNext >= _zoneCullFlips.size()) {
            _zoneCullFlips.clear();
            _zoneCullFlipsNext = 0;
        }
        while (_zoneCullFlips.empty() && statics < STATICS_PER_FRAME && !_staticRenderablesToUpdate.isEmpty() &&
               usecTimestampNow() <= expiry) {
            auto first = _staticRenderablesToUpdate.begin();
            const EntityRendererPointer renderable = *first;
            _staticRenderablesToUpdate.erase(first);
            if (renderable && renderable->getEntity() && !renderable->getEntity()->isDead()) {
                renderable->getEntity()->setNeedsRenderUpdate(true);
                renderable->updateInScene(scene, transaction);
                statics++;
            }
        }
        laneMs = (usecTimestampNow() - start) / 1000.0;
    }
    FrameTrace::set("zonecull.lane_ms", laneMs);
    FrameTrace::set("zonecull.flips", flips);
    FrameTrace::set("zonecull.statics", statics);
}

void EntityTreeRenderer::checkEnterLeaveEntities() {
    PROFILE_RANGE(simulation_physics, "EnterLeave");
    PerformanceTimer perfTimer("enterLeave");
    auto now = usecTimestampNow();

    if (_tree && !_shuttingDown) {
        glm::vec3 avatarPosition = _viewState->getAvatarPosition();

        // we want to check our enter/leave state if we've moved a significant amount, or
        // if some amount of time has elapsed since we last checked. We check the time
        // elapsed because zones or entities might have been created "around us" while we've
        // been stationary
        auto movedEnough = glm::distance(avatarPosition, _avatarPosition) > ZONE_CHECK_DISTANCE;
        auto enoughTimeElapsed = (now - _lastZoneCheck) > ZONE_CHECK_INTERVAL;

        if (_forceRecheckEntities || movedEnough || enoughTimeElapsed) {
            _avatarPosition = avatarPosition;
            _lastZoneCheck = now;

            QSet<EntityItemID> entitiesContainingAvatar;

            findBestZoneAndMaybeContainingEntities(entitiesContainingAvatar);  // eCA now has a list of all the zones where the avatar is located
            _forceRecheckEntities = false;
            updateZoneCullState();

            // Note: at this point we don't need to worry about the tree being locked, because we only deal with
            // EntityItemIDs from here. The callEntityScriptMethod() method is robust against attempting to call scripts
            // for entity IDs that no longer exist.

            if (_entitiesScriptEngine) {
                // for all of our previous containing entities, if they are no longer containing then send them a leave event
                foreach(const EntityItemID& entityID, _currentEntitiesInside) {
                    if (!entitiesContainingAvatar.contains(entityID)) {
                        emit leaveEntity(entityID);
                        _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
                    }
                }

                // for all of our new containing entities, if they weren't previously containing then send them an enter event
                foreach(const EntityItemID& entityID, entitiesContainingAvatar) {
                    if (!_currentEntitiesInside.contains(entityID)) {
                        emit enterEntity(entityID);
                        _entitiesScriptEngine->callEntityScriptMethod(entityID, "enterEntity");
                    }
                }
                _currentEntitiesInside = entitiesContainingAvatar;
            }
        }
    }
}

void EntityTreeRenderer::leaveDomainAndNonOwnedEntities() {
    if (_tree && !_shuttingDown) {
        QSet<EntityItemID> currentEntitiesInsideToSave;
        foreach (const EntityItemID& entityID, _currentEntitiesInside) {
            EntityItemPointer entityItem = getTree()->findEntityByEntityItemID(entityID);
            if (entityItem && !(entityItem->isLocalEntity() || entityItem->isMyAvatarEntity())) {
                emit leaveEntity(entityID);
                if (_entitiesScriptEngine) {
                    _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
                }
            } else {
                currentEntitiesInsideToSave.insert(entityID);
            }
        }

        _currentEntitiesInside = currentEntitiesInsideToSave;
        forceRecheckEntities();
    }
}

void EntityTreeRenderer::leaveAllEntities() {
    if (_tree && !_shuttingDown) {

        // for all of our previous containing entities, if they are no longer containing then send them a leave event
        foreach(const EntityItemID& entityID, _currentEntitiesInside) {
            emit leaveEntity(entityID);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
            }
        }
        _currentEntitiesInside.clear();
        forceRecheckEntities();
    }
}

void EntityTreeRenderer::forceRecheckEntities() {
    _forceRecheckEntities = true;
}

bool EntityTreeRenderer::applyLayeredZones() {
    // from the list of zones we are going to build a selection list the Render Item corresponding to the zones
    // in the expected layered order and update the scene with it
    auto scene = _viewState->getMain3DScene();
    if (scene) {
        render::ItemIDs list;
        _layeredZones.appendRenderIDs(list, this);

        render::Selection selection("RankedZones", list);
        render::Transaction transaction;
        transaction.resetSelection(selection);
        scene->enqueueTransaction(transaction);
    } else {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::applyLayeredZones(), Unexpected null scene, possibly during application shutdown";
    }

    return true;
}

void EntityTreeRenderer::processEraseMessage(ReceivedMessage& message, const SharedNodePointer& sourceNode) {
    OCTREE_PACKET_FLAGS flags;
    message.readPrimitive(&flags);

    OCTREE_PACKET_SEQUENCE sequence;
    message.readPrimitive(&sequence);

    _lastOctreeMessageSequence = sequence;
    message.seek(0);
    std::static_pointer_cast<EntityTree>(_tree)->processEraseMessage(message, sourceNode);
}

void EntityTreeRenderer::connectSignalsToSlots(EntityScriptingInterface* entityScriptingInterface) {
    connect(this, &EntityTreeRenderer::enterEntity, entityScriptingInterface, &EntityScriptingInterface::enterEntity);
    connect(this, &EntityTreeRenderer::leaveEntity, entityScriptingInterface, &EntityScriptingInterface::leaveEntity);
    connect(this, &EntityTreeRenderer::collisionWithEntity, entityScriptingInterface, &EntityScriptingInterface::collisionWithEntity);

    connect(DependencyManager::get<SceneScriptingInterface>().data(), &SceneScriptingInterface::shouldRenderEntitiesChanged, this, &EntityTreeRenderer::updateEntityRenderStatus, Qt::QueuedConnection);
}

static glm::vec2 projectOntoEntityXYPlane(EntityItemPointer entity, const PickRay& pickRay, const RayToEntityIntersectionResult& rayPickResult) {

    if (entity) {

        glm::vec3 entityPosition = entity->getWorldPosition();
        glm::quat entityRotation = entity->getWorldOrientation();
        glm::vec3 entityDimensions = entity->getScaledDimensions();
        glm::vec3 entityRegistrationPoint = entity->getRegistrationPoint();

        // project the intersection point onto the local xy plane of the object.
        float distance;
        glm::vec3 planePosition = entityPosition;
        glm::vec3 planeNormal = entityRotation * Vectors::UNIT_Z;
        glm::vec3 rayDirection = pickRay.direction;
        glm::vec3 rayStart = pickRay.origin;
        glm::vec3 p;
        if (rayPlaneIntersection(planePosition, planeNormal, rayStart, rayDirection, distance)) {
            p = rayStart + rayDirection * distance;
        } else {
            p = rayPickResult.intersection;
        }
        glm::vec3 localP = glm::inverse(entityRotation) * (p - entityPosition);
        glm::vec3 normalizedP = (localP / entityDimensions) + entityRegistrationPoint;
        return glm::vec2(normalizedP.x * entityDimensions.x,
                         (1.0f - normalizedP.y) * entityDimensions.y);  // flip y-axis
    } else {
        return glm::vec2();
    }
}

static uint32_t toPointerButtons(const QMouseEvent& event) {
    uint32_t buttons = 0;
    buttons |= event.buttons().testFlag(Qt::LeftButton) ? PointerEvent::PrimaryButton : 0;
    buttons |= event.buttons().testFlag(Qt::RightButton) ? PointerEvent::SecondaryButton : 0;
    buttons |= event.buttons().testFlag(Qt::MiddleButton) ? PointerEvent::TertiaryButton : 0;
    return buttons;
}

static PointerEvent::Button toPointerButton(const QMouseEvent& event) {
    switch (event.button()) {
    case Qt::LeftButton:
        return PointerEvent::PrimaryButton;
    case Qt::RightButton:
        return PointerEvent::SecondaryButton;
    case Qt::MiddleButton:
        return PointerEvent::TertiaryButton;
    default:
        return PointerEvent::NoButtons;
    }
}

QUuid EntityTreeRenderer::mousePressEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return UNKNOWN_ENTITY_ID;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mousePressEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Press, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->mousePressOnEntity(rayPickResult.entityID, pointerEvent);

        _currentClickingOnEntityID = rayPickResult.entityID;
        emit entityScriptingInterface->clickDownOnEntity(_currentClickingOnEntityID, pointerEvent);

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;

        return rayPickResult.entityID;
    }
    emit entityScriptingInterface->mousePressOffEntity();
    return UNKNOWN_ENTITY_ID;
}

void EntityTreeRenderer::mouseDoublePressEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mouseDoublePressEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Press, PointerManager::MOUSE_POINTER_ID,
            pos2D, rayPickResult.intersection,
            rayPickResult.surfaceNormal, ray.direction,
            toPointerButton(*event), toPointerButtons(*event), Qt::NoModifier);

        emit entityScriptingInterface->mouseDoublePressOnEntity(rayPickResult.entityID, pointerEvent);

        _currentClickingOnEntityID = rayPickResult.entityID;
        emit entityScriptingInterface->clickDownOnEntity(_currentClickingOnEntityID, pointerEvent);

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;
    } else {
        emit entityScriptingInterface->mouseDoublePressOffEntity();
    }
}

void EntityTreeRenderer::mouseReleaseEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mouseReleaseEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        // qCDebug(entitiesrenderer) << "mouseReleaseEvent over entity:" << rayPickResult.entityID;

        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Release, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->mouseReleaseOnEntity(rayPickResult.entityID, pointerEvent);

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;
    }

    // Even if we're no longer intersecting with an entity, if we started clicking on it, and now
    // we're releasing the button, then this is considered a clickReleaseOn event
    if (!_currentClickingOnEntityID.isInvalidID()) {
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Release, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->clickReleaseOnEntity(_currentClickingOnEntityID, pointerEvent);
    }

    // makes it the unknown ID, we just released so we can't be clicking on anything
    _currentClickingOnEntityID = UNKNOWN_ENTITY_ID;
}

void EntityTreeRenderer::mouseMoveEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mouseMoveEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Move, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->mouseMoveOnEntity(rayPickResult.entityID, pointerEvent);

        // handle the hover logic...

        // if we were previously hovering over an entity, and this new entity is not the same as our previous entity
        // then we need to send the hover leave.
        if (!_currentHoverOverEntityID.isInvalidID() && rayPickResult.entityID != _currentHoverOverEntityID) {
            glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
            PointerEvent pointerEvent(PointerEvent::Move, PointerManager::MOUSE_POINTER_ID,
                                      pos2D, rayPickResult.intersection,
                                      rayPickResult.surfaceNormal, ray.direction,
                                      toPointerButton(*event), toPointerButtons(*event),
                                      Qt::NoModifier); // TODO -- check for modifier keys?

            emit entityScriptingInterface->hoverLeaveEntity(_currentHoverOverEntityID, pointerEvent);
        }

        // If the new hover entity does not match the previous hover entity then we are entering the new one
        // this is true if the _currentHoverOverEntityID is known or unknown
        if (rayPickResult.entityID != _currentHoverOverEntityID) {
            emit entityScriptingInterface->hoverEnterEntity(rayPickResult.entityID, pointerEvent);
        }

        // and finally, no matter what, if we're intersecting an entity then we're definitely hovering over it, and
        // we should send our hover over event
        emit entityScriptingInterface->hoverOverEntity(rayPickResult.entityID, pointerEvent);

        // remember what we're hovering over
        _currentHoverOverEntityID = rayPickResult.entityID;

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;

    } else {
        // handle the hover logic...
        // if we were previously hovering over an entity, and we're no longer hovering over any entity then we need to
        // send the hover leave for our previous entity
        if (!_currentHoverOverEntityID.isInvalidID()) {
            glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
            PointerEvent pointerEvent(PointerEvent::Move, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                      toPointerButton(*event), toPointerButtons(*event),
                                      Qt::NoModifier); // TODO -- check for modifier keys?

            emit entityScriptingInterface->hoverLeaveEntity(_currentHoverOverEntityID, pointerEvent);
            _currentHoverOverEntityID = UNKNOWN_ENTITY_ID; // makes it the unknown ID

            _lastPointerEvent = pointerEvent;
            _lastPointerEventValid = true;
        }
    }
}

void EntityTreeRenderer::deletingEntity(const EntityItemID& entityID) {
    // If it's in a pending queue, remove it
    _entitiesToAdd.erase(entityID);

    auto itr = _entitiesInScene.find(entityID);
    if (_entitiesInScene.end() == itr) {
        // Not in the scene, and no longer potentially in the pending queue, we're done
        return;
    }

    if (_tree && !_shuttingDown && _entitiesScriptEngine && !itr->second->getEntity()->getScript().isEmpty()) {
        if (_currentEntitiesInside.contains(entityID)) {
            _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
        }
        _entitiesScriptEngine->unloadEntityScript(entityID, true);
    }

    auto scene = _viewState->getMain3DScene();
    if (!scene) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::deletingEntity(), Unexpected null scene, possibly during application shutdown";
        return;
    }

    auto renderable = itr->second;
    _renderablesToUpdate.erase(renderable);
    _entitiesInScene.erase(itr);
    undeferModelMesh(entityID);

    if (!renderable) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::deletingEntity(), trying to remove non-renderable entity";
        return;
    }

    forceRecheckEntities(); // reset our state to force checking our inside/outsideness of entities

    fadeOutRenderable(renderable);
}

void EntityTreeRenderer::addingEntity(const EntityItemID& entityID) {
    checkAndCallPreload(entityID);
    auto entity = std::static_pointer_cast<EntityTree>(_tree)->findEntityByID(entityID);
    if (entity) {
        _entitiesToAdd.insert({ entity->getEntityItemID(),  entity });
    }
}

void EntityTreeRenderer::entityScriptChanging(const EntityItemID& entityID, bool reload) {
    checkAndCallPreload(entityID, reload, true);
    // Force "re-checking" entities so that the logic inside `checkEnterLeaveEntities()` is run.
    // This will ensure that the `enterEntity()` signal is emitted on clients whose avatars
    // are inside an entity when the script is reloaded.
    forceRecheckEntities();
}

void EntityTreeRenderer::checkAndCallPreload(const EntityItemID& entityID, bool reload, bool unloadFirst) {
    if (_tree && !_shuttingDown) {
        EntityItemPointer entity = getTree()->findEntityByEntityItemID(entityID);
        if (!entity) {
            return;
        }
        bool shouldLoad = entity->shouldPreloadScript() && _entitiesScriptEngine;
        // "" while the entity's scripts are off: unloaded, as when its script is cleared
        QString scriptUrl = entity->getScriptToRun();
        if ((shouldLoad && unloadFirst) || scriptUrl.isEmpty()) {
            if (_entitiesScriptEngine) {
                if (_currentEntitiesInside.contains(entityID)) {
                    _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
                }
                _entitiesScriptEngine->unloadEntityScript(entityID);
            }
            entity->scriptHasUnloaded();
        }
        if (shouldLoad) {
            entity->setScriptHasFinishedPreload(false);
            _entitiesScriptEngine->loadEntityScript(entityID, resolveScriptURL(scriptUrl), reload);
            entity->scriptHasPreloaded();
        }
    }
}

void EntityTreeRenderer::fadeOutRenderable(const EntityRendererPointer& renderable) {
    render::Transaction transaction;
    auto scene = _viewState->getMain3DScene();

    EntityRendererWeakPointer weakRenderable = renderable;
    transaction.setTransitionFinishedOperator(renderable->getRenderItemID(), [scene, weakRenderable]() {
        auto renderable = weakRenderable.lock();
        if (renderable) {
            render::Transaction transaction;
            renderable->removeFromScene(scene, transaction);
            scene->enqueueTransaction(transaction);
        }
    });

    scene->enqueueTransaction(transaction);
}

// TIVOLI improve entity collision sounds
// This could be done a million times better.  At least on the sound side.
void EntityTreeRenderer::playEntityCollisionSound(const EntityItemPointer& entity, const Collision& collision) {
    assert((bool)entity);
    auto renderable = renderableForEntity(entity);
    if (!renderable) {
        return;
    }

    SharedSoundPointer collisionSound = renderable->getCollisionSound(); // TIVOLI improve
    if (!collisionSound) {
        return;
    }
    bool success = false;
    AACube minAACube = entity->getMinimumAACube(success);
    if (!success) {
        return;
    }
    float mass = entity->computeMass();

    const float COLLISION_PENETRATION_TO_VELOCITY = 50.0f; // as a subsitute for RELATIVE entity->getVelocity()
    // The collision.penetration is a pretty good indicator of changed velocity AFTER the initial contact,
    // but that first contact depends on exactly where we hit in the physics step.
    // We can get a more consistent initial-contact energy reading by using the changed velocity.
    // Note that velocityChange is not a good indicator for continuing collisions, because it does not distinguish
    // between bounce and sliding along a surface.
    const float speedSquared = (collision.type == CONTACT_EVENT_TYPE_START) ?
        glm::length2(collision.velocityChange) :
        glm::length2(collision.penetration) * COLLISION_PENETRATION_TO_VELOCITY;
    const float energy = mass * speedSquared / 2.0f;
    const float COLLISION_ENERGY_AT_FULL_VOLUME = (collision.type == CONTACT_EVENT_TYPE_START) ? 150.0f : 5.0f;
    const float COLLISION_MINIMUM_VOLUME = 0.005f;
    const float energyFactorOfFull = fmin(1.0f, energy / COLLISION_ENERGY_AT_FULL_VOLUME);
    if (energyFactorOfFull < COLLISION_MINIMUM_VOLUME) {
        return;
    }
    // Quiet sound aren't really heard at all, so we can compress everything to the range [1-c, 1], if we play it all.
    const float COLLISION_SOUND_COMPRESSION_RANGE = 1.0f; // This section could be removed when the value is 1, but let's see how it goes.
    const float volume = (energyFactorOfFull * COLLISION_SOUND_COMPRESSION_RANGE) + (1.0f - COLLISION_SOUND_COMPRESSION_RANGE);

    // Shift the pitch down by ln(1 + (size / COLLISION_SIZE_FOR_STANDARD_PITCH)) / ln(2)
    const float COLLISION_SIZE_FOR_STANDARD_PITCH = 0.2f;
    const float stretchFactor = logf(1.0f + (minAACube.getLargestDimension() / COLLISION_SIZE_FOR_STANDARD_PITCH)) / logf(2.0f);

    AudioInjectorOptions options;
    options.stereo = collisionSound->isStereo();
    options.position = collision.contactPoint;
    options.volume = volume;
    options.pitch = 1.0f / stretchFactor;

    DependencyManager::get<AudioInjectorManager>()->playSound(collisionSound, options, true);
}

void EntityTreeRenderer::entityCollisionWithEntity(const EntityItemID& idA, const EntityItemID& idB,
                                                    const Collision& collision) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);
    const QUuid& myNodeID = DependencyManager::get<NodeList>()->getSessionUUID();

    // trigger scripted collision sounds and events for locally owned objects
    EntityItemPointer entityA = entityTree->findEntityByEntityItemID(idA);
    EntityItemPointer entityB = entityTree->findEntityByEntityItemID(idB);
    if ((bool)entityA && (bool)entityB) {
        QUuid entityASimulatorID = entityA->getSimulatorID();
        QUuid entityBSimulatorID = entityB->getSimulatorID();
        bool entityAIsDynamic = entityA->getDynamic();
        bool entityBIsDynamic = entityB->getDynamic();

#ifdef WANT_DEBUG
        bool bothEntitiesStatic = !entityAIsDynamic && !entityBIsDynamic;
        if (bothEntitiesStatic) {
            qCDebug(entities) << "A collision has occurred between two static entities!";
            qCDebug(entities) << "Entity A ID:" << entityA->getID();
            qCDebug(entities) << "Entity B ID:" << entityB->getID();
        }
        assert(!bothEntitiesStatic);
#endif

        if (
            (myNodeID == entityASimulatorID && entityAIsDynamic) || 
            (myNodeID == entityBSimulatorID && 
                (!entityAIsDynamic || entityASimulatorID.isNull())
            )
        ) {
            playEntityCollisionSound(entityA, collision);
            emit collisionWithEntity(idA, idB, collision);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(idA, "collisionWithEntity", idB, collision);
            }
        }

        if (
            (myNodeID == entityBSimulatorID && entityBIsDynamic) || 
            (myNodeID == entityASimulatorID && 
                (!entityBIsDynamic || entityBSimulatorID.isNull())
            )
        ) {
            playEntityCollisionSound(entityB, collision);
            // since we're swapping A and B we need to send the inverted collision
            Collision invertedCollision(collision);
            invertedCollision.invert();
            emit collisionWithEntity(idB, idA, invertedCollision);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(idB, "collisionWithEntity", idA, invertedCollision);
            }
        }
    }
}

void EntityTreeRenderer::updateEntityRenderStatus(bool shouldRenderEntities) {
    if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderEntities()) {
        for (auto entityID : _entityIDsLastInScene) {
            addingEntity(entityID);
        }
        _entityIDsLastInScene.clear();
    } else {
        _entityIDsLastInScene.clear();
        using MapEntry = std::pair<EntityItemID, EntityRendererPointer>;
        std::transform(
            _entitiesInScene.begin(), _entitiesInScene.end(), 
            std::back_inserter(_entityIDsLastInScene),
            [](const MapEntry& entry) {
                return entry.first;
            }
        );

        for (auto entityID : _entityIDsLastInScene) {
            // FIXME - is this really right? do we want to do the deletingEntity() code or just remove from the scene.
            deletingEntity(entityID);
        }
    }
}

void EntityTreeRenderer::updateZone(const EntityItemID& id) {
    if (auto zone = std::dynamic_pointer_cast<ZoneEntityItem>(getTree()->findEntityByEntityItemID(id))) {
        if (_layeredZones.update(zone, _avatarPosition, this)) {
            applyLayeredZones();
        }
        zoneCullingZoneChanged(id);
    }
}

bool EntityTreeRenderer::LayeredZones::clearDomainAndNonOwnedZones() {
    bool zonesChanged = false;

    auto it = begin();
    while (it != end()) {
        auto zone = it->zone.lock();
        if (!zone || !(zone->isLocalEntity() || zone->isMyAvatarEntity())) {
            zonesChanged = true;
            it = erase(it);
        } else {
            it++;
        }
    }

    if (zonesChanged) {
        sort();
    }
    return zonesChanged;
}

std::pair<bool, bool> EntityTreeRenderer::LayeredZones::getZoneInteractionProperties() const {
    for (auto it = cbegin(); it != cend(); it++) {
        auto zone = it->zone.lock();
        if (zone && zone->isDomainEntity()) {
            return { zone->getFlyingAllowed(), zone->getGhostingAllowed() };
        }
    }
    return { true, true };
}

bool EntityTreeRenderer::LayeredZones::update(std::shared_ptr<ZoneEntityItem> zone, const glm::vec3& position, EntityTreeRenderer* entityTreeRenderer) {
    // When a zone's position or visibility changes, we call this method
    // In order to resort our zones, we first remove the changed zone, and then re-insert it if necessary

    bool needsResort = false;

    {
        auto it = begin();
        while (it != end()) {
            if (it->zone.lock() == zone) {
                break;
            }
            it++;
        }
        if (it != end()) {
            erase(it);
            needsResort = true;
        }
    }

    // Only call contains if the zone is rendering
    if (
        zone->isVisible() &&
        entityTreeRenderer->renderableIdForEntity(zone) != render::Item::INVALID_ITEM_ID && 
        zone->contains(position)
    ) {
        emplace_back(zone);
        needsResort = true;
    }

    if (needsResort) {
        sort();
    }

    return needsResort;
}

bool EntityTreeRenderer::LayeredZones::equals(const LayeredZones& other) const {
    if (size() != other.size()) {
        return false;
    }

    auto it = cbegin();
    auto otherIt = other.cbegin();
    while (it != cend()) {
        if (*it != *otherIt) {
            return false;
        }
        it++;
        otherIt++;
    }

    return true;
}

void EntityTreeRenderer::LayeredZones::appendRenderIDs(render::ItemIDs& list, EntityTreeRenderer* entityTreeRenderer) const {
    for (auto it = cbegin(); it != cend(); it++) {
        if (it->zone.lock()) {
            auto id = entityTreeRenderer->renderableIdForEntityId(it->id);
            if (id != render::Item::INVALID_ITEM_ID) {
                list.push_back(id);
            }
        }
    }
}

CalculateEntityLoadingPriority EntityTreeRenderer::_calculateEntityLoadingPriorityFunc = [](const EntityItem& item) -> float {
    return 0.0f;
};

std::pair<bool, bool> EntityTreeRenderer::getZoneInteractionProperties() {
    return _layeredZones.getZoneInteractionProperties();
}

bool EntityTreeRenderer::wantsKeyboardFocus(const EntityItemID& id) const {
    auto renderable = renderableForEntityId(id);
    if (!renderable) {
        return false;
    }
    return renderable->wantsKeyboardFocus();
}

QObject* EntityTreeRenderer::getEventHandler(const EntityItemID& id) {
    auto renderable = renderableForEntityId(id);
    if (!renderable) {
        return nullptr;
    }
    return renderable->getEventHandler();
}

bool EntityTreeRenderer::wantsHandControllerPointerEvents(const EntityItemID& id) const {
    auto renderable = renderableForEntityId(id);
    if (!renderable) {
        return false;
    }
    return renderable->wantsHandControllerPointerEvents();
}

void EntityTreeRenderer::setProxyWindow(const EntityItemID& id, QWindow* proxyWindow) {
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        renderable->setProxyWindow(proxyWindow);
    }
}

void EntityTreeRenderer::setCollisionSound(const EntityItemID& id, const SharedSoundPointer& sound) {
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        renderable->setCollisionSound(sound);
    }
}
EntityItemPointer EntityTreeRenderer::getEntity(const EntityItemID& id) {
    EntityItemPointer result;
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        result = renderable->getEntity();
    }
    return result;
}

void EntityTreeRenderer::deleteEntity(const EntityItemID& id) const {
    DependencyManager::get<EntityScriptingInterface>()->deleteEntity(id);
}

void EntityTreeRenderer::onEntityChanged(const EntityItemID& id) {
    _changedEntitiesGuard.withWriteLock([&] {
        _changedEntities.insert(id);
    });
}

EntityEditPacketSender* EntityTreeRenderer::getPacketSender() {
    EntityTreePointer tree = getTree();
    EntitySimulationPointer simulation = tree ? tree->getSimulation() : nullptr;
    PhysicalEntitySimulationPointer peSimulation = std::static_pointer_cast<PhysicalEntitySimulation>(simulation);
    EntityEditPacketSender* packetSender = peSimulation ? peSimulation->getPacketSender() : nullptr;
    return packetSender;
}

std::function<bool(const QUuid&, graphics::MaterialLayer, const std::string&)> EntityTreeRenderer::_addMaterialToEntityOperator = nullptr;
std::function<bool(const QUuid&, graphics::MaterialPointer, const std::string&)> EntityTreeRenderer::_removeMaterialFromEntityOperator = nullptr;
std::function<bool(const QUuid&, graphics::MaterialLayer, const std::string&)> EntityTreeRenderer::_addMaterialToAvatarOperator = nullptr;
std::function<bool(const QUuid&, graphics::MaterialPointer, const std::string&)> EntityTreeRenderer::_removeMaterialFromAvatarOperator = nullptr;

bool EntityTreeRenderer::addMaterialToEntity(const QUuid& entityID, graphics::MaterialLayer material, const std::string& parentMaterialName) {
    if (_addMaterialToEntityOperator) {
        return _addMaterialToEntityOperator(entityID, material, parentMaterialName);
    }
    return false;
}

bool EntityTreeRenderer::removeMaterialFromEntity(const QUuid& entityID, graphics::MaterialPointer material, const std::string& parentMaterialName) {
    if (_removeMaterialFromEntityOperator) {
        return _removeMaterialFromEntityOperator(entityID, material, parentMaterialName);
    }
    return false;
}

bool EntityTreeRenderer::addMaterialToAvatar(const QUuid& avatarID, graphics::MaterialLayer material, const std::string& parentMaterialName) {
    if (_addMaterialToAvatarOperator) {
        return _addMaterialToAvatarOperator(avatarID, material, parentMaterialName);
    }
    return false;
}

bool EntityTreeRenderer::removeMaterialFromAvatar(const QUuid& avatarID, graphics::MaterialPointer material, const std::string& parentMaterialName) {
    if (_removeMaterialFromAvatarOperator) {
        return _removeMaterialFromAvatarOperator(avatarID, material, parentMaterialName);
    }
    return false;
}
