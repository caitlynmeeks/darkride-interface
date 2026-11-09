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
#include <algorithm>
#include <QtCore/QSet>

#include <QtCore/QElapsedTimer>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QUdpSocket>

#include <AssetClient.h>
#include <AssetUpload.h>
#include <DependencyManager.h>
#include <LimitedNodeList.h>
#include <NodeList.h>
#include <SharedUtil.h>
#include <StatTracker.h>
#include <udt/ControlPacket.h>
#include <udt/Packet.h>
#include <udt/SendQueue.h>
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
// (SendQueue::run). The peer is our own silent socket. Timed from the first handshake, not the test's start, and the
// count only has to show it kept trying: on a loaded machine the queue may start late and resend less often (release,
// 11-03: 33 in 9.5 s at a load of ~30).
void UnansweredPeerTests::anUnansweredHandshakeIsGivenUp() {
    QUdpSocket silent;
    QVERIFY(silent.bind(QHostAddress(QHostAddress::LocalHost), 0));
    udt::Socket socket(nullptr, false);
    socket.bind(QHostAddress(QHostAddress::LocalHost), 0);
    const HifiSockAddr peer(QHostAddress::LocalHost, silent.localPort());

    socket.writePacket(udt::Packet::create(-1, true), peer);

    int handshakes = 0;
    qint64 first = -1;
    qint64 last = -1;
    QElapsedTimer clock;
    clock.start();
    // watch 15 s from the first handshake (or 30 s if none comes)
    while (clock.elapsed() < (first < 0 ? 30000 : first + 15000)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        while (silent.hasPendingDatagrams()) {
            const qint64 size = silent.pendingDatagramSize();
            std::unique_ptr<char[]> data(new char[size]);
            silent.readDatagram(data.get(), size);
            auto control = udt::ControlPacket::fromReceivedPacket(std::move(data), size, peer);
            if (!control || control->getType() != udt::ControlPacket::Handshake) {
                continue;
            }
            last = clock.elapsed();
            if (first < 0) {
                first = last;
            }
            handshakes++;
        }
    }
    QVERIFY2(handshakes >= 5, qPrintable(QString("%1 handshakes").arg(handshakes)));
    QVERIFY2(last - first <= 11000,
             qPrintable(QString("handshakes for %1 ms after the first (given up after 10 s)").arg(last - first)));
}

namespace {
QStringList givenUpLines;
QtMessageHandler previousHandler = nullptr;
void recordGiveUps(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (message.contains("No handshake answer from")) {
        givenUpLines << message;
    }
    if (previousHandler) {
        previousHandler(type, context, message);
    }
}
}

// physics' question about the 11-03 abort (servers, 11-04): a send queue given up after 10 s without a handshake
// answer, while its producer keeps queuing reliable packets into it, during the give-up and after (as the physics
// server's train states would). Run it alone (`networking-UnansweredPeerTests aGiveUpWithPacketsStillComing`, so the
// once-a-minute warning comes; the test counts on the queue's own counters either way) and under a malloc that traps a freed block's reuse
// (macOS: DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib): it must give up with packets waiting, more than once, and
// nothing may crash.
void UnansweredPeerTests::aGiveUpWithPacketsStillComing() {
    QUdpSocket silent;
    QVERIFY(silent.bind(QHostAddress(QHostAddress::LocalHost), 0));
    udt::Socket socket(nullptr, false);
    socket.bind(QHostAddress(QHostAddress::LocalHost), 0);
    const HifiSockAddr peer(QHostAddress::LocalHost, silent.localPort());

    givenUpLines.clear();
    previousHandler = qInstallMessageHandler(recordGiveUps);
    const quint64 givenUpBefore = udt::SendQueue::handshakesGivenUp();
    const quint64 withPacketsBefore = udt::SendQueue::handshakesGivenUpWithPacketsWaiting();

    int sent = 0;
    QList<qint64> giveUpTimes;       // when each give-up was seen (ms)
    QList<qint64> handshakeTimes;    // when the silent peer got a handshake (ms)
    QElapsedTimer clock;
    clock.start();
    QElapsedTimer sinceSend;
    sinceSend.start();
    while (clock.elapsed() < 25000) {
        if (sinceSend.elapsed() >= 10) {
            // a reliable packet every 10 ms, the physics server's train states at their fastest
            socket.writePacket(udt::Packet::create(-1, true), peer);
            sent++;
            sinceSend.restart();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        while (giveUpTimes.size() < int(udt::SendQueue::handshakesGivenUp() - givenUpBefore)) {
            giveUpTimes << clock.elapsed();
        }
        while (silent.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = silent.receiveDatagram();
            const QByteArray bytes = datagram.data();
            std::unique_ptr<char[]> data(new char[bytes.size()]);
            memcpy(data.get(), bytes.constData(), bytes.size());
            auto control = udt::ControlPacket::fromReceivedPacket(std::move(data), bytes.size(), peer);
            if (control && control->getType() == udt::ControlPacket::Handshake) {
                handshakeTimes << clock.elapsed();
            }
        }
    }
    qInstallMessageHandler(previousHandler);

    QVERIFY2(sent > 1000, qPrintable(QString("%1 packets sent").arg(sent)));
    // every give-up had packets waiting (counted by the queue, since the warning is logged once a minute and another
    // test's line may hold it back; when a line does come, it says so too)
    const quint64 withPackets = udt::SendQueue::handshakesGivenUpWithPacketsWaiting() - withPacketsBefore;
    QVERIFY2(withPackets == quint64(giveUpTimes.size()),
             qPrintable(QString("%1 of %2 give-ups had packets waiting").arg(withPackets).arg(giveUpTimes.size())));
    for (const auto& line : givenUpLines) {
        QVERIFY2(line.contains("with packets waiting"), qPrintable(line));
    }
    // physics: the swap itself, not only no crash: given up twice, about 10 s apart, and after each give-up the
    // producer's next packet opened a new queue, which sent a new handshake within a second
    QVERIFY2(giveUpTimes.size() >= 2, qPrintable(QString("%1 give-ups in 25 s").arg(giveUpTimes.size())));
    QVERIFY2(giveUpTimes[0] > 9500 && giveUpTimes[0] < 12000, qPrintable(QString("first at %1 ms").arg(giveUpTimes[0])));
    const qint64 apart = giveUpTimes[1] - giveUpTimes[0];
    QVERIFY2(apart > 9500 && apart < 12000, qPrintable(QString("%1 ms apart").arg(apart)));
    for (qint64 at : giveUpTimes) {
        const bool newHandshake = std::any_of(handshakeTimes.begin(), handshakeTimes.end(),
                                              [at](qint64 t) { return t > at && t < at + 1000; });
        QVERIFY2(newHandshake, qPrintable(QString("no new handshake within 1 s of the give-up at %1 ms").arg(at)));
    }
}

// security's addition: the peer answers late. A relay between them holds everything sent to the peer for the first
// 12 s, then lets it through (and the peer's answers back, always): the first queue's handshake reaches the peer and
// its answer comes back after that queue was given up, while the producer keeps sending. The stale answer must hurt
// nothing (under a malloc that traps a freed block's reuse), and what is sent from 14 s on must arrive.
void UnansweredPeerTests::aLateAnswerAfterTheGiveUp() {
    udt::Socket receiver(nullptr, false);
    receiver.bind(QHostAddress(QHostAddress::LocalHost), 0);
    QSet<quint32> received;
    receiver.setPacketHandler([&received](std::unique_ptr<udt::Packet> packet) {
        quint32 index = 0;
        if (packet->readPrimitive(&index) == sizeof(index)) {
            received.insert(index);
        }
    });
    QUdpSocket relay;
    QVERIFY(relay.bind(QHostAddress(QHostAddress::LocalHost), 0));
    const HifiSockAddr relayAddress(QHostAddress::LocalHost, relay.localPort());
    udt::Socket sender(nullptr, false);
    sender.bind(QHostAddress(QHostAddress::LocalHost), 0);
    const quint16 senderPort = sender.localPort();

    const quint64 givenUpBefore = udt::SendQueue::handshakesGivenUp();
    QList<QByteArray> held;
    quint32 next = 0;
    quint32 firstAfter14 = 0;
    QElapsedTimer clock;
    clock.start();
    QElapsedTimer sinceSend;
    sinceSend.start();
    while (clock.elapsed() < 25000) {
        if (sinceSend.elapsed() >= 10) {
            auto packet = udt::Packet::create(-1, true);
            packet->writePrimitive(next++);
            sender.writePacket(std::move(packet), relayAddress);
            sinceSend.restart();
            if (firstAfter14 == 0 && clock.elapsed() >= 14000) {
                firstAfter14 = next - 1;
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const bool holding = clock.elapsed() < 12000;
        if (!holding && !held.isEmpty()) {
            for (const auto& bytes : held) {
                relay.writeDatagram(bytes, QHostAddress::LocalHost, receiver.localPort());
            }
            held.clear();
        }
        while (relay.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = relay.receiveDatagram();
            if (datagram.senderPort() == senderPort) {
                if (holding) {
                    held << datagram.data();
                } else {
                    relay.writeDatagram(datagram.data(), QHostAddress::LocalHost, receiver.localPort());
                }
            } else {
                relay.writeDatagram(datagram.data(), QHostAddress::LocalHost, senderPort);
            }
        }
    }

    QVERIFY2(udt::SendQueue::handshakesGivenUp() > givenUpBefore, "the first queue wasn't given up");
    QVERIFY(firstAfter14 > 0);
    int arrived = 0;
    for (quint32 index = firstAfter14; index < next; index++) {
        arrived += received.contains(index) ? 1 : 0;
    }
    const int expected = int(next - firstAfter14);
    QVERIFY2(arrived >= expected * 9 / 10,
             qPrintable(QString("%1 of the %2 packets sent from 14 s on arrived").arg(arrived).arg(expected)));
}

// A packet that passed the hash check came from one of the node's two known addresses: that address is used at once,
// before any ping is answered, so a one-off answer (the asset server's reply to an upload) isn't dropped. Any other
// address (a symmetric NAT's) is left to the pings, and a node with an active socket keeps it.
void UnansweredPeerTests::aVerifiedPacketsAddressIsUsedBeforeAnyPing() {
    const HifiSockAddr publicSocket(QHostAddress("203.0.113.9"), 41000);
    const HifiSockAddr localSocket(QHostAddress("192.168.1.30"), 41001);
    const auto packetType = PacketType::AssetUpload;

    Node fromLocal(QUuid::createUuid(), NodeType::Agent, publicSocket, localSocket);
    QVERIFY(!fromLocal.getActiveSocket());
    QVERIFY(LimitedNodeList::activateSocketFromVerifiedPacket(fromLocal, localSocket, packetType));
    QVERIFY(fromLocal.getActiveSocket() == &fromLocal.getLocalSocket());

    Node fromPublic(QUuid::createUuid(), NodeType::Agent, publicSocket, localSocket);
    QVERIFY(LimitedNodeList::activateSocketFromVerifiedPacket(fromPublic, publicSocket, packetType));
    QVERIFY(fromPublic.getActiveSocket() == &fromPublic.getPublicSocket());

    Node fromElsewhere(QUuid::createUuid(), NodeType::Agent, publicSocket, localSocket);
    QVERIFY(!LimitedNodeList::activateSocketFromVerifiedPacket(fromElsewhere,
                                                               HifiSockAddr(QHostAddress("203.0.113.9"), 41002), packetType));
    QVERIFY(!LimitedNodeList::activateSocketFromVerifiedPacket(fromElsewhere,
                                                               HifiSockAddr(QHostAddress("198.51.100.1"), 41000), packetType));
    QVERIFY(!fromElsewhere.getActiveSocket());

    // already answering at its public address: a packet from its local one changes nothing here (pings decide that)
    QVERIFY(!LimitedNodeList::activateSocketFromVerifiedPacket(fromPublic, localSocket, packetType));
    QVERIFY(fromPublic.getActiveSocket() == &fromPublic.getPublicSocket());

    // a node whose local address the domain server left blank (a loopback node's public one is blanked the same way)
    Node noLocal(QUuid::createUuid(), NodeType::Agent, publicSocket, HifiSockAddr());
    QVERIFY(!LimitedNodeList::activateSocketFromVerifiedPacket(noLocal, HifiSockAddr(), packetType));
    QVERIFY(LimitedNodeList::activateSocketFromVerifiedPacket(noLocal, publicSocket, packetType));
    QVERIFY(noLocal.getActiveSocket() == &noLocal.getPublicSocket());
}

// An upload or mapping operation fails as a network error when no answer has come 60 s after both its sending and
// the asset server's last answer of any kind; an upload gets a second per 16 KB on top.
void UnansweredPeerTests::anUnansweredUploadOrMappingHasADeadline() {
    using namespace std::chrono;
    const AssetClient::Clock::time_point sentAt { seconds(1000) };

    // nothing answered since the sending
    QVERIFY(AssetClient::answerDeadline(sentAt, sentAt - seconds(30), 0) == sentAt + seconds(60));
    QVERIFY(AssetClient::answerDeadline(sentAt, AssetClient::Clock::time_point(), 0) == sentAt + seconds(60));

    // the asset server answered something else later: the wait starts again from there
    QVERIFY(AssetClient::answerDeadline(sentAt, sentAt + seconds(45), 0) == sentAt + seconds(105));

    // an upload: a second for every 16 KB
    QVERIFY(AssetClient::answerDeadline(sentAt, sentAt, 54) == sentAt + seconds(60) + milliseconds(54 * 1000 / (16 * 1024)));
    QVERIFY(AssetClient::answerDeadline(sentAt, sentAt, 10 * 16 * 1024) == sentAt + seconds(70));
    QVERIFY(AssetClient::answerDeadline(sentAt, sentAt, 100LL * 1024 * 1024) == sentAt + seconds(60) + seconds(6400));
}

// The forced case: an asset server that is alive (heard from all the time, as its pings are in the bug) but never
// answers the upload (our own silent socket). The upload fails as a network error between 60 and 66 s (the deadline,
// then the next 5-s check), instead of waiting forever. (A wholly silent server is removed as a silent node after
// ~15 s and fails its requests then, as before.) Run it alone: ~66 s.
void UnansweredPeerTests::anUnansweredUploadFailsAtItsDeadline() {
    QUdpSocket silent;
    QVERIFY(silent.bind(QHostAddress(QHostAddress::LocalHost), 0));
    const HifiSockAddr server(QHostAddress::LocalHost, silent.localPort());

    auto nodeList = DependencyManager::get<LimitedNodeList>();
    auto assetServer = nodeList->addOrUpdateNode(QUuid::createUuid(), NodeType::AssetServer, server, server);
    assetServer->activatePublicSocket();
    auto assetClient = DependencyManager::set<AssetClient>();

    auto upload = assetClient->createUpload(QByteArray("// nobody answers this upload\n"));
    bool finished = false;
    AssetUpload::Error error = AssetUpload::NoError;
    qint64 finishedAt = 0;
    QElapsedTimer clock;
    connect(upload, &AssetUpload::finished, this, [&](AssetUpload* done, const QString&) {
        finished = true;
        error = done->getError();
        finishedAt = clock.elapsed();
    });
    clock.start();
    upload->start();
    while (!finished && clock.elapsed() < 80000) {
        assetServer->setLastHeardMicrostamp(usecTimestampNow());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    QVERIFY2(finished, "the upload never finished");
    QCOMPARE(error, AssetUpload::NetworkError);
    QVERIFY2(finishedAt >= 60000 && finishedAt < 66000, qPrintable(QString("finished at %1 ms").arg(finishedAt)));

    upload->deleteLater();
    nodeList->killNodeWithUUID(assetServer->getUUID());
    assetClient.clear();
    DependencyManager::destroy<AssetClient>();
}
