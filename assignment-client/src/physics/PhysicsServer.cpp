//
//  PhysicsServer.cpp
//  assignment-client/src/physics
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: these functions, the physics server's train: run, step, publishTrainStates, handleOctreePacket (its
//   query: tracks, vehicles, collidable things, Terrain), measureStreamAge, handleVehicleInput, each with everything
//   it calls on the vehicle's path.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "PhysicsServer.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include <EntityItemProperties.h>
#include <EntityNodeData.h>
#include <EntityScriptingInterface.h>
#include <EntityTree.h>
#include <PhysicsHelpers.h>
#include <PickFilter.h>
#include <PhysicsLogging.h>
#include <ResourceManager.h>
#include <TrackEntityItem.h>
#include <TrackSimulation.h>
#include <VehicleInput.h>
#include <VehicleSimulation.h>

#include "../AssignmentDynamicFactory.h"
#include "../entities/AssignmentParentFinder.h"
#include "ServerTerrainEntityItem.h"
#include "ServerTrackEntityItem.h"

namespace {
    const QString PHYSICS_SERVER_LOGGING_NAME = "physics-server";
    const int STEP_INTERVAL_MSECS = 11;   // a little faster than the 90 Hz substep; the engine's paced clock does the rest
    const quint64 QUERY_INTERVAL = USECS_PER_SECOND;   // the entity server is asked again this often
    const quint64 TERRAIN_INTERVAL = USECS_PER_SECOND; // the terrains are checked for a new file this often
    const int EDIT_PACKETS_PER_SECOND = 300;
    // Jolt's workers on the server (PHYSICS.md round 9, the idle cost): a few, not the machine's cores less one
    const int SERVER_WORKERS = 4;
    // TIVOLI_PHYSICS_NO_IDLE=1 keeps the old behaviour (every worker, every step) for a before measurement
    bool idleSkipAllowed() {
        static const bool allowed = qgetenv("TIVOLI_PHYSICS_NO_IDLE").isEmpty();
        return allowed;
    }
}

PhysicsServer::PhysicsServer(ReceivedMessage& message) : ThreadedAssignment(message) {
    DependencyManager::registerInheritance<EntityDynamicFactoryInterface, AssignmentDynamicFactory>();
    DependencyManager::set<AssignmentDynamicFactory>();
    DependencyManager::registerInheritance<SpatialParentFinder, AssignmentParentFinder>();
    DependencyManager::set<EntityScriptingInterface>(false)->setPacketSender(&_entityEditSender);
    DependencyManager::set<ResourceManager>();

    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListenerForTypes({ PacketType::OctreeStats, PacketType::EntityData, PacketType::EntityErase,
                                              PacketType::EntityQueryInitialResultsComplete },
                                            this, "handleOctreePacket");
    packetReceiver.registerListener(PacketType::VehicleInput, this, "handleVehicleInput");
}

PhysicsServer::~PhysicsServer() {
}

// FROZEN (vehicle path): see the note at the top of this file.
void PhysicsServer::run() {
    ThreadedAssignment::commonInit(PHYSICS_SERVER_LOGGING_NAME, NodeType::PhysicsServer);

    auto nodeList = DependencyManager::get<NodeList>();
    // the entity server for the tree; the clients (agents) so that the domain introduces them and the hole punch
    // completes: a driver's VehicleInput packets come straight from its client (round 6); the asset server for the
    // world's own files, such as a terrain at an atp: URL (without it every atp: fetch fails "Server Unavailable")
    nodeList->addSetOfNodeTypesToNodeInterestSet({ NodeType::EntityServer, NodeType::Agent, NodeType::AssetServer });
    connect(nodeList.data(), &LimitedNodeList::nodeActivated, this, &PhysicsServer::nodeActivated);
    connect(nodeList.data(), &LimitedNodeList::nodeKilled, this, &PhysicsServer::nodeKilled);
    connect(nodeList.data(), &LimitedNodeList::uuidChanged, this, [](const QUuid& sessionUUID, const QUuid&) {
        Physics::setSessionUUID(sessionUUID);
    });
    Physics::setSessionUUID(nodeList->getSessionUUID());
    Physics::setServerAuthority(true);

    _entityEditSender.setPacketsPerSecond(EDIT_PACKETS_PER_SECOND);

    // this server's Terrain entities hold the ground's collision mesh (round 6); the plain one is only its properties.
    // Its Track entities hold their guideway's (the beam and pylons) the same way.
    EntityTypes::registerEntityType(EntityTypes::Terrain, "Terrain", ServerTerrainEntityItem::factory);
    EntityTypes::registerEntityType(EntityTypes::Track, "Track", ServerTrackEntityItem::factory);

    _entityViewer.init();
    EntityTreePointer tree = _entityViewer.getTree();

    // what this server sees of the domain: every Track and everything that rides one, every vehicle (round 6) and the
    // ground (Terrain entities and anything collidable with a primitive shape); nothing that needs a model loaded, so
    // a big world costs it little. Not their ancestors: the entity server rebuilds a filtered query's ancestor set on
    // every pass and takes every ancestor for a new one, which forces a full re-send of the view each pass (with a few
    // hundred matching entities, ten seconds of lag on everything, round 6 found). A car or a vehicle with a parent
    // entity is therefore placed by its own transform here.
    QJsonObject queryJSONParameters;
    queryJSONParameters[EntityJSONQueryProperties::TRACK_ID_PROPERTY] = EntityQueryFilterSymbol::NonDefault;
    queryJSONParameters[EntityJSONQueryProperties::VEHICLE_PROPERTY] = EntityQueryFilterSymbol::NonDefault;
    queryJSONParameters[EntityJSONQueryProperties::COLLIDABLE_PROPERTY] = EntityQueryFilterSymbol::NonDefault;
    QJsonArray types;
    types.append(EntityTypes::getEntityTypeName(EntityTypes::Track));
    types.append(EntityTypes::getEntityTypeName(EntityTypes::Terrain));
    queryJSONParameters[EntityJSONQueryProperties::ENTITY_TYPE_PROPERTY] = types;
    QJsonObject queryFlags;
    queryFlags[EntityJSONQueryProperties::INCLUDE_ANCESTORS_PROPERTY] = false;
    queryFlags[EntityJSONQueryProperties::INCLUDE_DESCENDANTS_PROPERTY] = false;
    queryJSONParameters[EntityJSONQueryProperties::FLAGS_PROPERTY] = queryFlags;
    _entityViewer.getOctreeQuery().setJSONParameters(queryJSONParameters);

    DependencyManager::set<AssignmentParentFinder>(tree);
    DependencyManager::get<EntityScriptingInterface>()->setEntityTree(tree);

    // the physics: the same engine and simulation a client runs, with no character and no workload regions
    ObjectMotionState::setShapeManager(&_shapeManager);
    ObjectMotionState::setInterpolatePresentation(false);   // nothing is drawn here: the records are the last substep itself
    _physicsEngine = std::make_shared<PhysicsEngine>(Vectors::ZERO);
    _physicsEngine->init(idleSkipAllowed() ? SERVER_WORKERS : 0);
    _entitySimulation = std::make_shared<PhysicalEntitySimulation>();
    _entitySimulation->init(tree, _physicsEngine, &_entityEditSender);
    tree->setSimulation(_entitySimulation);

    _stepTimer = new QTimer(this);
    _stepTimer->setTimerType(Qt::PreciseTimer);
    connect(_stepTimer, &QTimer::timeout, this, &PhysicsServer::step);
    _stepTimer->start(STEP_INTERVAL_MSECS);
    _running = true;
    _lastStatsTime = usecTimestampNow();
    _lastLogTime = _lastStatsTime;

    qCInfo(physics) << "physics-server running: the trains on the tracks are driven from here (PHYSICS.md round 5)";
}

// FROZEN (vehicle path): see the note at the top of this file.
void PhysicsServer::step() {
    if (!_running) {
        return;
    }
    EntityTreePointer tree = _entityViewer.getTree();
    if (!tree) {
        return;
    }

    quint64 now = usecTimestampNow();
    if (now - _lastQueryTime > QUERY_INTERVAL) {
        _entityViewer.queryOctree();
        _lastQueryTime = now;
    }
    if (now - _lastTerrainTime > TERRAIN_INTERVAL) {
        updateTerrains();
        _lastTerrainTime = now;
    }

    // the same order as the client's update: the tree's bookkeeping, the physics transaction, the step, the results
    _entityViewer.update();
    _entitySimulation->removeDeadEntities();
    {
        PhysicsEngine::Transaction transaction;
        _entitySimulation->buildPhysicsTransaction(transaction);
        _physicsEngine->processTransaction(transaction);
        _entitySimulation->handleProcessedPhysicsTransaction(transaction);
    }
    _entitySimulation->applyDynamicChanges();
    _physicsEngine->forEachDynamic([&](EntityDynamicPointer dynamic) {
        dynamic->prepareForPhysicsSimulation();
    });
    // with no train, no vehicle and nothing awake, the step is skipped (round 9): a world of resting things costs nothing
    _physicsEngine->setIdleWhenStill(idleSkipAllowed() && _entitySimulation->getTrackSimulation()->getNumTrains() == 0
                                     && _entitySimulation->getVehicleSimulation()->getNumVehicles() == 0);
    tree->withWriteLock([&] {
        _physicsEngine->stepSimulation();
    });
    if (_physicsEngine->hasOutgoingChanges()) {
        auto& collisionEvents = _physicsEngine->getCollisionEvents();
        tree->withWriteLock([&] {
            const VectorOfMotionStates& outgoingChanges = _physicsEngine->getChangedMotionStates();
            _entitySimulation->handleChangedMotionStates(outgoingChanges);
            const VectorOfMotionStates& deactivations = _physicsEngine->getDeactivatedMotionStates();
            _entitySimulation->handleDeactivatedMotionStates(deactivations);
        });
        _entitySimulation->handleCollisionEvents(collisionEvents);
        _physicsEngine->updateContactMap();
        _physicsEngine->doOwnershipInfectionForConstraints();
    }
    _stepsTaken++;

    publishTrainStates();

    // a line in the log every ten seconds: the loop's rate, what it holds, what it has sent
    const quint64 LOG_INTERVAL = 10 * USECS_PER_SECOND;
    if (now - _lastLogTime > LOG_INTERVAL) {
        float seconds = (float)(now - _lastLogTime) / (float)USECS_PER_SECOND;
        quint64 steps = _stepsTaken - _stepsAtLastLog;
        qCInfo(physics) << "physics-server:" << (seconds > 0.0f ? (float)steps / seconds : 0.0f) << "steps/s,"
            << _physicsEngine->getNumSubsteps() << "substeps," << _physicsEngine->getNumCollisionObjects() << "bodies,"
            << _physicsEngine->getNumFrames() << "frames," << tree->getOctreeElementsCount() << "tree elements,"
            << _statesPublished << "train states published," << _entityEditSender.getPacketsPerSecond() << "pps;"
            << _entitySimulation->getVehicleSimulation()->getNumVehicles() << "vehicles,"
            << _entitySimulation->getVehicleSimulation()->getStats().inputsReceived << "inputs received,"
            << _entitySimulation->getVehicleSimulation()->getStats().inputsStarved << "held,"
            << _entitySimulation->getVehicleSimulation()->getStats().inputsLate << "late,"
            << _entitySimulation->getVehicleSimulation()->getStats().inputsEarly << "early,"
            << _entitySimulation->getVehicleSimulation()->getStats().bufferRealigned << "realigned, margin"
            << _entitySimulation->getVehicleSimulation()->getStats().bufferMargin << ";"
            << "stream" << _streamPackets << "packets, flight max" << _streamFlightMax / 1000 << "ms, queued max"
            << _streamQueuedMax / 1000 << "ms, parse max" << _parseMaxUsec / 1000 << "ms, total" << _parseTotalUsec / 1000
            << "ms (lock wait" << _entityViewer.getAverageWaitLockPerPacket() / 1000.0f << "ms, uncompress"
            << _entityViewer.getAverageUncompressPerPacket() / 1000.0f << "ms, read" << _entityViewer.getAverageReadBitstreamPerPacket() / 1000.0f
            << "ms per packet;" << describeEntityServerConnection() << ");" << _physicsEngine->getNumIdleSteps() << "idle substeps skipped ("
            << _physicsEngine->describeActiveBodies(3) << "),"
            << _entitySimulation->getVehicleSimulation()->getStats().streamStatesSent << "states streamed to"
            << _entitySimulation->getVehicleSimulation()->getStats().streamSubscribers << "subscribers";
        _lastLogTime = now;
        _stepsAtLastLog = _stepsTaken;
        _streamPackets = 0;
        _streamFlightMax = 0;
        _streamQueuedMax = 0;
        _parseMaxUsec = 0;
        _parseTotalUsec = 0;
    }

    // the bids, the cars' updates and the train states go out from here: an assignment client's edit sender is
    // released and pumped by its script engine every frame, and this one has none. Without the release a lone edit (a
    // train state on its own) waits in a half-filled packet until a burst fills it.
    if (_entityEditSender.serversExist()) {
        _entityEditSender.releaseQueuedMessages();
        _entityEditSender.process();
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
// The trains' states go out as ordinary edits of their Track entities, stamped with the domain time of the substep
// they describe; the entity server relays them to every client, which runs the follower on from there.
void PhysicsServer::publishTrainStates() {
    std::vector<TrackSimulation::Publication> publications;
    _entitySimulation->getTrackSimulation()->takePublications(publications);
    if (publications.empty()) {
        return;
    }
    EntityTreePointer tree = _entityViewer.getTree();
    quint64 now = usecTimestampNow();
    for (const auto& publication : publications) {
        EntityItemProperties properties;
        properties.setType(EntityTypes::Track);
        properties.setTrainDistance(publication.state.s);
        properties.setTrainSpeed(publication.state.speed);
        properties.setTrainReversed(publication.state.direction < 0);
        properties.setTrainDwell(publication.state.dwellRemaining);
        properties.setTrainStation(publication.state.station < 0 ? TrackEntityItem::NO_STATION : (quint16)publication.state.station);
        properties.setTrainTime(publication.domainTime);
        properties.setLastEdited(now);
        _entityEditSender.queueEditEntityMessage(PacketType::EntityEdit, tree, publication.trackID, properties);
        if (_statesPublished == 0) {
            qCInfo(physics) << "physics-server: first train state published for track" << publication.trackID << "at" << publication.state.s
                << "m, domain time" << publication.domainTime;
        }
        _statesPublished++;
    }
}

// The Terrain entities fetch their files and build their meshes from here, once a second; the Track entities build
// their guideways' the same way.
void PhysicsServer::updateTerrains() {
    EntityTreePointer tree = _entityViewer.getTree();
    if (!tree) {
        return;
    }
    QVector<QUuid> terrainIDs;
    QVector<QUuid> trackIDs;
    unsigned int searchFilter = PickFilter::getBitMask(PickFilter::FlagBit::DOMAIN_ENTITIES);
    std::vector<std::shared_ptr<ServerTerrainEntityItem>> terrains;
    std::vector<std::shared_ptr<ServerTrackEntityItem>> tracks;
    tree->withReadLock([&] {
        tree->evalEntitiesInSphereWithType(Vectors::ZERO, (float)TREE_SCALE, EntityTypes::Terrain, PickFilter(searchFilter), terrainIDs);
        for (const QUuid& id : terrainIDs) {
            if (auto terrain = std::dynamic_pointer_cast<ServerTerrainEntityItem>(tree->findEntityByID(id))) {
                terrains.push_back(terrain);
            }
        }
        tree->evalEntitiesInSphereWithType(Vectors::ZERO, (float)TREE_SCALE, EntityTypes::Track, PickFilter(searchFilter), trackIDs);
        for (const QUuid& id : trackIDs) {
            if (auto track = std::dynamic_pointer_cast<ServerTrackEntityItem>(tree->findEntityByID(id))) {
                tracks.push_back(track);
            }
        }
    });
    for (auto& terrain : terrains) {
        terrain->update();
    }
    for (auto& track : tracks) {
        track->update();
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
// A driver's inputs for a vehicle, to apply one per substep; only the one in the driver's seat is heard.
void PhysicsServer::handleVehicleInput(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    if (!senderNode || !_entitySimulation) {
        return;
    }
    QUuid vehicleID;
    std::vector<VehicleInput> inputs;
    if (!VehicleInputPacket::unpack(message->getMessage(), vehicleID, inputs)) {
        qCWarning(physics) << "physics-server: a vehicle input packet from" << senderNode->getUUID() << "could not be read";
        return;
    }
    _entitySimulation->getVehicleSimulation()->receiveInputs(vehicleID, senderNode->getUUID(), inputs);
}

void PhysicsServer::nodeActivated(SharedNodePointer activatedNode) {
    if (activatedNode->getType() == NodeType::EntityServer) {
        _lastQueryTime = 0;   // ask the new entity server at once
    }
}

void PhysicsServer::nodeKilled(SharedNodePointer killedNode) {
    if (killedNode->getType() == NodeType::EntityServer) {
        bool hasAnotherEntityServer = false;
        auto nodeList = DependencyManager::get<NodeList>();
        nodeList->eachNodeBreakable([&hasAnotherEntityServer, &killedNode](const SharedNodePointer& node) {
            if (node->getType() == NodeType::EntityServer && node->getUUID() != killedNode->getUUID()) {
                hasAnotherEntityServer = true;
                return false;
            }
            return true;
        });
        if (!hasAnotherEntityServer) {
            // the world is gone until an entity server comes back; the trains start again from their cars then
            _entityViewer.clear();
        }
    }
}

void PhysicsServer::sendStatsPacket() {
    QJsonObject statsObject;

    quint64 now = usecTimestampNow();
    float seconds = (float)(now - _lastStatsTime) / (float)USECS_PER_SECOND;
    if (seconds > 0.0f) {
        _stepsPerSecond = (float)(_stepsTaken - _stepsAtLastStats) / seconds;
    }
    _lastStatsTime = now;
    _stepsAtLastStats = _stepsTaken;

    QJsonObject physicsStats;
    physicsStats["steps_per_second"] = _stepsPerSecond;
    physicsStats["substeps"] = (double)(_physicsEngine ? _physicsEngine->getNumSubsteps() : 0);
    physicsStats["bodies"] = _physicsEngine ? _physicsEngine->getNumCollisionObjects() : 0;
    physicsStats["frames"] = (double)(_physicsEngine ? _physicsEngine->getNumFrames() : 0);
    physicsStats["train_states_published"] = (double)_statesPublished;
    if (_entitySimulation) {
        const VehicleSimulation::Stats& vehicleStats = _entitySimulation->getVehicleSimulation()->getStats();
        QJsonObject vehicles;
        vehicles["count"] = (double)_entitySimulation->getVehicleSimulation()->getNumVehicles();
        vehicles["inputs_received"] = vehicleStats.inputsReceived;
        vehicles["inputs_starved"] = vehicleStats.inputsStarved;
        vehicles["inputs_dropped"] = vehicleStats.inputsDropped;
        physicsStats["vehicles"] = vehicles;
    }

    QJsonArray trains;
    EntityTreePointer tree = _entityViewer.getTree();
    if (tree) {
        QJsonObject octreeStats;
        octreeStats["elementCount"] = (double)OctreeElement::getNodeCount();
        statsObject["octree_stats"] = octreeStats;
        QVector<QUuid> trackIDs;
        unsigned int searchFilter = PickFilter::getBitMask(PickFilter::FlagBit::DOMAIN_ENTITIES) |
                                    PickFilter::getBitMask(PickFilter::FlagBit::AVATAR_ENTITIES);
        tree->withReadLock([&] {
            tree->evalEntitiesInSphereWithType(Vectors::ZERO, (float)TREE_SCALE, EntityTypes::Track, PickFilter(searchFilter), trackIDs);
            for (const QUuid& trackID : trackIDs) {
                auto track = std::dynamic_pointer_cast<TrackEntityItem>(tree->findEntityByID(trackID));
                if (!track) {
                    continue;
                }
                QJsonObject train;
                train["track"] = uuidStringWithoutCurlyBraces(track->getID());
                train["name"] = track->getName();
                train["distance_m"] = track->getTrainDistance();
                train["speed_m_s"] = track->getTrainSpeed();
                train["reversed"] = track->getTrainReversed();
                train["dwell_s"] = track->getTrainDwell();
                train["station"] = track->getTrainStation() == TrackEntityItem::NO_STATION ? -1 : (int)track->getTrainStation();
                train["time"] = (double)track->getTrainTime();
                trains.append(train);
            }
        });
    }
    physicsStats["trains"] = trains;
    statsObject["physics_stats"] = physicsStats;

    addPacketStatsAndSendStatsPacket(statsObject);
}

// FROZEN (vehicle path): see the note at the top of this file.
void PhysicsServer::handleOctreePacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    // World data is taken only from the world's own entity server: the node list's HMAC check names the sending
    // node, not its type. A piggybacked packet inherits its carrier's sender. (Vehicle master plan entry 15.)
    if (!senderNode || senderNode->getType() != NodeType::EntityServer) {
        return;
    }
    auto packetType = message->getType();
    qint64 receiveTime = message->getFirstPacketReceiveTime();

    if (packetType == PacketType::OctreeStats) {
        int statsMessageLength = OctreeHeadlessViewer::parseOctreeStats(message, senderNode);
        if (statsMessageLength >= 0 && message->getSize() > statsMessageLength) {
            // pull out the piggybacked packet and create a new QSharedPointer<NLPacket> for it
            int piggyBackedSizeWithHeader = message->getSize() - statsMessageLength;
            auto buffer = std::unique_ptr<char[]>(new char[piggyBackedSizeWithHeader]);
            memcpy(buffer.get(), message->getRawMessage() + statsMessageLength, piggyBackedSizeWithHeader);
            auto newPacket = NLPacket::fromReceivedPacket(std::move(buffer), piggyBackedSizeWithHeader, message->getSenderSockAddr());
            if (!newPacket) {
                return;   // too short for a packet's header: nothing to read
            }
            message = QSharedPointer<ReceivedMessage>::create(*newPacket);
        } else {
            return;   // no piggyback data
        }
        packetType = message->getType();
    }

    if (packetType == PacketType::EntityData) {
        measureStreamAge(*message, senderNode, receiveTime);
        quint64 parseStart = usecTimestampNow();
        _entityViewer.processDatagram(*message, senderNode);
        qint64 parse = (qint64)(usecTimestampNow() - parseStart);
        _parseMaxUsec = std::max(_parseMaxUsec, parse);
        _parseTotalUsec += parse;
    } else if (packetType == PacketType::EntityErase) {
        _entityViewer.processEraseMessage(*message, senderNode);
    } else if (packetType == PacketType::EntityQueryInitialResultsComplete) {
        _entityViewer.handleInitialResultsComplete(message, senderNode);
    }
}

// The udt connection to the entity server as this side sees it: what came in reliably and unreliably over the window,
// the ACKs this side sent, the round trip udt measured (sampling resets the node list's own window; a diagnostic).
QString PhysicsServer::describeEntityServerConnection() {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer entityServer = nodeList ? nodeList->soloNodeOfType(NodeType::EntityServer) : nullptr;
    if (!entityServer || !entityServer->getActiveSocket()) {
        return "no entity server";
    }
    for (const auto& entry : nodeList->sampleStatsForAllConnections()) {
        if (entry.first == *entityServer->getActiveSocket()) {
            const udt::ConnectionStats::Stats& s = entry.second;
            return QString("udt from the entity server: %1 reliable and %2 unreliable packets in, %3 ACKs sent, rtt %4 ms")
                .arg(s.receivedPackets).arg(s.receivedUnreliablePackets).arg(s.events[udt::ConnectionStats::Stats::SentACK]).arg(s.rtt / 1000);
        }
    }
    return "udt: no connection sampled";
}

// FROZEN (vehicle path): see the note at the top of this file.
// How late the entity server's stream is when this server reads it: the flight by the packet's stamp (the entity
// server's clock, skew-corrected) and the wait in this process's queue by the socket's receive time. A packet a second
// late is logged at once (one line a second); the ten-second line carries the window's maxima.
void PhysicsServer::measureStreamAge(ReceivedMessage& message, const SharedNodePointer& senderNode, qint64 receiveTime) {
    qint64 position = message.getPosition();
    OCTREE_PACKET_FLAGS flags;
    OCTREE_PACKET_SEQUENCE sequence;
    OCTREE_PACKET_SENT_TIME sentAt;
    message.readPrimitive(&flags);
    message.readPrimitive(&sequence);
    message.readPrimitive(&sentAt);
    message.seek(position);
    quint64 now = usecTimestampNow();
    qint64 clockSkew = senderNode ? senderNode->getClockSkewUsec() : 0;
    qint64 flight = (qint64)now - (qint64)sentAt + clockSkew;
    // (a message built from a packet list has no receive time; then the wait is unknown and counts as none)
    qint64 queued = 0;
    if (receiveTime > 0) {
        qint64 nowReceiveClock = std::chrono::duration_cast<std::chrono::microseconds>(
            p_high_resolution_clock::now().time_since_epoch()).count();
        queued = nowReceiveClock - receiveTime;
    }
    _streamPackets++;
    _streamFlightMax = std::max(_streamFlightMax, flight);
    _streamQueuedMax = std::max(_streamQueuedMax, queued);
    const qint64 LATE = USECS_PER_SECOND;
    if ((flight > LATE || queued > LATE) && now - _streamLateLogTime > USECS_PER_SECOND) {
        _streamLateLogTime = now;
        qCDebug(physics) << "physics-server: entity data late:" << flight / 1000 << "ms in flight," << queued / 1000
            << "ms queued here, sequence" << sequence;
    }
}

void PhysicsServer::aboutToFinish() {
    _running = false;
    if (_stepTimer) {
        _stepTimer->stop();
    }
    if (_entitySimulation) {
        _entitySimulation->clearEntities();
    }
    _entityViewer.clear();
    DependencyManager::get<EntityScriptingInterface>()->setEntityTree(nullptr);
    DependencyManager::get<ResourceManager>()->cleanup();
    DependencyManager::destroy<EntityScriptingInterface>();
    DependencyManager::destroy<ResourceManager>();
    DependencyManager::destroy<AssignmentDynamicFactory>();
    DependencyManager::destroy<AssignmentParentFinder>();
    Physics::setServerAuthority(false);
}
