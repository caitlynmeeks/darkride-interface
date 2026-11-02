//
//  WorldMessagesTests.cpp
//  tests/script-api-guard
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The sender mark on Messages (security finding #11): a local message from the app's own side (a script on the user's
//  engine, QML, C++: all call the shared MessagesClient) arrives with fromOwnScripts true; one from a world's entity
//  scripts (WorldMessages, the world's send path) with false; a world's scripts can't send on, or hear, the app's
//  channels.
//

#include "WorldMessagesTests.h"

#include <atomic>

#include <QtCore/QThread>

#include <AccountManager.h>
#include <AddressManager.h>
#include <DependencyManager.h>
#include <MessagesClient.h>
#include <NodeList.h>
#include <ScriptCaller.h>

#include <WorldMessages.h>

QTEST_MAIN(WorldMessagesTests)

namespace {

struct Received {
    QString channel;
    bool localOnly { false };
    bool fromOwnScripts { false };
};

} // namespace

void WorldMessagesTests::initTestCase() {
    DependencyManager::registerInheritance<LimitedNodeList, NodeList>();
    DependencyManager::set<AccountManager>();
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent);
    DependencyManager::set<MessagesClient>();
}

void WorldMessagesTests::testAppChannels() {
    for (const char* channel : { "Snap.camera", "snap.camera", "Hifi-Notifications", "io.darkride.avatarMenu",
                                 "io.highfidelity.makeUserConnection", "com.tivolicloud.explore", "tivoli.emote",
                                 "Darkride-Panels", "entityToolUpdates", "chat", " Snap.camera" }) {
        QVERIFY2(WorldMessages::isAppChannel(channel), channel);
    }
    for (const char* channel : { "Darkride-Hop", "Hifi-Teleport-Disabler", "Hifi-Hand-Disabler", "Hifi-Object-Manipulation",
                                 "world.game", "Snapshot", "chatroom", "" }) {
        QVERIFY2(!WorldMessages::isAppChannel(channel), channel);
    }
}

void WorldMessagesTests::testSenderMark() {
    auto messages = DependencyManager::get<MessagesClient>();
    QList<Received> received;
    QObject context;
    QObject::connect(messages.data(), &MessagesClient::messageReceived, &context,
                     [&](QString channel, QString, QUuid, bool localOnly, bool fromOwnScripts) {
                         received.append({ channel, localOnly, fromOwnScripts });
                     });
    // the app's own side: a user's script, QML and C++ all call the shared object's script-callable methods
    messages->sendLocalMessage("own.local", "x");
    QMetaObject::invokeMethod(messages.data(), "sendMessage", Q_ARG(QString, "own.qml"), Q_ARG(QString, "x"), Q_ARG(bool, true));
    // no messages mixer here: a message for the network is delivered on this computer, marked the same way
    messages->sendMessage("own.nomixer", "x", false);
    // the world's send path
    messages->sendMessageFromWorld("world.local", "x", true);
    messages->sendMessageFromWorld("world.nomixer", "x", false);
    QCOMPARE(received.size(), 5);
    QCOMPARE(received[0].channel, QString("own.local"));
    QVERIFY(received[0].fromOwnScripts && received[0].localOnly);
    QVERIFY(received[1].fromOwnScripts);
    QVERIFY(received[2].fromOwnScripts);
    QVERIFY(!received[3].fromOwnScripts && received[3].localOnly);
    QVERIFY(!received[4].fromOwnScripts);

    // data messages carry the same mark
    QList<bool> dataMarks;
    QObject::connect(messages.data(), &MessagesClient::dataReceived, &context,
                     [&](QString, QByteArray, QUuid, bool, bool fromOwnScripts) { dataMarks.append(fromOwnScripts); });
    messages->sendData("own.data", QByteArray("x"), true);
    messages->sendDataFromWorld("world.data", QByteArray("x"), true);
    QCOMPARE(dataMarks, QList<bool>({ true, false }));
}

void WorldMessagesTests::testWorldRelay() {
    auto messages = DependencyManager::get<MessagesClient>();
    WorldMessages world(nullptr);
    QList<Received> heardByWorld;
    QObject::connect(&world, &WorldMessages::messageReceived, &world,
                     [&](QString channel, QString, QUuid, bool localOnly, bool fromOwnScripts) {
                         heardByWorld.append({ channel, localOnly, fromOwnScripts });
                     });
    QList<Received> heardByApp;
    QObject context;
    QObject::connect(messages.data(), &MessagesClient::messageReceived, &context,
                     [&](QString channel, QString, QUuid, bool localOnly, bool fromOwnScripts) {
                         heardByApp.append({ channel, localOnly, fromOwnScripts });
                     });

    // a world's script: its own channel goes through, marked as the world's; the app's channels are refused
    world.sendLocalMessage("world.game", "x");
    world.sendLocalMessage("Snap.camera", "{\"shutter\":true}");
    world.sendMessage("Hifi-Notifications", "x", false);
    world.sendLocalMessage("Darkride-Hop", "x");
    QCOMPARE(heardByApp.size(), 2);
    QCOMPARE(heardByApp[0].channel, QString("world.game"));
    QVERIFY(!heardByApp[0].fromOwnScripts);
    QCOMPARE(heardByApp[1].channel, QString("Darkride-Hop"));

    // the app's side: the world hears its game channels, never the app's own
    heardByWorld.clear();
    messages->sendLocalMessage("Hifi-Teleport-Disabler", "both");
    messages->sendLocalMessage("io.darkride.avatarMenu", "secret");
    messages->sendLocalMessage("chat", "{\"message\":\"hi\"}");
    QCoreApplication::processEvents();
    QCOMPARE(heardByWorld.size(), 1);
    QCOMPARE(heardByWorld[0].channel, QString("Hifi-Teleport-Disabler"));
    QVERIFY(heardByWorld[0].fromOwnScripts);
}

void WorldMessagesTests::testWorldLeft() {
    // security #32: once its world is left, a world's Messages relays nothing more to its scripts, on the channels it
    // subscribed to as on any other. Its relay runs on its engine's thread, marked as the world's.
    const int LEFT = 9401;
    auto messages = DependencyManager::get<MessagesClient>();
    QThread engineThread;
    engineThread.start();
    auto world = new WorldMessages(nullptr);
    world->moveToThread(&engineThread);
    QMetaObject::invokeMethod(world, [] { ScriptCaller::markThreadAsWorld(LEFT); }, Qt::BlockingQueuedConnection);
    std::atomic<int> heard { 0 };
    QObject::connect(world, &WorldMessages::messageReceived, world, [&] { heard++; }, Qt::DirectConnection);
    QObject::connect(world, &WorldMessages::dataReceived, world, [&] { heard++; }, Qt::DirectConnection);
    world->subscribe("world.game");

    messages->sendLocalMessage("world.game", "while there");
    QTRY_COMPARE(heard.load(), 1);

    ScriptCaller::retireWorld(LEFT);
    messages->sendLocalMessage("world.game", "after leaving");
    messages->sendData("world.game", QByteArray("x"), true);
    messages->sendLocalMessage("another.channel", "after leaving");
    // a marker through the same thread: once it has run, every message sent before it has been relayed or dropped
    std::atomic<bool> drained { false };
    QMetaObject::invokeMethod(world, [&] { drained = true; }, Qt::QueuedConnection);
    QTRY_VERIFY(drained.load());
    QCOMPARE(heard.load(), 1);

    world->deleteLater();
    engineThread.quit();
    QVERIFY(engineThread.wait(5000));
}
