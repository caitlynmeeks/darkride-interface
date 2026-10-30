//
//  OctreePacketProcessor.cpp
//  interface/src/octree
//
//  Created by Brad Hefta-Gaub on 8/12/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "OctreePacketProcessor.h"

#include <chrono>

#include <PerfStat.h>

#include "Application.h"
#include "Menu.h"
#include "SceneScriptingInterface.h"

OctreePacketProcessor::OctreePacketProcessor():
    _safeLanding(new SafeLanding())
{
    setObjectName("Octree Packet Processor");

    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    const PacketReceiver::PacketTypeList octreePackets =
        { PacketType::OctreeStats, PacketType::EntityData, PacketType::EntityErase, PacketType::EntityQueryInitialResultsComplete };
    packetReceiver.registerDirectListenerForTypes(octreePackets, this, "handleOctreePacket");
}

OctreePacketProcessor::~OctreePacketProcessor() { }

void OctreePacketProcessor::handleOctreePacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode) {
    queueReceivedPacket(message, senderNode);
}

void OctreePacketProcessor::processPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer sendingNode) {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings),
                            "OctreePacketProcessor::processPacket()");

#ifndef Q_OS_ANDROID
    const int WAY_BEHIND = 300;

    if (packetsToProcessCount() > WAY_BEHIND && qApp->getLogger()->extraDebugging()) {
        qDebug("OctreePacketProcessor::processPacket() packets to process=%d", packetsToProcessCount());
    }
#endif
    
    bool wasStatsPacket = false;

    PacketType octreePacketType = message->getType();

    // note: PacketType_OCTREE_STATS can have PacketType_VOXEL_DATA
    // immediately following them inside the same packet. So, we process the PacketType_OCTREE_STATS first
    // then process any remaining bytes as if it was another packet
    if (octreePacketType == PacketType::OctreeStats) {
        int statsMessageLength = qApp->getDomainSession().processOctreeStats(*message, sendingNode);

        wasStatsPacket = true;
        int piggybackBytes = message->getSize() - statsMessageLength;
        
        if (statsMessageLength >= 0 && piggybackBytes > 0) {
            // construct a new packet from the piggybacked one
            auto buffer = std::unique_ptr<char[]>(new char[piggybackBytes]);
            memcpy(buffer.get(), message->getRawMessage() + statsMessageLength, piggybackBytes);
            auto newPacket = NLPacket::fromReceivedPacket(std::move(buffer), piggybackBytes, message->getSenderSockAddr());
            if (!newPacket) {
                return; // too short for a packet's header: nothing to read
            }
            message = QSharedPointer<ReceivedMessage>::create(*newPacket);
        } else {
            // Note... stats packets don't have sequence numbers, so we don't want to send those to trackIncomingVoxelPacket()
            return; // bail since no piggyback data
        }
    } // fall through to piggyback message

    PacketType packetType = message->getType();

    // check version of piggyback packet against expected version
    if (message->getVersion() != versionForPacketType(message->getType())) {
        static QMultiMap<QUuid, PacketType> versionDebugSuppressMap;

        const QUuid& senderUUID = sendingNode->getUUID();
        if (!versionDebugSuppressMap.contains(senderUUID, packetType)) {
            qDebug() << "Was stats packet? " << wasStatsPacket;
            qDebug() << "OctreePacketProcessor - piggyback packet version mismatch on" << packetType << "- Sender"
                << senderUUID << "sent" << (int) message->getVersion() << "but"
                << (int) versionForPacketType(packetType) << "expected.";

            emit packetVersionMismatch();

            versionDebugSuppressMap.insert(senderUUID, packetType);
        }
        return; // bail since piggyback version doesn't match
    }

    if (packetType != PacketType::EntityQueryInitialResultsComplete) {
        qApp->getDomainSession().trackIncomingOctreePacket(*message, sendingNode, wasStatsPacket);
    }
    
    // seek back to beginning of packet after tracking
    message->seek(0);

    // The server's sequence number of this packet: entity data and erase packets both carry one (the erase packets
    // consume numbers too, so the safe landing's window must count them), the completion carries the number after the
    // last packet of the scene
    OCTREE_PACKET_SEQUENCE sequence = 0;
    bool hasSequence = false;
    if (packetType == PacketType::EntityQueryInitialResultsComplete) {
        message->readPrimitive(&sequence);
        message->seek(0);
    } else if (message->getSize() >= (int)(sizeof(OCTREE_PACKET_FLAGS) + sizeof(OCTREE_PACKET_SEQUENCE))) {
        message->seek(sizeof(OCTREE_PACKET_FLAGS));
        message->readPrimitive(&sequence);
        message->seek(0);
        hasSequence = true;
    }
    bool landingTracks = _safeLanding && _safeLanding->isTracking();
    if (landingTracks && hasSequence) {
        _safeLanding->addToSequence(sequence);
    }

    // TIVOLI_ARRIVAL_TRACE=1: a line per octree packet while the safe landing is tracking (WORLD-LOADING.md, the
    // arrival gate): its type, the server's sequence number, its size and how long it waited in this processor's queue
    static const bool arrivalTrace = !qgetenv("TIVOLI_ARRIVAL_TRACE").isEmpty();
    if (arrivalTrace && landingTracks) {
        qint64 received = message->getFirstPacketReceiveTime();   // microseconds of the high-resolution clock, 0 for a piggyback
        qint64 queuedUs = received > 0 ? (qint64)std::chrono::duration_cast<std::chrono::microseconds>(
            p_high_resolution_clock::now().time_since_epoch()).count() - received : -1;
        qDebug() << "arrival trace:" << packetType << "sequence" << sequence << "bytes" << message->getSize()
            << "queued" << (queuedUs >= 0 ? QString::number(queuedUs / 1000.0, 'f', 1) : QString("?")) << "ms"
            << (wasStatsPacket ? "piggybacked on stats" : "") << "backlog" << packetsToProcessCount();
    }

    switch(packetType) {
        case PacketType::EntityErase: {
            if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderEntities()) {
                auto renderer = qApp->getEntities();
                if (renderer) {
                    renderer->processEraseMessage(*message, sendingNode);
                }
            }
        } break;

        case PacketType::EntityData: {
            if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderEntities()) {
                auto renderer = qApp->getEntities();
                if (renderer) {
                    renderer->processDatagram(*message, sendingNode);
                }
            }
        } break;

        case PacketType::EntityQueryInitialResultsComplete: {
            // the number after the last packet of the initial scene; the landing's window starts at the first packet it
            // saw since it began tracking (an earlier landing's packets do not count: until 2026-10-19 the start was
            // kept here across landings, so a landing restarted by the arrival's teleport waited for packets that had
            // been seen, and consumed, before it began, and the loading screen held to its time limit)
            if (landingTracks) {
                _safeLanding->finishSequence(sequence);
            }
        } break;

        default: {
            // nothing to do
        } break;
    }
}

void OctreePacketProcessor::startSafeLanding() {
    if (_safeLanding) {
        _safeLanding->startTracking(qApp->getEntities());
    }
}

void OctreePacketProcessor::updateSafeLanding() {
    if (_safeLanding) {
        _safeLanding->updateTracking();
    }
}

void OctreePacketProcessor::stopSafeLanding() {
    if (_safeLanding) {
        _safeLanding->stopTracking();
    }
}

void OctreePacketProcessor::resetSafeLanding() {
    if (_safeLanding) {
        _safeLanding->reset();
    }
}

bool OctreePacketProcessor::safeLandingIsActive() const {
    return _safeLanding && _safeLanding->isTracking();
}

bool OctreePacketProcessor::safeLandingIsComplete() const {
    if (_safeLanding) {
        return _safeLanding->trackingIsComplete();
    }
    return false;
}
