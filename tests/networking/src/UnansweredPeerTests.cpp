//
//  UnansweredPeerTests.cpp
//  tests/networking/src
//
//  Created by servers on 2026-11-03.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "UnansweredPeerTests.h"

#include <cstring>

#include <QtCore/QElapsedTimer>
#include <QtNetwork/QUdpSocket>

#include <DependencyManager.h>
#include <LimitedNodeList.h>
#include <NodeList.h>
#include <StatTracker.h>
#include <udt/ControlPacket.h>
#include <udt/Packet.h>
#include <udt/Socket.h>

QTEST_MAIN(UnansweredPeerTests)

void UnansweredPeerTests::initTestCase() {
    DependencyManager::set<StatTracker>();
    DependencyManager::registerInheritance<LimitedNodeList, NodeList>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
}

// The domain server answers a path query only from an address findNodeWithAddr knows: a node's public, local or
// symmetric socket, nothing else (DomainServer::processPathQueryPacket)
void UnansweredPeerTests::onlyANodesAddressIsANode() {
    auto nodeList = DependencyManager::get<LimitedNodeList>();
    const HifiSockAddr publicSocket(QHostAddress("203.0.113.7"), 40000);
    const HifiSockAddr localSocket(QHostAddress("192.168.1.20"), 40001);
    nodeList->addOrUpdateNode(QUuid::createUuid(), NodeType::Agent, publicSocket, localSocket);

    QVERIFY(nodeList->findNodeWithAddr(publicSocket));
    QVERIFY(nodeList->findNodeWithAddr(localSocket));
    QVERIFY(!nodeList->findNodeWithAddr(HifiSockAddr(QHostAddress("203.0.113.7"), 40002)));   // another port
    QVERIFY(!nodeList->findNodeWithAddr(HifiSockAddr(QHostAddress("203.0.113.8"), 40000)));   // another address
    QVERIFY(!nodeList->findNodeWithAddr(HifiSockAddr(QHostAddress::LocalHost, 40000)));
}

// A reliable packet to a peer that never answers: the handshake is sent ~10 times a second, then given up after 10 s
// (SendQueue::run). The peer is our own silent socket.
void UnansweredPeerTests::anUnansweredHandshakeIsGivenUp() {
    QUdpSocket silent;
    QVERIFY(silent.bind(QHostAddress(QHostAddress::LocalHost), 0));
    udt::Socket socket(nullptr, false);
    socket.bind(QHostAddress(QHostAddress::LocalHost), 0);
    const HifiSockAddr peer(QHostAddress::LocalHost, silent.localPort());

    socket.writePacket(udt::Packet::create(-1, true), peer);

    int whileTrying = 0;
    int afterGivingUp = 0;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        while (silent.hasPendingDatagrams()) {
            const qint64 size = silent.pendingDatagramSize();
            std::unique_ptr<char[]> data(new char[size]);
            silent.readDatagram(data.get(), size);
            auto control = udt::ControlPacket::fromReceivedPacket(std::move(data), size, peer);
            if (!control || control->getType() != udt::ControlPacket::Handshake) {
                continue;
            }
            const qint64 at = clock.elapsed();
            if (at < 9500) {
                whileTrying++;
            } else if (at > 11000) {
                afterGivingUp++;
            }
        }
    }
    QVERIFY2(whileTrying >= 50, qPrintable(QString("%1 handshakes in the first 9.5 s").arg(whileTrying)));
    QVERIFY2(afterGivingUp == 0, qPrintable(QString("%1 handshakes after 11 s").arg(afterGivingUp)));
}
