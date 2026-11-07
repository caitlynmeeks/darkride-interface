//
//  DomainSession.cpp
//  interface/src
//
//  The client's session with a domain (see DomainSession.h). The code of the members of ApplicationDomain.cpp, of the
//  frame loop's physics gate, query decision and NACK step, and of the start-up's connection handlers, with
//  Application reached through qApp for the entities, the view, the window title, the display plug-in, the physics
//  engine, the edit packet sender, the settings' state and the first-run setting.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "DomainSession.h"

#include <chrono>
#include <thread>
#include <glm/glm.hpp>
#include <QtCore/QCommandLineParser>
#include <QProcessEnvironment>
#include <AccountManager.h>
#include <AddressManager.h>
#include <ContentAddresses.h>
#include <AssetClient.h>
#include <EntityNodeData.h>
#include <OctreeSceneStats.h>
#include <OffscreenUi.h>
#include <PhysicsHelpers.h>
#include <plugins/PluginManager.h>
#include <plugins/PluginUtils.h>
#include <render/EngineStats.h>
#include <ResourceRequest.h>
#include <SceneScriptingInterface.h>
#include <ScriptEngines.h>
#include <ScriptCache.h>
#include <SharedUtil.h>
#include <SoundCacheScriptingInterface.h>
#include <recording/ClipCache.h>
#include <display-plugins/hmd/HmdDisplayPlugin.h>

#include "Application.h"
#include "AudioClient.h"
#include "avatar/AvatarManager.h"
#include "LocationBookmarks.h"
#include "LODManager.h"
#include "ui/Stats.h"
#include "ui/Keyboard.h"

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();   // Application.cpp's
QString worldNameForPopup();                     // ApplicationDialogs.cpp's: the world's name for a popup's text

// the entity server's two time-outs (the domain connected; the server's node added) and the first query's radius
static const int ENTITY_SERVER_ADDED_TIMEOUT = 5000;
static const int ENTITY_SERVER_CONNECTION_TIMEOUT = 5000;
static const float INITIAL_QUERY_RADIUS = 10.0f;  // priority radius for entities before physics enabled
// the start-up's own arrival wait ends this long after the connect if no script takes it over: inside the governor's
// 10 s arrival grace (counted from the connect signal), with 2 s left for physics coming on (physics' ruling, 11-07:
// this clock starts a frame or more after that signal and is read once a frame)
static const int LAUNCH_HOLD_LIMIT_SECONDS = 8;

// Application's getMyAvatar is private; this is the same call
static std::shared_ptr<MyAvatar> getMyAvatar() {
    return DependencyManager::get<AvatarManager>()->getMyAvatar();
}

DomainSession::DomainSession() : _maxOctreePPS(_maxOctreePPSSetting.get()), _lastNackTime(usecTimestampNow()) {
    _entityServerConnectionTimer.setSingleShot(true);
    connect(&_entityServerConnectionTimer, &QTimer::timeout, this, &DomainSession::setFailedToConnectToEntityServer);
    connect(&_octreeProcessor, &OctreePacketProcessor::packetVersionMismatch, this, &DomainSession::notifyPacketVersionMismatch);
}

void DomainSession::shutdown() {
    _octreeProcessor.terminate();
}

void DomainSession::setFailedToConnectToEntityServer() {
    _failedToConnectToEntityServer = true;
}

void DomainSession::domainConnectionRefused(const QString& reasonMessage, int reasonCodeInt, const QString& extraInfo) {
    DomainHandler::ConnectionRefusedReason reasonCode = static_cast<DomainHandler::ConnectionRefusedReason>(reasonCodeInt);

    if (reasonCode == DomainHandler::ConnectionRefusedReason::TooManyUsers && !extraInfo.isEmpty()) {
        DependencyManager::get<AddressManager>()->handleLookupString(extraInfo);
        return;
    }

    switch (reasonCode) {
        case DomainHandler::ConnectionRefusedReason::ProtocolMismatch:
        case DomainHandler::ConnectionRefusedReason::TooManyUsers:
        case DomainHandler::ConnectionRefusedReason::Unknown: {
            const QString world = worldNameForPopup();
            QString why = reasonMessage.trimmed();
            if (reasonCode == DomainHandler::ConnectionRefusedReason::TooManyUsers) {
                why = "It's full. Try again later.";
            } else if (reasonCode == DomainHandler::ConnectionRefusedReason::ProtocolMismatch) {
                why = "It runs a different version of Darkride.";
            }
            OffscreenUi::asyncWarning("Couldn't join " + (world.isEmpty() ? QString("this world") : world), why);
            getMyAvatar()->setWorldVelocity(glm::vec3(0.0f));
            break;
        }
        case DomainHandler::ConnectionRefusedReason::NotAuthorized: {
            // a ban, or a world its owner keeps to some people; the world says so only once it knows who is asking, and
            // someone signed out gets the sign-in screen instead (DomainHandler asks for it)
            if (DependencyManager::get<AccountManager>()->isLoggedIn()) {
                const QString world = worldNameForPopup();
                OffscreenUi::asyncWarning("Couldn't join " + (world.isEmpty() ? QString("this world") : world),
                                          "You don't have permission to enter it.");
            }
            getMyAvatar()->setWorldVelocity(glm::vec3(0.0f));
            break;
        }
        default:
            // nothing to do.
            break;
    }
}

bool DomainSession::isServerlessMode() const {
    auto tree = qApp->getEntities()->getTree();
    if (tree) {
        return tree->isServerlessMode();
    }
    return false;
}

void DomainSession::setIsInterstitialMode(bool interstitialMode) {
    // whole-world loading uses interstitial mode for its loading screen whatever the user's own setting says
    bool enableInterstitial = DependencyManager::get<NodeList>()->getDomainHandler().getInterstitialModeEnabled() ||
        _worldLoading.active;
    if (enableInterstitial) {
        if (_interstitialMode != interstitialMode) {
            _interstitialMode = interstitialMode;
            emit interstitialModeChanged(_interstitialMode);

            DependencyManager::get<AudioClient>()->setAudioPaused(_interstitialMode);
            DependencyManager::get<AvatarManager>()->setMyAvatarDataPacketsPaused(_interstitialMode);
        }
    }
}

void DomainSession::setIsServerlessMode(bool serverlessDomain) {
    DependencyManager::get<NodeList>()->setSendDomainServerCheckInEnabled(!serverlessDomain);
    auto tree = qApp->getEntities()->getTree();
    if (tree) {
        tree->setIsServerlessMode(serverlessDomain);
    }
}

std::map<QString, QString> DomainSession::prepareServerlessDomainContents(QUrl domainURL, QByteArray data) {
    QUuid serverlessSessionID = QUuid::createUuid();
    getMyAvatar()->setSessionUUID(serverlessSessionID);
    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->setSessionUUID(serverlessSessionID);

    // there is no domain-server to tell us our permissions, so enable all
    NodePermissions permissions;
    permissions.setAll(true);
    nodeList->setPermissions(permissions);

    // FIXME: Lock the main tree and import directly into it.
    EntityTreePointer tmpTree(new EntityTree());
    tmpTree->setIsServerlessMode(true);
    tmpTree->createRootElement();
    // a world file from a link (http, atp) isn't local: an address in it that names a file on this computer is
    // emptied as it is read; a world file on this computer is the visitor's own and keeps every address
    const bool ownFile = ContentAddresses::isOnThisComputer(domainURL.toString());
    tmpTree->setRefusesContentFromThisComputer(!ownFile);
    qCDebug(interfaceapp) << "Serverless world" << domainURL.toString(QUrl::RemoveQuery)
                          << ": content addresses naming a file on this computer"
                          << (ownFile ? "allowed (the world is a file here)" : "refused (the world is a link)");
    auto myAvatar = getMyAvatar();
    tmpTree->setMyAvatar(myAvatar);
    bool success = tmpTree->readFromByteArray(domainURL.toString(), data);
    if (success) {
        tmpTree->reaverageOctreeElements();
        tmpTree->sendEntities(qApp->getEntityEditPacketSender(), qApp->getEntities()->getTree(), "domain", 0, 0, 0);
    }
    std::map<QString, QString> namedPaths = tmpTree->getNamedPaths();

    // we must manually eraseAllOctreeElements(false) else the tmpTree will mem-leak
    tmpTree->eraseAllOctreeElements(false);

    return namedPaths;
}

void DomainSession::loadServerlessDomain(QUrl domainURL) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, domainURL] { loadServerlessDomain(domainURL); });
        return;
    }

    if (domainURL.isEmpty()) {
        return;
    }

    QString trimmedUrl = domainURL.toString().trimmed();
    bool DEFAULT_IS_OBSERVABLE = true;
    const qint64 DEFAULT_CALLER_ID = -1;
    auto request = DependencyManager::get<ResourceManager>()->createResourceRequest(
        this, trimmedUrl, DEFAULT_IS_OBSERVABLE, DEFAULT_CALLER_ID, "DomainSession::loadServerlessDomain");

    if (!request) {
        return;
    }

    connect(request, &ResourceRequest::finished, this, [=]() {
        if (request->getResult() == ResourceRequest::Success) {
            auto namedPaths = prepareServerlessDomainContents(domainURL, request->getData());
            auto nodeList = DependencyManager::get<NodeList>();
            nodeList->getDomainHandler().connectedToServerless(namedPaths);
            _fullSceneReceivedCounter++;
        }
        request->deleteLater();
    });
    request->send();
}

void DomainSession::loadErrorDomain(QUrl domainURL) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, domainURL] { loadErrorDomain(domainURL); });
        return;
    }

    loadServerlessDomain(domainURL);
}

void DomainSession::requeryOctree() {      
    DependencyManager::get<EntityTreeRenderer>()->forceRecheckEntities();
    auto now = SteadyClock::now();
    static const std::chrono::seconds MIN_PERIOD_BETWEEN_QUERIES { 3 };
    _queryExpiry = SteadyClock::now();
    _octreeQuery.incrementConnectionID();
    QJsonObject queryJSONParameters;
    QJsonObject queryFlags;
    queryFlags["includeDescendants"] = true;
    queryFlags["includeAncestors"] = true;
    queryJSONParameters["flags"] = queryFlags;
    queryOctree(
        NodeType::EntityServer,
        PacketType::EntityQuery,
        queryJSONParameters
    );    
    _queryExpiry = now + MIN_PERIOD_BETWEEN_QUERIES;
}

void DomainSession::resetPhysicsReadyInformation() {
    // we've changed domains or cleared out caches or something.  we no longer know enough about the
    // collision information of nearby entities to make running bullet be safe.
    _fullSceneReceivedCounter = 0;
    _fullSceneCounterAtLastPhysicsCheck = 0;
    _gpuTextureMemSizeStabilityCount = 0;
    _gpuTextureMemSizeAtLastCheck = 0;
    _physicsEnabled = false;
    _octreeProcessor.stopSafeLanding();
    endWorldLoading();
    _worldLoading = WorldLoading();
}

void DomainSession::queryAvatars() {
    if (!isInterstitialMode()) {
        auto avatarPacket = NLPacket::create(PacketType::AvatarQuery);
        auto destinationBuffer = reinterpret_cast<unsigned char*>(avatarPacket->getPayload());
        unsigned char* bufferStart = destinationBuffer;

        uint8_t numFrustums = (uint8_t)qApp->getViewState().conicalViews().size();
        memcpy(destinationBuffer, &numFrustums, sizeof(numFrustums));
        destinationBuffer += sizeof(numFrustums);

        for (const auto& view : qApp->getViewState().conicalViews()) {
            destinationBuffer += view.serialize(destinationBuffer);
        }

        avatarPacket->setPayloadSize(destinationBuffer - bufferStart);

        DependencyManager::get<NodeList>()->broadcastToNodes(std::move(avatarPacket), NodeSet() << NodeType::AvatarMixer);
    }
}

int DomainSession::sendNackPackets() {

    // iterates through all nodes in NodeList
    auto nodeList = DependencyManager::get<NodeList>();

    int packetsSent = 0;

    nodeList->eachNode([&](const SharedNodePointer& node){

        if (node->getActiveSocket() && node->getType() == NodeType::EntityServer) {

            auto nackPacketList = NLPacketList::create(PacketType::OctreeDataNack);

            QUuid nodeUUID = node->getUUID();

            // if there are octree packets from this node that are waiting to be processed,
            // don't send a NACK since the missing packets may be among those waiting packets.
            if (_octreeProcessor.hasPacketsToProcessFrom(nodeUUID)) {
                return;
            }

            QSet<OCTREE_PACKET_SEQUENCE> missingSequenceNumbers;
            _octreeServerSceneStats.withReadLock([&] {
                // retrieve octree scene stats of this node
                if (_octreeServerSceneStats.find(nodeUUID) == _octreeServerSceneStats.end()) {
                    return;
                }
                // get sequence number stats of node, prune its missing set, and make a copy of the missing set
                SequenceNumberStats& sequenceNumberStats = _octreeServerSceneStats[nodeUUID].getIncomingOctreeSequenceNumberStats();
                sequenceNumberStats.pruneMissingSet();
                missingSequenceNumbers = sequenceNumberStats.getMissingSet();
            });

            _isMissingSequenceNumbers = (missingSequenceNumbers.size() != 0);

            // construct nack packet(s) for this node
            foreach(const OCTREE_PACKET_SEQUENCE& missingNumber, missingSequenceNumbers) {
                nackPacketList->writePrimitive(missingNumber);
            }

            if (nackPacketList->getNumPackets()) {
                packetsSent += (int)nackPacketList->getNumPackets();

                // send the packet list
                nodeList->sendPacketList(std::move(nackPacketList), *node);
            }
        }
    });

    return packetsSent;
}

void DomainSession::queryOctree(
    NodeType_t serverType,
    PacketType packetType,
    QJsonObject queryJSONParameters
) {

    if (!qApp->settingsLoaded()) {
        return; // bail early if settings are not loaded
    }

    const bool isModifiedQuery = !_physicsEnabled;
    
    if (isModifiedQuery) {   

        if (!_octreeProcessor.safeLandingIsActive()) {
            // don't send the octreeQuery until SafeLanding knows it has started
            return;
        }
        if (!_worldLoading.settingsKnown) {
            // the domain's settings say how to arrive (whole world or as we go): wait the moment they take
            return;
        }
    }
    if (isModifiedQuery && _worldLoading.active) {
        // no view at all: the entity server's traversal then takes in the whole world, and its "initial results
        // complete" means all of it has been sent
        _octreeQuery.clearConicalViews();
        _octreeQuery.setOctreeSizeScale(DEFAULT_OCTREE_SIZE_SCALE);
        static constexpr float MIN_LOD_ADJUST = -20.0f;
        _octreeQuery.setBoundaryLevelAdjust(MIN_LOD_ADJUST);
    } else if (isModifiedQuery) {
        // Create modified view that is a simple sphere.

        ConicalViewFrustum sphericalView;
        // the sphere is centred on where the avatar is now, not on the global bounding box: that box follows
        // _globalPosition, refreshed in simulateAttachments, so the query sent in the frame of a teleport (the
        // arrival's own path lookup) still asked around the old place, and the restarted landing had nothing of the
        // new place to wait for (WORLD-LOADING.md, the arrival gate)
        float radius = INITIAL_QUERY_RADIUS; //glm::max(INITIAL_QUERY_RADIUS, 350);//0.5 * glm::length(box.getDimensions())); 
        sphericalView.setPositionAndSimpleRadius(getMyAvatar()->getWorldPosition(), radius);

        auto menu = Menu::getInstance();
        if (menu && menu->isOptionChecked(MenuOption::LoadCompleteEntityTree)) {            // if Dev > Tivoli Options >>> LoadCompleteEntityTree truee

            ConicalViewFrustum farView;
            farView.set(qApp->getViewState().viewFrustum());

            QJsonObject queryFlags;
            queryFlags["includeDescendants"] = true; // forces entire tree
            queryFlags["includeAncestors"] = true; // forces entire tree
            queryFlags["serverScripts"] = true;

            queryJSONParameters["flags"] = queryFlags;
           // _octreeQuery->static_cast<EntityNodeData*>(node->getLinkedData()); //setShouldForceFullScene(true);
            _octreeQuery.clearConicalViews();                     // TIVOLI go frustumless
            _octreeQuery.setJSONParameters(queryJSONParameters);  // TIVOLI force ancestors and descendents
            _octreeQuery.setConicalViews({ sphericalView, farView });
        } else {
            _octreeQuery.setConicalViews({ sphericalView });
        }

        _octreeQuery.setOctreeSizeScale(DEFAULT_OCTREE_SIZE_SCALE);
        static constexpr float MIN_LOD_ADJUST = -20.0f;
        _octreeQuery.setBoundaryLevelAdjust(MIN_LOD_ADJUST);
    } else {
        ConicalViewFrustums views = qApp->getViewState().conicalViews();
        // the load zone (a Zone's culling mode "Cull Outside, Don't Load", ZONE-CULLING.md): while the avatar stands in
        // it, each view reaches no further than the zone's farthest corner, so the entity server sends nothing beyond
        // the zone from then on; what the client already holds stays (hidden by the culling), and leaving the zone
        // widens the view again. Never below the server's 3 m keyhole, and never an empty view list.
        const auto& zoneCull = DependencyManager::get<EntityTreeRenderer>()->getZoneCullState();
        if (zoneCull.dontLoad) {
            static const float LOAD_ZONE_MIN_REACH = 3.0f;
            for (auto& view : views) {
                view.clampFarClip(std::max(zoneCull.reachFrom(view.getPosition()), LOAD_ZONE_MIN_REACH));
            }
        }
        _octreeQuery.setConicalViews(views);
        auto lodManager = DependencyManager::get<LODManager>();
        _octreeQuery.setOctreeSizeScale(lodManager->getOctreeSizeScale());
        _octreeQuery.setBoundaryLevelAdjust(lodManager->getBoundaryLevelAdjust());
    }
    // ENTITY-STREAMING.md 3.3: this client draws impostor cards, so the entity server sizes a carded entity, and the
    // models under it, by its card and sends them out to where the card is drawn. One flag among the query's JSON
    // flags, kept whatever the paths above set (only the whole-tree option writes the JSON); a server that does not
    // know it sends as before. TIVOLI_NO_IMPOSTOR_CARDS in the environment leaves the flag out, to measure an older
    // client's stream against the same server (launch-editor.sh EDITOR_NO_IMPOSTOR_CARDS=1).
    static const bool SEND_IMPOSTOR_CARDS = !QProcessEnvironment::systemEnvironment().contains("TIVOLI_NO_IMPOSTOR_CARDS");
    if (SEND_IMPOSTOR_CARDS) {
        QJsonObject parameters = _octreeQuery.getJSONParameters();
        QJsonObject flags = parameters[EntityJSONQueryProperties::FLAGS_PROPERTY].toObject();
        if (!flags[EntityJSONQueryProperties::IMPOSTOR_CARDS_PROPERTY].toBool()) {
            flags[EntityJSONQueryProperties::IMPOSTOR_CARDS_PROPERTY] = true;
            parameters[EntityJSONQueryProperties::FLAGS_PROPERTY] = flags;
            _octreeQuery.setJSONParameters(parameters);
        }
    }
    _octreeQuery.setReportInitialCompletion(isModifiedQuery);

    auto treeRenderer = DependencyManager::get<EntityTreeRenderer>();

    auto nodeList = DependencyManager::get<NodeList>();
    auto node = nodeList->soloNodeOfType(serverType);

    if (node && node->getActiveSocket()) {
        _octreeQuery.setMaxQueryPacketsPerSecond(getMaxOctreePacketsPerSecond());

        auto queryPacket = NLPacket::create(packetType);

        // encode the query data
        auto packetData = reinterpret_cast<unsigned char*>(queryPacket->getPayload());
        int packetSize = _octreeQuery.getBroadcastData(packetData);
        queryPacket->setPayloadSize(packetSize);

        // TIVOLI_QUERY_DIAG in the environment: a log line per entity query sent, with the views it carries (the hunt
        // for entities that never reach a client, KNOWN-ISSUES.md). Off by default: nothing is formatted.
        static const bool QUERY_DIAG = QProcessEnvironment::systemEnvironment().contains("TIVOLI_QUERY_DIAG");
        if (QUERY_DIAG && serverType == NodeType::EntityServer) {
            QString views;
            const ConicalViewFrustums sentViews = _octreeQuery.getConicalViews();  // as sent: the load zone may have narrowed them
            for (const auto& view : sentViews) {
                const auto& p = view.getPosition();
                const auto& d = view.getDirection();
                views += QString(" [pos %1 %2 %3 dir %4 %5 %6 angle %7 far %8 radius %9]")
                    .arg(p.x, 0, 'f', 2).arg(p.y, 0, 'f', 2).arg(p.z, 0, 'f', 2)
                    .arg(d.x, 0, 'f', 3).arg(d.y, 0, 'f', 3).arg(d.z, 0, 'f', 3)
                    .arg(glm::degrees(view.getAngle()), 0, 'f', 1).arg(view.getFarClip(), 0, 'f', 0).arg(view.getRadius(), 0, 'f', 2);
            }
            qCDebug(interfaceapp).noquote() << "entity query:" << (isModifiedQuery ? "initial-sphere" : "views")
                << "views" << sentViews.size() << views
                << "lodAdjust" << _octreeQuery.getBoundaryLevelAdjust() << "sizeScale" << _octreeQuery.getOctreeSizeScale()
                << "pps" << _octreeQuery.getMaxQueryPacketsPerSecond() << "bytes" << packetSize;
        }

        // make sure we still have an active socket
        nodeList->sendUnreliablePacket(*queryPacket, *node);
    }
}

void DomainSession::clearDomainOctreeDetails(bool clearAll) {
    // if we're about to quit, we really don't need to do the rest of these things...
    if (qApp->isAboutToQuit()) {
        return;
    }

    qCDebug(interfaceapp) << "Clearing domain octree details...";

    resetPhysicsReadyInformation();
    setIsInterstitialMode(true);

    _octreeServerSceneStats.withWriteLock([&] {
        _octreeServerSceneStats.clear();
    });

    // reset the model renderer
    clearAll ? qApp->getEntities()->clear() : qApp->getEntities()->clearDomainAndNonOwnedEntities();

    DependencyManager::get<AnimationCache>()->clearUnusedResources();
    DependencyManager::get<SoundCache>()->clearUnusedResources();
    DependencyManager::get<MaterialCache>()->clearUnusedResources();
    DependencyManager::get<ModelCache>()->clearUnusedResources();
    ShaderCache::instance().clearUnusedResources();
    DependencyManager::get<TextureCache>()->clearUnusedResources();
    DependencyManager::get<recording::ClipCache>()->clearUnusedResources();
}

void DomainSession::domainURLChanged(QUrl domainURL) {
    // disable physics until we have enough information about our new location to not cause craziness.
    setIsServerlessMode(domainURL.scheme() != URL_SCHEME_HIFI);
    if (isServerlessMode()) {
        loadServerlessDomain(domainURL);
    }
    qApp->updateWindowTitle();
}

void DomainSession::goToErrorDomainURL(QUrl errorDomainURL) {
    // disable physics until we have enough information about our new location to not cause craziness.
    setIsServerlessMode(errorDomainURL.scheme() != URL_SCHEME_HIFI);
    if (isServerlessMode()) {
        loadErrorDomain(errorDomainURL);
    }
    qApp->updateWindowTitle();
}

void DomainSession::resettingDomain() {
    _notifiedPacketVersionMismatchThisDomain = false;

    clearDomainOctreeDetails(false);
}

void DomainSession::nodeAdded(SharedNodePointer node) {
    if (node->getType() == NodeType::EntityServer) {
        if (_failedToConnectToEntityServer && !_entityServerConnectionTimer.isActive()) {
            _octreeProcessor.stopSafeLanding();
            _failedToConnectToEntityServer = false;
        } else if (_entityServerConnectionTimer.isActive()) {
            _entityServerConnectionTimer.stop();
        }
        _octreeProcessor.startSafeLanding();
        _entityServerConnectionTimer.setInterval(ENTITY_SERVER_CONNECTION_TIMEOUT);
        _entityServerConnectionTimer.start();
    }
}

void DomainSession::nodeActivated(SharedNodePointer node) {
    if (node->getType() == NodeType::AssetServer) {
        // asset server just connected - check if we have the asset browser showing

        auto offscreenUi = getOffscreenUI();
        if (offscreenUi) {
            auto nodeList = DependencyManager::get<NodeList>();

            if (nodeList->getThisNodeCanWriteAssets()) {
                // call reload on the shown asset browser dialog to get the mappings (if permissions allow)
                auto assetDialog = offscreenUi ? offscreenUi->getRootItem()->findChild<QQuickItem*>("AssetServer") : nullptr;
                if (assetDialog) {
                    QMetaObject::invokeMethod(assetDialog, "reload");
                }
            } else {
                // we switched to an Asset Server that we can't modify, hide the Asset Browser
                offscreenUi->hide("AssetServer");
            }
        }
    }

    // If we get a new EntityServer activated, reset lastQueried time
    // so we will do a proper query during update
    if (node->getType() == NodeType::EntityServer) {
        _queryExpiry = SteadyClock::now();
        _octreeQuery.incrementConnectionID();

        if  (!_failedToConnectToEntityServer) {
            _entityServerConnectionTimer.stop();
        }
    }

    if (node->getType() == NodeType::AudioMixer && !isInterstitialMode()) {
        DependencyManager::get<AudioClient>()->negotiateAudioFormat();
    }

    if (node->getType() == NodeType::AvatarMixer) {
        _queryExpiry = SteadyClock::now();

        // new avatar mixer, send off our identity packet on next update loop
        // Reset skeletonModelUrl if the last server modified our choice.
        // Override the avatar url (but not model name) here too.
        if (qApp->getAvatarOverrideUrl().isValid()) {
            getMyAvatar()->useFullAvatarURL(qApp->getAvatarOverrideUrl());
        }

        if (getMyAvatar()->getFullAvatarURLFromPreferences() != getMyAvatar()->getSkeletonModelURL()) {
            getMyAvatar()->resetFullAvatarURL();
        }
        getMyAvatar()->markIdentityDataChanged();
        getMyAvatar()->resetLastSent();

        if (!isInterstitialMode()) {
            // transmit a "sendAll" packet to the AvatarMixer we just connected to.
            getMyAvatar()->sendAvatarDataPacket(true);
        }
    }
}

void DomainSession::nodeKilled(SharedNodePointer node) {
    // These are here because connecting NodeList::nodeKilled to OctreePacketProcessor::nodeKilled doesn't work:
    // OctreePacketProcessor::nodeKilled is not being called when NodeList::nodeKilled is emitted.
    // This may have to do with GenericThread::threadRoutine() blocking the QThread event loop

    _octreeProcessor.nodeKilled(node);

    qApp->getEntityEditPacketSender()->nodeKilled(node);

    // Let's somehow compute a network reliability value here depending on 
    // the frequency of things getting lost and use that to determine when
    // to show the user a message that they need a better network connection

    if (node->getType() == NodeType::DomainServer) qDebug() <<"*** DOMAIN SERVER LOST ***" << usecTimestampNow();
    if (node->getType() == NodeType::AvatarMixer) qDebug() << "*** AVATAR MIXER LOST ***" << usecTimestampNow();
    if (node->getType() == NodeType::MessagesMixer) qDebug() << "*** MESSAGES MIXER LOST ***" << usecTimestampNow();
    if (node->getType() == NodeType::EntityScriptServer) qDebug() << "*** ENTITY SCRIPT SERVER LOST ***" << usecTimestampNow();
    if (node->getType() == NodeType::AudioMixer) {
        qDebug() << "*** AUDIO MIXER LOST ***" << usecTimestampNow();
        QMetaObject::invokeMethod(DependencyManager::get<AudioClient>().data(), "audioMixerKilled");
    } else if (node->getType() == NodeType::EntityServer) {
        clearDomainOctreeDetails(false); // CPM No. Terrible user experience.
        qDebug() << "*** ENTITY SERVER LOST *** " << usecTimestampNow();
    } else if (node->getType() == NodeType::AssetServer) {
        // asset server going away - check if we have the asset browser showing
        qDebug() << "*** ASSET SERVER LOST ***" << usecTimestampNow();

        auto offscreenUi = getOffscreenUI();
        auto assetDialog = offscreenUi ? offscreenUi->getRootItem()->findChild<QQuickItem*>("AssetServer") : nullptr;

        if (assetDialog) {
            // call reload on the shown asset browser dialog
            QMetaObject::invokeMethod(assetDialog, "clear");
        }
    }
}

void DomainSession::trackIncomingOctreePacket(ReceivedMessage& message, SharedNodePointer sendingNode, bool wasStatsPacket) {
    // Attempt to identify the sender from its address.
    if (sendingNode) {
        const QUuid& nodeUUID = sendingNode->getUUID();

        // now that we know the node ID, let's add these stats to the stats for that node...
        _octreeServerSceneStats.withWriteLock([&] {
            if (_octreeServerSceneStats.find(nodeUUID) != _octreeServerSceneStats.end()) {
                OctreeSceneStats& stats = _octreeServerSceneStats[nodeUUID];
                stats.trackIncomingOctreePacket(message, wasStatsPacket, sendingNode->getClockSkewUsec());
            }
        });
    }
}

bool DomainSession::gpuTextureMemSizeStable() {
    auto renderConfig = qApp->getRenderEngine()->getConfiguration();
    auto renderStats = renderConfig->getConfig<render::EngineStats>("Stats");

    qint64 textureResourceGPUMemSize = renderStats->textureResourceGPUMemSize;
    qint64 texturePopulatedGPUMemSize = renderStats->textureResourcePopulatedGPUMemSize;
    qint64 textureTransferSize = renderStats->texturePendingGPUTransferSize;

    if (_gpuTextureMemSizeAtLastCheck == textureResourceGPUMemSize) {
        _gpuTextureMemSizeStabilityCount++;
    } else {
        _gpuTextureMemSizeStabilityCount = 0;
    }
    _gpuTextureMemSizeAtLastCheck = textureResourceGPUMemSize;

    if (_gpuTextureMemSizeStabilityCount >= _minimumGPUTextureMemSizeStabilityCount) {
        return (textureResourceGPUMemSize == texturePopulatedGPUMemSize) && (textureTransferSize == 0);
    }
    return false;
}

int DomainSession::processOctreeStats(ReceivedMessage& message, SharedNodePointer sendingNode) {
    // parse the incoming stats datas stick it in a temporary object for now, while we
    // determine which server it belongs to
    int statsMessageLength = 0;

    const QUuid& nodeUUID = sendingNode->getUUID();

    // now that we know the node ID, let's add these stats to the stats for that node...
    _octreeServerSceneStats.withWriteLock([&] {
        OctreeSceneStats& octreeStats = _octreeServerSceneStats[nodeUUID];
        statsMessageLength = octreeStats.unpackFromPacket(message);

        if (octreeStats.isFullScene()) {
            _fullSceneReceivedCounter++;
        }
    });

    return statsMessageLength;
}

void DomainSession::setSessionUUID(const QUuid& sessionUUID) const {
    Physics::setSessionUUID(sessionUUID);
}

void DomainSession::notifyPacketVersionMismatch() {
    if (!_notifiedPacketVersionMismatchThisDomain && !isInterstitialMode()) {
        _notifiedPacketVersionMismatchThisDomain = true;

        const QString world = worldNameForPopup();
        OffscreenUi::asyncWarning((world.isEmpty() ? QString("This world") : world) + " runs a different version of Darkride",
                                  "Some things in it may not look right.");
    }
}

void DomainSession::setMaxOctreePacketsPerSecond(int maxOctreePPS) {
    if (maxOctreePPS != _maxOctreePPS) {
        _maxOctreePPS = maxOctreePPS;
        _maxOctreePPSSetting.set(_maxOctreePPS);
    }
}

int DomainSession::getMaxOctreePacketsPerSecond() const {
    return _maxOctreePPS;
}

void DomainSession::setArrivalWaitsForNearby(bool wait) {
    // a script's word (worldLoading.js): it takes over the start-up's own wait, if that is still held
    if (_firstArrivalHold.held.exchange(false)) {
        qCDebug(interfaceapp) << "First arrival: the wait taken over by a script, which says" << wait;
    }
    setArrivalWait(wait);
}

void DomainSession::setArrivalWait(bool wait) {
    _arrivalWaitsForNearby = wait;
    // whole-world loading sets SafeLanding's wait itself, with its radius
    if (!_worldLoading.active) {
        _octreeProcessor.setSafeLandingWaitsForVisuals(wait);
    }
}

void DomainSession::holdFirstArrival() {
    _firstArrivalHold.connectedAt = 0;
    _firstArrivalHold.held = true;
    setArrivalWait(true);
    qCDebug(interfaceapp) << "First arrival: waits for what is near the avatar until a script takes it over, or" <<
        LAUNCH_HOLD_LIMIT_SECONDS << "s after the connect";
}

void DomainSession::updateFirstArrivalHold() {
    if (!_firstArrivalHold.held) {
        return;
    }
    const quint64 now = usecTimestampNow();
    if (_firstArrivalHold.connectedAt == 0) {
        if (isServerlessMode() || DependencyManager::get<NodeList>()->getDomainHandler().isConnected()) {
            _firstArrivalHold.connectedAt = now;
        }
        return;
    }
    if (now - _firstArrivalHold.connectedAt > (quint64)LAUNCH_HOLD_LIMIT_SECONDS * USECS_PER_SECOND) {
        // no script took it over (no worldLoading.js running): the start-up's wait ends by itself, unless a script
        // takes it over in this very moment (then the script's word stands)
        if (_firstArrivalHold.held.exchange(false)) {
            setArrivalWait(false);
            qCDebug(interfaceapp) << "First arrival: no script took the wait over;" << LAUNCH_HOLD_LIMIT_SECONDS <<
                "s after the connect it ends (physics" << (_physicsEnabled ? "on" : "not yet on") << ")";
        }
    }
}

void DomainSession::tryToEnablePhysics(bool skipTextureCheck) {
    bool enableInterstitial = DependencyManager::get<NodeList>()->getDomainHandler().getInterstitialModeEnabled() ||
        _worldLoading.active || _arrivalWaitsForNearby;
    if (skipTextureCheck || !enableInterstitial || gpuTextureMemSizeStable()) {
        _fullSceneCounterAtLastPhysicsCheck = _fullSceneReceivedCounter;
        _lastQueriedViews.clear();  // Force new view.
        // process octree stats packets are sent in between full sends of a scene (this isn't currently true).
        // We keep physics disabled until we've received a full scene and everything near the avatar in that
        // scene is ready to compute its collision shape.
        auto myAvatar = getMyAvatar();
        if (myAvatar->isReadyForPhysics()) {
            bool safeLandingComplete = _octreeProcessor.safeLandingIsComplete();   // read before the reset below clears it
            myAvatar->getCharacterController()->setPhysicsEngine(qApp->getPhysicsEngine());
            _octreeProcessor.resetSafeLanding();
            _physicsEnabled = true;
            StartupProfiler::instant("physics enabled: in the world");
            setIsInterstitialMode(false);
            myAvatar->updateMotionBehaviorFromMenu();
            qDebug() << "Physics enabled" << "(ghosting" << DependencyManager::get<EntityTreeRenderer>()->getGhostingAllowed()
                << "arrival waits for nearby" << _arrivalWaitsForNearby << "interstitial"
                << DependencyManager::get<NodeList>()->getDomainHandler().getInterstitialModeEnabled()
                << "safe landing complete" << safeLandingComplete << "full scenes" << _fullSceneReceivedCounter << ")";
            DependencyManager::get<EntityTreeRenderer>()->setSceneIsReady(true);
            // the renderer rechecks zone culling now that the scene is ready. Until 2026-10-19 this was
            // requeryOctree, whose new connection id made the entity server resend the whole view it had just sent
            // (Tivoli's 14f41d044a: "hit it with a quick refresh to make sure everything is drawn"); the query with
            // the real views goes out from the update loop as before
            DependencyManager::get<EntityTreeRenderer>()->forceRecheckEntities();
        }
    }
}

// Whole-world loading. A domain's owner may ask (its settings' "loading" group) that visitors see a loading screen
// until the whole world has arrived and its models and textures have loaded, instead of watching it stream in. The
// settings come from the domain-server when the client connects; until they are here (a moment) the client sends no
// entity query and does not start physics, and a domain that sends none within a few seconds loads as we go.
void DomainSession::updateWorldLoading() {
    if (_worldLoading.settingsKnown) {
        return;
    }
    auto& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();
    quint64 now = usecTimestampNow();
    if (!domainHandler.isConnected() || _worldLoading.waitStart == 0) {
        _worldLoading.waitStart = now;
        return;
    }
    const QJsonObject& settings = domainHandler.getSettingsObject();
    if (settings.isEmpty()) {
        static const quint64 SETTINGS_WAIT = 3 * USECS_PER_SECOND;
        if (now - _worldLoading.waitStart > SETTINGS_WAIT) {
            qCDebug(interfaceapp) << "World loading: no settings from the domain, loading as we go";
            _worldLoading.settingsKnown = true;
        }
        return;
    }
    _worldLoading.settingsKnown = true;
    QJsonObject loading = settings["loading"].toObject();
    if (loading["whole_world"].toVariant().toBool()) {
        static const int DEFAULT_TIME_LIMIT = 60;
        static const int MIN_TIME_LIMIT = 10;
        static const int MAX_TIME_LIMIT = 600;
        int timeLimit = loading["time_limit"].toVariant().toInt();
        if (timeLimit <= 0) {
            timeLimit = DEFAULT_TIME_LIMIT;
        }
        // the whole world always arrives; the screen waits for the models and textures within this distance of
        // the avatar (0: all of them)
        static const float DEFAULT_RADIUS = 100.0f;
        static const float MAX_RADIUS = 10000.0f;
        bool hasRadius = false;
        float radius = loading["radius"].toVariant().toFloat(&hasRadius);
        if (!hasRadius || radius < 0.0f) {
            radius = DEFAULT_RADIUS;
        }
        startWorldLoading(glm::clamp(timeLimit, MIN_TIME_LIMIT, MAX_TIME_LIMIT), glm::min(radius, MAX_RADIUS));
    }
}

void DomainSession::startWorldLoading(int timeLimitSeconds, float radius) {
    _worldLoading.active = true;
    _worldLoading.skipRequested = false;
    _worldLoading.timeLimit = timeLimitSeconds;
    _worldLoading.radius = radius;
    _worldLoading.started = usecTimestampNow();
    _worldLoading.deadline = _worldLoading.started + (quint64)timeLimitSeconds * USECS_PER_SECOND;
    _worldLoading.lastQuery = 0;
    _octreeProcessor.setSafeLandingWaitsForVisuals(true, radius);
    setIsInterstitialMode(true);
    // a new connection id: the entity server drops what it knows of this client and starts a first traversal, of
    // the whole world now that the query has no view
    _octreeQuery.incrementConnectionID();
    qCDebug(interfaceapp) << "World loading: the whole world first, for at most" << timeLimitSeconds << "s, waiting for the models" <<
        (radius > 0.0f ? QString("within %1 m").arg(radius) : QString("everywhere"));
    emit worldLoadingChanged(true);
}

void DomainSession::endWorldLoading() {
    if (!_worldLoading.active) {
        return;
    }
    setIsInterstitialMode(false);  // while still active, so it ends whatever the user's interstitial setting says
    _worldLoading.active = false;
    _octreeProcessor.setSafeLandingWaitsForVisuals(false);
    emit worldLoadingChanged(false);
}

QVariantMap DomainSession::getWorldLoadingState() {
    QVariantMap state;
    state["active"] = _worldLoading.active;
    if (_worldLoading.active) {
        auto progress = _octreeProcessor.safeLandingProgress();
        quint64 now = usecTimestampNow();
        state["received"] = progress.received;
        state["ready"] = progress.received - progress.waiting;
        state["serverDone"] = progress.serverDone;
        state["seconds"] = (double)(now - _worldLoading.started) / USECS_PER_SECOND;
        state["secondsLeft"] = _worldLoading.deadline > now ? (double)(_worldLoading.deadline - now) / USECS_PER_SECOND : 0.0;
        state["timeLimit"] = _worldLoading.timeLimit;
        state["radius"] = _worldLoading.radius;
    }
    return state;
}

void DomainSession::showWorldNow() {
    if (_worldLoading.active) {
        _worldLoading.skipRequested = true;
    }
}

void DomainSession::enablePhysicsWhenReady() {
    updateFirstArrivalHold();   // outside the gate: it only sets or clears the wait the gate reads
    if (!_physicsEnabled) {
        if (!_domainLoadingInProgress) {
            PROFILE_ASYNC_BEGIN(app, "Scene Loading", "");
            _domainLoadingInProgress = true;
            qDebug() << "SCENE LOADING";
        }

        // we haven't yet enabled physics.  we wait until we think we have all the collision information
        // for nearby entities before starting bullet up.
        if (isServerlessMode()) {
            tryToEnablePhysics();
        } else if (_failedToConnectToEntityServer) {
            if (_octreeProcessor.safeLandingIsActive()) {
                _octreeProcessor.stopSafeLanding();
                qDebug() << "FAILED TO CONNECT TO ENTITY SERVER";
            }
        } else {
            _octreeProcessor.updateSafeLanding();
            updateWorldLoading();

            if (_worldLoading.active) {
                // the whole world first: until it has all arrived and loaded, "Show me now", or the time limit
                bool timedOut = usecTimestampNow() > _worldLoading.deadline;
                if (_worldLoading.skipRequested || timedOut) {
                    tryToEnablePhysics(true);
                } else if (_octreeProcessor.safeLandingIsComplete()) {
                    tryToEnablePhysics();
                }
                if (_physicsEnabled) {
                    qCDebug(interfaceapp) << "World loading:" << (_worldLoading.skipRequested ? "shown on request" :
                        timedOut ? "time limit reached" : "the whole world is in") << "after" <<
                        (usecTimestampNow() - _worldLoading.started) / USECS_PER_MSEC << "ms";
                    endWorldLoading();
                }
            } else if (_worldLoading.settingsKnown &&
                       // ghosting lets the avatar move before the world near it is in, unless a loading screen waits
                       ((DependencyManager::get<EntityTreeRenderer>()->getGhostingAllowed() && !_arrivalWaitsForNearby) ||
                        _octreeProcessor.safeLandingIsComplete())) {
                tryToEnablePhysics();
            }
        }
    } else if (_domainLoadingInProgress) {
        _domainLoadingInProgress = false;
        PROFILE_ASYNC_END(app, "Scene Loading", "");
    }
}

void DomainSession::queryServers() {
    PROFILE_RANGE_EX(app, "QueryOctree", 0xffff0000, (uint64_t)qApp->getActiveDisplayPlugin()->presentCount());
    PerformanceTimer perfTimer("queryOctree");
    QMutexLocker viewLocker(&qApp->getViewState().mutex());

    bool viewIsDifferentEnough = false;
    if (qApp->getViewState().conicalViews().size() == _lastQueriedViews.size()) {
        for (size_t i = 0; i < qApp->getViewState().conicalViews().size(); ++i) {
            if (!qApp->getViewState().conicalViews()[i].isVerySimilar(_lastQueriedViews[i])) {
                viewIsDifferentEnough = true;
                break;
            }
        }
    } else {
        viewIsDifferentEnough = true;
    }


    // if it's been a while since our last query or the view has significantly changed then send a query, otherwise suppress it
    static const std::chrono::seconds MIN_PERIOD_BETWEEN_QUERIES { 2 };
    auto now = SteadyClock::now();

    // The periodic query keeps its connection. Until 2026-10-19 it went through requeryOctree, whose new connection
    // id makes the entity server forget what this client holds and resend its whole view: every 3 s, for as long
    // as the client ran (Tivoli's 14f41d044a of 2020, "Fix entities intermittently not rendering"; measured 26
    // full sends, 2.6 MB, in 97 s over a still field of 200 entities). The renderer's recheck of zone culling
    // from that fix stays periodic. requeryOctree, with its new connection id, remains for the explicit
    // refreshes: Redraw (Ctrl+Shift+T), a reload of the caches, physics coming up.
    bool enoughTimePassed = now > _queryExpiry;
    if (_worldLoading.active) {
        // the same whole-world query again now and then (it travels unreliably), never a new one: a new
        // connection id would start the entity server's traversal of the world over
        quint64 usecNow = usecTimestampNow();
        if (usecNow - _worldLoading.lastQuery > USECS_PER_SECOND) {
            queryOctree(NodeType::EntityServer, PacketType::EntityQuery, QJsonObject());
            _worldLoading.lastQuery = usecNow;
        }
    } else if (enoughTimePassed) {
        DependencyManager::get<EntityTreeRenderer>()->forceRecheckEntities();
    }

    if (enoughTimePassed || viewIsDifferentEnough) {            
        if (DependencyManager::get<SceneScriptingInterface>()
        ->shouldRenderEntities()
        ) {
            QJsonObject queryJSONParameters;
            QJsonObject queryFlags;
            queryFlags["includeDescendants"] = true;
            queryFlags["includeAncestors"] = true;
            queryFlags["serverScripts"] = true; 
            queryJSONParameters["flags"] = queryFlags;
            queryOctree(
                NodeType::EntityServer,
                PacketType::EntityQuery,
                queryJSONParameters
            );
        }
        queryAvatars();

        _lastQueriedViews = qApp->getViewState().conicalViews();
        _queryExpiry = now + MIN_PERIOD_BETWEEN_QUERIES;
    }
}

void DomainSession::sendNacksWhenDue(quint64 now) {
    quint64 sinceLastNack = now - _lastNackTime;
    const quint64 TOO_LONG_SINCE_LAST_NACK = 1 * USECS_PER_SECOND;
    if (sinceLastNack > TOO_LONG_SINCE_LAST_NACK) {
        _lastNackTime = now;
        sendNackPackets();
    }
}

void DomainSession::connectedToDomain() {
    if (!isServerlessMode()) {
        _entityServerConnectionTimer.setInterval(ENTITY_SERVER_ADDED_TIMEOUT);
        _entityServerConnectionTimer.start();
        _failedToConnectToEntityServer = false;
    }
}

void DomainSession::disconnectedFromDomain() {
    _failedToConnectToEntityServer = false;
}

void DomainSession::arrivedWithoutPhysics() {
    if (!_physicsEnabled) {
        // when we arrive somewhere without physics enabled --> startSafeLanding
        _octreeProcessor.startSafeLanding();
        // and query the entity server at once: the restarted landing completes on the server's next report of the
        // initial scene, which it makes on the next query that asks for it; left to the periodic query that was
        // up to 3 s of waiting for nothing (WORLD-LOADING.md, the arrival gate)
        _queryExpiry = SteadyClock::now();
    }
}

// a link or file the app was opened with, taken as a drop or a click is, once the app is up
static void acceptLater(const QString& link) {
    QMetaObject::invokeMethod(qApp, [link] {
        if (qApp->canAcceptURL(link)) {
            qApp->acceptURL(link);
        }
    }, Qt::QueuedConnection);
}

void DomainSession::noStartingLocation() {
    if (!_startupLink.isEmpty()) {
        acceptLater(_startupLink);
        _startupLink.clear();
    }
    _startingLocationChosen = true;
}

void DomainSession::goToStartingLocation() {
    PROFILE_RANGE(render, __FUNCTION__);

    // Get controller availability
#ifdef ANDROID_APP_QUEST_INTERFACE
    bool hasHandControllers = true;
#else
    bool hasHandControllers = false;
    if (PluginUtils::isViveControllerAvailable() || PluginUtils::isOculusTouchControllerAvailable()) {
        hasHandControllers = true;
    }
#endif

    // Check HMD use (may be technically available without being in use)
    bool hasHMD = PluginUtils::isHMDAvailable();
    bool isUsingHMD = qApp->getActiveDisplayPlugin()->isHmd();
    bool isUsingHMDAndHandControllers = hasHMD && hasHandControllers && isUsingHMD;

    qCDebug(interfaceapp) << "HMD:" << hasHMD << ", Hand Controllers: " << hasHandControllers << ", Using HMD: " << isUsingHMDAndHandControllers;

    QString addressLookupString;

    // when --url in command line, teleport to location
    QCommandLineParser parser;
    QCommandLineOption urlOption("url", "", "value");
    parser.addOption(urlOption);
    parser.parse(QCoreApplication::arguments());
    if (parser.isSet(urlOption)) {
        QUrl url = QUrl(parser.value(urlOption));
        if (url.scheme() == URL_SCHEME_HIFIAPP) {
            Setting::Handle<QVariant>("startUpApp").set(url.path());
        } else {
            addressLookupString = url.toString();
        }
    } else if (!_startupLink.isEmpty()) {
        // the link Darkride was opened with: there, as --url would, the entry on a first run included
        QString link = _startupLink;
        if (isWorldAddressScheme(QUrl(link).scheme())) {
            addressLookupString = link;
            _overrideEntry = true;
        } else {
            // a file opened with the app (dropped on its icon): opened as a drop is, once the app is up
            acceptLater(link);
        }
    }
    _startupLink.clear();
    _startingLocationChosen = true;

    // If this is a first run we short-circuit the address passed in
    if (qApp->firstRun().get()) {
        if (!_overrideEntry) {
            DependencyManager::get<AddressManager>()->goToEntry();
        } else {
            DependencyManager::get<AddressManager>()->loadSettings(addressLookupString);
        }
       qApp->firstRun().set(false);
    } else {
        QString goingTo = "";
        if (addressLookupString.isEmpty()) {
            if (Menu::getInstance()->isOptionChecked(MenuOption::HomeLocation)) {
                auto locationBookmarks = DependencyManager::get<LocationBookmarks>();
                addressLookupString = locationBookmarks->addressForBookmark(LocationBookmarks::HOME_BOOKMARK);
                goingTo = "home location";
            } else {
                goingTo = "previous location";
            }
            // a saved world (not a link) that doesn't answer falls back to the entry world (no home saved: the last)
            _connectionMonitor.setStartUpFallback(addressLookupString.isEmpty() ? "last" : "home");
        }
        qCDebug(interfaceapp) << "Not first run... going to" << qPrintable(!goingTo.isEmpty() ? goingTo : addressLookupString);
        DependencyManager::get<AddressManager>()->loadSettings(addressLookupString);
    }
    
    _connectionMonitor.init();
}

void DomainSession::reloadResourceCaches() {
    resetPhysicsReadyInformation();

    // Query the octree to refresh everything in view
    _queryExpiry = SteadyClock::now();
    _octreeQuery.incrementConnectionID();

    QJsonObject queryJSONParameters;
    QJsonObject queryFlags;
    queryFlags["includeDescendants"] = true;
    queryFlags["includeAncestors"] = true;
    queryJSONParameters["flags"] = queryFlags;
    queryOctree(
        NodeType::EntityServer,
        PacketType::EntityQuery,
        queryJSONParameters
    );

    getMyAvatar()->prepareAvatarEntityDataForReload();
    // Clear the entities and their renderables
    qApp->getEntities()->clear();

    DependencyManager::get<AssetClient>()->clearCache();
    DependencyManager::get<ScriptCache>()->clearCache();

    // Clear all the resource caches
    DependencyManager::get<ResourceCacheSharedItems>()->clear();
    DependencyManager::get<AnimationCache>()->refreshAll();
    DependencyManager::get<SoundCache>()->refreshAll();
    DependencyManager::get<MaterialCache>()->refreshAll();
    DependencyManager::get<ModelCache>()->refreshAll();
    ShaderCache::instance().refreshAll();
    DependencyManager::get<TextureCache>()->refreshAll();
    DependencyManager::get<recording::ClipCache>()->refreshAll();

    DependencyManager::get<NodeList>()->reset("Reloading resources");  // Force redownload of .fst models

    DependencyManager::get<ScriptEngines>()->reloadAllScripts();
    getOffscreenUI()->clearCache();

    DependencyManager::get<Keyboard>()->createKeyboard();

    getMyAvatar()->resetFullAvatarURL();
}

void DomainSession::goToHandedOffUrl(const QString& url) const {
    // a later launch's --url, handed over by SingleInstance: go there, as that launch would have
    if (DependencyManager::get<AddressManager>()->handleLookupString(url)) {
        qApp->raiseWindow();
    }
}
