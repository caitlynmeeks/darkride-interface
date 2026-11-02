//
//  AvatarMixerClientData.cpp
//  assignment-client/src/avatars
//
//  Created by Stephen Birarda on 2/4/2014.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AvatarMixerClientData.h"

#include <algorithm>
#include <cstddef>
#include <udt/PacketHeaders.h>

#include <DependencyManager.h>
#include <NodeList.h>
#include <SharedUtil.h>
#include <EntityTree.h>
#include <ZoneEntityItem.h>
#include <WebEntityItem.h>
#include <ScriptAddressCheck.h>

#include "AvatarLogging.h"
#include "AvatarMixerChecks.h"

#include "AvatarMixerWorker.h"

// An entity an avatar carries holds to the same rules as the entity server's, for everyone: a Web entity whose address
// isn't a web page's, and any entity whose script or server scripts aren't on the same allow-list as the entity
// server's (ScriptAddressCheck), is never passed on. Its data is read from a zero-padded copy and passed on only when it
// reads cleanly.
static const int READ_PADDING = 1 << 20;

// AvatarMixerChecks reads the skeleton by these offsets
static_assert(sizeof(AvatarSkeletonTrait::Header) == avatar_mixer_checks::SKELETON_HEADER_SIZE, "skeleton header");
static_assert(offsetof(AvatarSkeletonTrait::Header, numJoints) == avatar_mixer_checks::SKELETON_NUM_JOINTS_AT, "numJoints");
static_assert(offsetof(AvatarSkeletonTrait::Header, stringTableLength) == avatar_mixer_checks::SKELETON_TABLE_LENGTH_AT,
              "stringTableLength");
static_assert(sizeof(AvatarSkeletonTrait::JointData) == avatar_mixer_checks::SKELETON_JOINT_SIZE, "joint record");
static_assert(offsetof(AvatarSkeletonTrait::JointData, stringStart) == avatar_mixer_checks::SKELETON_STRING_START_AT,
              "stringStart");
static_assert(offsetof(AvatarSkeletonTrait::JointData, stringLength) == avatar_mixer_checks::SKELETON_STRING_LENGTH_AT,
              "stringLength");

namespace {

// Reads the properties every entity has, its scripts among them. They come before a type's own in the data
// (EntityItem::readEntityDataFromBuffer), so they read the same as in the app, whatever the type; a type's own are left.
class CommonPropertiesReader : public EntityItem {
public:
    CommonPropertiesReader(const EntityItemID& entityItemID) : EntityItem(entityItemID) {}
    ALLOW_INSTANTIATION
};

// Why a worn entity isn't passed on (empty when it is), and the value refused, for the log.
struct CarriedRefusal {
    QString reason;
    QString value;
};

CarriedRefusal scriptRefusal(const EntityItem& entity) {
    for (const QString& script : { entity.getScript(), entity.getServerScripts() }) {
        QString refusal = script_address::refusalReason(script);
        if (!refusal.isEmpty()) {
            return { "a script that has " + refusal, script.left(200) };
        }
    }
    return {};
}

}

// Why an entity an avatar carries isn't passed on, or an empty reason when it is.
static CarriedRefusal refusalForCarriedEntity(const QByteArray& data) {
    EntityTypes::EntityType type = EntityTypes::Unknown;
    QUuid id;
    EntityTypes::extractEntityTypeAndID(reinterpret_cast<const unsigned char*>(data.constData()), data.size(), type, id);
    if (data.size() > AvatarTraits::MAXIMUM_TRAIT_SIZE) {
        return { "more data than an app sends", QString() };
    }
    // one zeroed buffer per worker thread: only the data's own bytes are written, and zeroed again after
    thread_local std::vector<unsigned char> buffer(AvatarTraits::MAXIMUM_TRAIT_SIZE + READ_PADDING, 0);
    memcpy(buffer.data(), data.constData(), data.size());
    CarriedRefusal refusal { "data that doesn't read cleanly", QString() };
    try {
        // read as the apps read it (OtherAvatar), but freed here at once: EntityTypes' factory frees with deleteLater,
        // and this worker thread runs no event loop
        ReadBitstreamToTreeParams args;
        if (type == EntityTypes::Web) {
            auto entity = std::make_shared<WebEntityItem>(EntityItemID(id));
            int bytesRead = entity->readEntityDataFromBuffer(buffer.data(), data.size(), args);
            if (bytesRead > 0 && bytesRead <= data.size()) {
                refusal = WebEntityItem::isWebPageUrl(entity->getSourceUrl())
                    ? scriptRefusal(*entity)
                    : CarriedRefusal { "an address that isn't a web page's", entity->getSourceUrl().left(200) };
            }
        } else {
            // an entity of any type whose read runs past its data (a string's length running over it) is refused
            auto entity = std::make_shared<CommonPropertiesReader>(EntityItemID(id));
            int bytesRead = entity->readEntityDataFromBuffer(buffer.data(), data.size(), args);
            if (bytesRead > 0 && bytesRead <= data.size()) {
                refusal = scriptRefusal(*entity);
            }
        }
    } catch (...) {
        // unreadable: refused
    }
    memset(buffer.data(), 0, data.size());
    return refusal;
}

AvatarMixerClientData::AvatarMixerClientData(const QUuid& nodeID, Node::LocalID nodeLocalID) : NodeData(nodeID, nodeLocalID) {
    // in case somebody calls getSessionUUID on the AvatarData instance, make sure it has the right ID
    _avatar->setID(nodeID);
}

uint64_t AvatarMixerClientData::getLastOtherAvatarEncodeTime(NLPacket::LocalID otherAvatar) const {
    const auto itr = _lastOtherAvatarEncodeTime.find(otherAvatar);
    if (itr != _lastOtherAvatarEncodeTime.end()) {
        return itr->second;
    }
    return 0;
}

void AvatarMixerClientData::setLastOtherAvatarEncodeTime(NLPacket::LocalID otherAvatar, uint64_t time) {
    auto itr = _lastOtherAvatarEncodeTime.find(otherAvatar);
    if (itr != _lastOtherAvatarEncodeTime.end()) {
        itr->second = time;
    } else {
        _lastOtherAvatarEncodeTime.emplace(std::pair<NLPacket::LocalID, uint64_t>(otherAvatar, time));
    }
}

void AvatarMixerClientData::queuePacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer node) {
    if (!_packetQueue.node) {
        _packetQueue.node = node;
    }
    _packetQueue.push(message);
}

int AvatarMixerClientData::processPackets(const WorkerSharedData& workerSharedData) {
    int packetsProcessed = 0;
    SharedNodePointer node = _packetQueue.node;
    assert(_packetQueue.empty() || node);
    _packetQueue.node.clear();

    while (!_packetQueue.empty()) {
        auto& packet = _packetQueue.front();

        packetsProcessed++;

        switch (packet->getType()) {
            case PacketType::AvatarData:
                parseData(*packet, workerSharedData);
                break;
            case PacketType::SetAvatarTraits:
                processSetTraitsMessage(*packet, workerSharedData, *node);
                break;
            case PacketType::BulkAvatarTraitsAck:
                processBulkAvatarTraitsAckMessage(*packet);
                break;
            default:
                Q_UNREACHABLE();
        }
        _packetQueue.pop();
    }
    assert(_packetQueue.empty());

    return packetsProcessed;
}

namespace {
using std::static_pointer_cast;

// Operator to find if a point is within an avatar-priority (hero) Zone Entity.
struct FindContainingZone {
    glm::vec3 position;
    bool isInPriorityZone { false };
    float priorityZoneVolume { std::numeric_limits<float>::max() };

    static bool operation(const OctreeElementPointer& element, void* extraData) {
        auto findContainingZone = static_cast<FindContainingZone*>(extraData);
        if (element->getAACube().contains(findContainingZone->position)) {
            const EntityTreeElementPointer entityTreeElement = static_pointer_cast<EntityTreeElement>(element);
            entityTreeElement->forEachEntity([&findContainingZone](EntityItemPointer item) {
                if (item->getType() == EntityTypes::Zone && item->contains(findContainingZone->position)) {
                    auto zoneItem = static_pointer_cast<ZoneEntityItem>(item);
                    auto avatarPriorityProperty = zoneItem->getAvatarPriority();
                    float volume = zoneItem->getVolumeEstimate();
                    if (avatarPriorityProperty != COMPONENT_MODE_INHERIT
                        && volume < findContainingZone->priorityZoneVolume) {  // Smaller volume wins
                        findContainingZone->isInPriorityZone = avatarPriorityProperty == COMPONENT_MODE_ENABLED;
                        findContainingZone->priorityZoneVolume = volume;
                    }
                }
            });
            return true;  // Keep recursing
        } else {          // Position isn't within this subspace, so end recursion.
            return false;
        }
    }
};

}  // namespace

int AvatarMixerClientData::parseData(ReceivedMessage& message, const WorkerSharedData& workerSharedData) {
    // pull the sequence number from the data first
    uint16_t sequenceNumber;

    message.readPrimitive(&sequenceNumber);

    if (sequenceNumber < _lastReceivedSequenceNumber && _lastReceivedSequenceNumber != UINT16_MAX) {
        incrementNumOutOfOrderSends();
    }
    _lastReceivedSequenceNumber = sequenceNumber;
    glm::vec3 oldPosition = _avatar->getClientGlobalPosition();
    bool oldHasPriority = _avatar->getHasPriority();

    // compute the offset to the data payload
    if (!_avatar->parseDataFromBuffer(message.readWithoutCopy(message.getBytesLeftToRead()))) {
        return false;
    }

    // Regardless of what the client says, restore the priority as we know it without triggering any update.
    _avatar->setHasPriorityWithoutTimestampReset(oldHasPriority);

    auto newPosition = _avatar->getClientGlobalPosition();
    if (newPosition != oldPosition || _avatar->getNeedsHeroCheck()) {
        EntityTree& entityTree = *workerSharedData.entityTree;
        FindContainingZone findContainingZone{ newPosition };
        entityTree.recurseTreeWithOperation(&FindContainingZone::operation, &findContainingZone);
        bool currentlyHasPriority = findContainingZone.isInPriorityZone;
        if (currentlyHasPriority != _avatar->getHasPriority()) {
            _avatar->setHasPriority(currentlyHasPriority);
        }
        _avatar->setNeedsHeroCheck(false);
    }

    return true;
}

void AvatarMixerClientData::processSetTraitsMessage(ReceivedMessage& message,
                                                    const WorkerSharedData& workerSharedData,
                                                    Node& sendingNode) {
    // Trying to read more bytes than available, bail
    if (message.getBytesLeftToRead() < qint64(sizeof(AvatarTraits::TraitVersion))) {
        logRefusedData("a traits packet", "no version", sendingNode.getUUID());
        return;
    }

    // pull the trait version from the message
    AvatarTraits::TraitVersion packetTraitVersion;
    message.readPrimitive(&packetTraitVersion);

    bool anyTraitsChanged = false;

    while (message.getBytesLeftToRead() > 0) {
        // for each trait in the packet, apply it if the trait version is newer than what we have

        // Trying to read more bytes than available, bail
        if (message.getBytesLeftToRead() < qint64(sizeof(AvatarTraits::TraitType))) {
            logRefusedData("a traits packet", "a trait cut short", sendingNode.getUUID());
            return;
        }

        AvatarTraits::TraitType traitType;
        message.readPrimitive(&traitType);

        if (AvatarTraits::isSimpleTrait(traitType)) {
            // Trying to read more bytes than available, bail
            if (message.getBytesLeftToRead() < qint64(sizeof(AvatarTraits::TraitWireSize))) {
                logRefusedData("a traits packet", "a trait cut short", sendingNode.getUUID());
                return;
            }

            AvatarTraits::TraitWireSize traitSize;
            message.readPrimitive(&traitSize);

            // a simple trait has no deleted form (DELETED_TRAIT_SIZE is for instances)
            if (traitSize < 0 || traitSize > message.getBytesLeftToRead()) {
                logRefusedData("a traits packet", QString("a trait of size %1").arg(traitSize), sendingNode.getUUID());
                break;
            }

            if (packetTraitVersion > _lastReceivedTraitVersions[traitType]) {
                QByteArray traitData = message.read(traitSize);
                const QString refusal = traitType == AvatarTraits::SkeletonData
                    ? avatar_mixer_checks::skeletonRefusal(traitData) : QString();
                if (!refusal.isEmpty()) {
                    // dropped: nothing is unpacked or passed on, and the version stays, so a good one still applies
                    logRefusedData("a skeleton", refusal, sendingNode.getUUID());
                    continue;
                }
                _avatar->processTrait(traitType, traitData);
                _lastReceivedTraitVersions[traitType] = packetTraitVersion;
                anyTraitsChanged = true;
            } else {
                message.seek(message.getPosition() + traitSize);
            }
        } else {
            // Trying to read more bytes than available, bail
            if (message.getBytesLeftToRead() < qint64(NUM_BYTES_RFC4122_UUID + sizeof(AvatarTraits::TraitWireSize))) {
                logRefusedData("a traits packet", "a trait cut short", sendingNode.getUUID());
                return;
            }

            AvatarTraits::TraitInstanceID instanceID = QUuid::fromRfc4122(message.readWithoutCopy(NUM_BYTES_RFC4122_UUID));

            AvatarTraits::TraitWireSize traitSize;
            message.readPrimitive(&traitSize);

            if (traitSize < -1 || traitSize > message.getBytesLeftToRead()) {
                logRefusedData("a traits packet", QString("an instance of size %1").arg(traitSize), sendingNode.getUUID());
                break;
            }

            if (traitType == AvatarTraits::AvatarEntity || traitType == AvatarTraits::Grab ||
                traitType == AvatarTraits::AvatarMenuParameters) {
                // a new id is taken within the limits for its kind and the rate (security #52)
                auto& instances = _lastReceivedTraitVersions.getInstanceIDValuePairs(traitType);
                auto listed = std::find_if(instances.begin(), instances.end(),
                                           [&instanceID](const auto& pair) { return pair.id == instanceID; });
                if (listed == instances.end()) {
                    const auto& limits = traitType == AvatarTraits::AvatarEntity ? avatar_mixer_checks::AVATAR_ENTITY_LIMITS
                        : traitType == AvatarTraits::Grab ? avatar_mixer_checks::GRAB_LIMITS
                        : avatar_mixer_checks::MENU_PARAMETERS_LIMITS;
                    const int live = (int)std::count_if(instances.begin(), instances.end(),
                                                        [](const auto& pair) { return pair.value > 0; });
                    QString refusal = avatar_mixer_checks::newInstanceRefusal(live, (int)instances.size(),
                        traitSize == AvatarTraits::DELETED_TRAIT_SIZE, limits);
                    if (refusal.isEmpty() && !_newInstanceRate.take(usecTimestampNow())) {
                        refusal = QString("new ids faster than %1 a second").arg(avatar_mixer_checks::NewInstanceRate::PER_SECOND);
                    }
                    if (!refusal.isEmpty()) {
                        // not listed, not passed on; the app's next version of it is judged again
                        logRefusedData(traitType == AvatarTraits::AvatarEntity ? "an avatar entity"
                                       : traitType == AvatarTraits::Grab ? "a grab" : "menu parameters",
                                       refusal, sendingNode.getUUID());
                        message.seek(message.getPosition() + std::max<qint64>(traitSize, 0));
                        continue;
                    }
                }
                auto& instanceVersionRef = listed != instances.end()
                    ? listed->value : _lastReceivedTraitVersions.getInstanceValueRef(traitType, instanceID);

                if (packetTraitVersion > instanceVersionRef) {
                    if (traitSize == AvatarTraits::DELETED_TRAIT_SIZE) {
                        _refusedCarriedEntities.erase(instanceID);
                        _avatar->processDeletedTraitInstance(traitType, instanceID);
                        // Mixer doesn't need deleted IDs.
                        _avatar->getAndClearRecentlyRemovedIDs();

                        // to track a deleted instance but keep version information
                        // the avatar mixer uses the negative value of the sent version
                        instanceVersionRef = -packetTraitVersion;
                    } else {
                        QByteArray traitData = message.read(traitSize);
                        CarriedRefusal refusal;
                        if (traitType == AvatarTraits::AvatarEntity) {
                            refusal = refusalForCarriedEntity(traitData);
                        }
                        if (!refusal.reason.isEmpty()) {
                            static const size_t MAX_REFUSALS_REMEMBERED = 256;  // many ids only log again
                            if (_refusedCarriedEntities.size() >= MAX_REFUSALS_REMEMBERED) {
                                _refusedCarriedEntities.clear();
                            }
                            // the reason, not the value: a value changed on every update still logs once
                            QString& lastRefusal = _refusedCarriedEntities[instanceID];
                            if (lastRefusal != refusal.reason) {
                                qCDebug(avatars) << "Avatar" << sendingNode.getUUID() << "carries entity" << instanceID
                                    << "with" << refusal.reason << refusal.value << "; not passed on";
                                lastRefusal = refusal.reason;
                            }
                            // nobody else sees it: taken away, as a deleted one, until a version that passes
                            _avatar->processDeletedTraitInstance(traitType, instanceID);
                            _avatar->getAndClearRecentlyRemovedIDs();
                            instanceVersionRef = -packetTraitVersion;
                        } else {
                            _refusedCarriedEntities.erase(instanceID);
                            _avatar->processTraitInstance(traitType, instanceID, traitData);
                            instanceVersionRef = packetTraitVersion;
                        }
                    }

                    anyTraitsChanged = true;
                } else {
                    // an older version: skipped (a delete has no data: its size, -1, isn't a step back)
                    message.seek(message.getPosition() + std::max<qint64>(traitSize, 0));
                }
            } else {
                logRefusedData("a traits packet", "an instance of a kind the mixer doesn't take", sendingNode.getUUID());
                break;
            }
        }
    }

    if (anyTraitsChanged) {
        _lastReceivedTraitsChange = std::chrono::steady_clock::now();
    }
}

void AvatarMixerClientData::processBulkAvatarTraitsAckMessage(ReceivedMessage& message) {
    // Avatar Traits flow control marks each outgoing avatar traits packet with a
    // sequence number. The mixer caches the traits sent in the traits packet.
    // Until an ack with the sequence number comes back, all updates to _traits
    // in that packet_ are ignored.  Updates to traits not in that packet will
    // be sent.

    // Look up the avatar/trait data associated with this ack and update the 'last ack' list
    // with it.
    AvatarTraits::TraitMessageSequence seq;
    message.readPrimitive(&seq);
    auto sentAvatarTraitVersions = _perNodePendingTraitVersions.find(seq);
    if (sentAvatarTraitVersions != _perNodePendingTraitVersions.end()) {
        for (auto& perNodeTraitVersions : sentAvatarTraitVersions->second) {
            auto& nodeId = perNodeTraitVersions.first;
            auto& traitVersions = perNodeTraitVersions.second;
            // For each trait that was sent in the traits packet,
            // update the 'acked' trait version.  Traits not
            // sent in the traits packet keep their version.

            // process simple traits
            auto simpleReceivedIt = traitVersions.simpleCBegin();
            while (simpleReceivedIt != traitVersions.simpleCEnd()) {
                if (*simpleReceivedIt != AvatarTraits::DEFAULT_TRAIT_VERSION) {
                    auto traitType =
                        static_cast<AvatarTraits::TraitType>(std::distance(traitVersions.simpleCBegin(), simpleReceivedIt));
                    _perNodeAckedTraitVersions[nodeId][traitType] = *simpleReceivedIt;
                }
                simpleReceivedIt++;
            }

            // process instanced traits
            auto instancedSentIt = traitVersions.instancedCBegin();
            while (instancedSentIt != traitVersions.instancedCEnd()) {
                auto traitType = instancedSentIt->traitType;

                for (auto& sentInstance : instancedSentIt->instances) {
                    auto instanceID = sentInstance.id;
                    const auto sentVersion = sentInstance.value;
                    _perNodeAckedTraitVersions[nodeId].instanceInsert(traitType, instanceID, sentVersion);
                }
                instancedSentIt++;
            }
        }
        _perNodePendingTraitVersions.erase(sentAvatarTraitVersions);
    } else {
        // This can happen either the BulkAvatarTraits was sent with no simple traits,
        // or if the avatar mixer restarts while there are pending
        // BulkAvatarTraits messages in-flight.
        if (seq > getTraitsMessageSequence()) {
            qWarning() << "Received BulkAvatarTraitsAck with future seq (potential avatar mixer restart) " << seq << " from "
                       << message.getSenderSockAddr();
        }
    }
}

uint64_t AvatarMixerClientData::getLastBroadcastTime(NLPacket::LocalID nodeUUID) const {
    // return the matching PacketSequenceNumber, or the default if we don't have it
    auto nodeMatch = _lastBroadcastTimes.find(nodeUUID);
    if (nodeMatch != _lastBroadcastTimes.end()) {
        return nodeMatch->second;
    }
    return 0;
}

uint16_t AvatarMixerClientData::getLastBroadcastSequenceNumber(NLPacket::LocalID nodeID) const {
    // return the matching PacketSequenceNumber, or the default if we don't have it
    auto nodeMatch = _lastBroadcastSequenceNumbers.find(nodeID);
    if (nodeMatch != _lastBroadcastSequenceNumbers.end()) {
        return nodeMatch->second;
    }
    return 0;
}

void AvatarMixerClientData::ignoreOther(SharedNodePointer self, SharedNodePointer other) {
    ignoreOther(self.data(), other.data());
}

void AvatarMixerClientData::ignoreOther(const Node* self, const Node* other) {
    if (!isRadiusIgnoring(other->getUUID())) {
        addToRadiusIgnoringSet(other->getUUID());
        auto killPacket = NLPacket::create(PacketType::KillAvatar, NUM_BYTES_RFC4122_UUID + sizeof(KillAvatarReason), true);
        killPacket->write(other->getUUID().toRfc4122());
        if (_isIgnoreRadiusEnabled) {
            killPacket->writePrimitive(KillAvatarReason::TheirAvatarEnteredYourBubble);
        } else {
            killPacket->writePrimitive(KillAvatarReason::YourAvatarEnteredTheirBubble);
        }
        setLastBroadcastTime(other->getLocalID(), 0);

        resetSentTraitData(other->getLocalID());

        DependencyManager::get<NodeList>()->sendPacket(std::move(killPacket), *self);
    }
}

bool AvatarMixerClientData::isRadiusIgnoring(const QUuid& other) const {
    return std::find(_radiusIgnoredOthers.cbegin(), _radiusIgnoredOthers.cend(), other) != _radiusIgnoredOthers.cend();
}

void AvatarMixerClientData::addToRadiusIgnoringSet(const QUuid& other) {
    if (!isRadiusIgnoring(other)) {
        _radiusIgnoredOthers.push_back(other);
    }
}

void AvatarMixerClientData::removeFromRadiusIgnoringSet(const QUuid& other) {
    auto ignoredOtherIter = std::find(_radiusIgnoredOthers.cbegin(), _radiusIgnoredOthers.cend(), other);
    if (ignoredOtherIter != _radiusIgnoredOthers.cend()) {
        _radiusIgnoredOthers.erase(ignoredOtherIter);
    }
}

void AvatarMixerClientData::logRefusedData(const QString& what, const QString& reason, const QUuid& sender) {
    static const quint64 LOG_INTERVAL_USECS = 60 * USECS_PER_SECOND;
    const quint64 now = usecTimestampNow();
    const int refused = ++_refusedDataSinceLog;
    quint64 last = _lastRefusedDataLogUsecs.load();
    if ((last != 0 && now - last < LOG_INTERVAL_USECS) || !_lastRefusedDataLogUsecs.compare_exchange_strong(last, now)) {
        return;
    }
    _refusedDataSinceLog -= refused;
    qCWarning(avatars) << "Avatar" << sender << "sent" << what << "the mixer dropped:" << reason
                       << QString("(%1 dropped from it since the last line)").arg(refused);
}

void AvatarMixerClientData::resetSentTraitData(Node::LocalID nodeLocalID) {
    _lastSentTraitsTimestamps[nodeLocalID] = TraitsCheckTimestamp();
    _perNodeSentTraitVersions[nodeLocalID].reset();
    _perNodeAckedTraitVersions[nodeLocalID].reset();
    for (auto&& pendingTraitVersions : _perNodePendingTraitVersions) {
        pendingTraitVersions.second[nodeLocalID].reset();
    }
}

void AvatarMixerClientData::readViewFrustumPacket(const QByteArray& message) {
    _currentViewFrustums.clear();

    auto sourceBuffer = reinterpret_cast<const unsigned char*>(message.constData());

    uint8_t numFrustums = 0;
    memcpy(&numFrustums, sourceBuffer, sizeof(numFrustums));
    sourceBuffer += sizeof(numFrustums);

    for (uint8_t i = 0; i < numFrustums; ++i) {
        ConicalViewFrustum frustum;
        sourceBuffer += frustum.deserialize(sourceBuffer);

        _currentViewFrustums.push_back(frustum);
    }
}

bool AvatarMixerClientData::otherAvatarInView(const AABox& otherAvatarBox) {
    return std::any_of(std::begin(_currentViewFrustums), std::end(_currentViewFrustums),
                       [&](const ConicalViewFrustum& viewFrustum) { return viewFrustum.intersects(otherAvatarBox); });
}

void AvatarMixerClientData::loadJSONStats(QJsonObject& jsonObject) const {
    jsonObject["display_name"] = _avatar->getDisplayName();
    jsonObject["num_avs_sent_last_frame"] = _numAvatarsSentLastFrame;
    jsonObject["avg_other_av_starves_per_second"] = getAvgNumOtherAvatarStarvesPerSecond();
    jsonObject["avg_other_av_skips_per_second"] = getAvgNumOtherAvatarSkipsPerSecond();
    jsonObject["total_num_out_of_order_sends"] = _numOutOfOrderSends;

    jsonObject[OUTBOUND_AVATAR_DATA_STATS_KEY] = getOutboundAvatarDataKbps();
    jsonObject[OUTBOUND_AVATAR_TRAITS_STATS_KEY] = getOutboundAvatarTraitsKbps();
    jsonObject[INBOUND_AVATAR_DATA_STATS_KEY] = _avatar->getAverageBytesReceivedPerSecond() / (float)BYTES_PER_KILOBIT;

    jsonObject["av_data_receive_rate"] = _avatar->getReceiveRate();
    jsonObject["recent_other_av_in_view"] = _recentOtherAvatarsInView;
    jsonObject["recent_other_av_out_of_view"] = _recentOtherAvatarsOutOfView;
}

AvatarMixerClientData::TraitsCheckTimestamp AvatarMixerClientData::getLastOtherAvatarTraitsSendPoint(
    Node::LocalID otherAvatar) const {
    auto it = _lastSentTraitsTimestamps.find(otherAvatar);

    if (it != _lastSentTraitsTimestamps.end()) {
        return it->second;
    } else {
        return TraitsCheckTimestamp();
    }
}

void AvatarMixerClientData::cleanupKilledNode(const QUuid&, Node::LocalID nodeLocalID) {
    removeLastBroadcastSequenceNumber(nodeLocalID);
    removeLastBroadcastTime(nodeLocalID);
    _lastSentTraitsTimestamps.erase(nodeLocalID);
    _perNodeSentTraitVersions.erase(nodeLocalID);
    _perNodeAckedTraitVersions.erase(nodeLocalID);
    for (auto&& pendingTraitVersions : _perNodePendingTraitVersions) {
        pendingTraitVersions.second.erase(nodeLocalID);
    }
}
