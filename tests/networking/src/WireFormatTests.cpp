//
//  WireFormatTests.cpp
//  tests/networking/src
//
//  Created by servers on 2026-11-03.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
//  The recorded bytes (GOLDEN_*) came from a Qt 5.15 build of this code. To record them again on a Qt 5 build (never on
//  Qt 6: they are what Qt 6 must match), run the test with WIRE_FORMAT_PRINT=1 and paste the printed lines in.
//

#include "WireFormatTests.h"

#include <algorithm>

#include <QtCore/QUuid>

#include <Assignment.h>
#include <DataServerAccountInfo.h>
#include <NetworkPeer.h>
#include <OAuthAccessToken.h>
#include <HifiSockAddr.h>
#include <Node.h>
#include <NodePermissions.h>
#include <shared/WireStream.h>

QTEST_MAIN(WireFormatTests)

// as AccountManager.cpp declares them, for the saved accounts file
Q_DECLARE_METATYPE(OAuthAccessToken)
Q_DECLARE_METATYPE(DataServerAccountInfo)

namespace {

const QUuid NODE_ID("{8f2a7c3e-1d4b-4e6f-9a0c-5b7d2e8f1a3c}");
const QUuid ASSIGNMENT_ID("{2b4d6f80-1a3c-4e5f-8b7d-9c0e2a4f6b8d}");
const HifiSockAddr PUBLIC_SOCKET(QHostAddress("203.0.113.7"), 40000);
const HifiSockAddr LOCAL_SOCKET(QHostAddress("192.168.1.20"), 40001);

// recorded from the Qt 5.15 build (see the top of the file)
const char* GOLDEN_SOCK_ADDR = "00cb0071079c40";
const char* GOLDEN_PERMISSIONS = "00000005";
const char* GOLDEN_NODE = "498f2a7c3e1d4b4e6f9a0c5b7d2e8f1a3c00cb0071079c4000c0a801149c410000000101002a";
const char* GOLDEN_ASSIGNMENT = "062b4d6f801a3c4e5f8b7d9c0e2a4f6b8d0000000c0070006f006f006c002d0061000000077061796c6f6164";
// a request as the Qt 5 build wrote it: it adds the app's version ("dev" on that build) and an empty wallet id, so it
// is checked by reading, since another build writes another version
const char* GOLDEN_NETWORK_PEER = "8f2a7c3e1d4b4e6f9a0c5b7d2e8f1a3c00cb0071079c4000c0a801149c41";
const char* GOLDEN_NODE_TYPES = "000000044d576d6f";
// AccountInfo.bin as a Qt 5 build writes it, for a made-up account (no real token or key): the riskiest of our own
// files, a QVariantMap holding a user type
const char* GOLDEN_ACCOUNT_FILE =
    "000000010000002c00680074007400700073003a002f002f00760072002e006400610072006b0072006900640065002e0061"
    "0069000004000000000016446174615365727665724163636f756e74496e666f000000001a006d006100640065002d007500"
    "70002d0074006f006b0065006e000001a19eb4a0000000000c0042006500610072006500720000001e006d00610064006500"
    "2d00750070002d00720065006600720065007300680000001000770069007200650074006500730074ffffffffffffffff00"
    "000000000000000000000000000000ffffffff00000000000000000000000000000000000000000000000000000000000000"
    "00ffffffff";
const char* GOLDEN_ASSIGNMENT_REQUEST =
    "062b4d6f801a3c4e5f8b7d9c0e2a4f6b8d0000000c0070006f006f006c002d0061000000077061796c6f61640000000600640065007600000000"
    "000000000000000000000000";

template <typename T>
QByteArray written(const T& value) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    hifi::stream::pin(out);
    out << value;
    return bytes;
}

// compares with the recorded bytes, or prints them when recording
void checkGolden(const char* name, const QByteArray& bytes, const char* golden) {
    if (qEnvironmentVariableIsSet("WIRE_FORMAT_PRINT")) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QFAIL("record on a Qt 5 build only: the recorded bytes are what Qt 6 must match");
#endif
        printf("const char* GOLDEN_%s = \"%s\";\n", name, bytes.toHex().constData());
        return;
    }
    QVERIFY2(QByteArray(golden).size() > 0, qPrintable(QString("no recorded bytes for %1").arg(name)));
    QCOMPARE(bytes.toHex(), QByteArray(golden));
}

template <typename T>
T readBack(const QByteArray& bytes, T value) {
    QDataStream in(bytes);
    hifi::stream::pin(in);
    in >> value;
    return value;
}

}

void WireFormatTests::thePinIsQt5_15WithDoubleFloats() {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    hifi::stream::pin(out);
    QCOMPARE(out.version(), int(QDataStream::Qt_5_15));
    QCOMPARE(out.floatingPointPrecision(), QDataStream::DoublePrecision);
    QCOMPARE(out.byteOrder(), QDataStream::BigEndian);
    out << 0.5f << quint16(0x0102);
    QCOMPARE(bytes.size(), 10);   // a float goes as 8 bytes, as Qt 5 writes it
    QCOMPARE(bytes.right(2).toHex(), QByteArray("0102"));   // big-endian
}

// On the Qt 5.15 build the pin writes exactly what an unpinned stream wrote, so pinning changes no bytes there.
void WireFormatTests::pinnedBytesMatchQt5Defaults() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QByteArray pinned = written(PUBLIC_SOCKET);
    QByteArray unpinned;
    QDataStream out(&unpinned, QIODevice::WriteOnly);
    out << PUBLIC_SOCKET << 0.25f << QString("x");
    QByteArray pinnedMore;
    QDataStream outPinned(&pinnedMore, QIODevice::WriteOnly);
    hifi::stream::pin(outPinned);
    outPinned << PUBLIC_SOCKET << 0.25f << QString("x");
    QCOMPARE(pinnedMore, unpinned);
    QVERIFY(pinned.size() > 0);
#else
    QSKIP("only meaningful on the Qt 5 build");
#endif
}

void WireFormatTests::hifiSockAddr() {
    const QByteArray bytes = written(PUBLIC_SOCKET);
    checkGolden("SOCK_ADDR", bytes, GOLDEN_SOCK_ADDR);
    QCOMPARE(readBack(bytes, HifiSockAddr()), PUBLIC_SOCKET);
}

void WireFormatTests::nodePermissions() {
    NodePermissions permissions;
    permissions.set(NodePermissions::Permission::canConnectToDomain);
    permissions.set(NodePermissions::Permission::canRezPermanentEntities);
    const QByteArray bytes = written(permissions);
    checkGolden("PERMISSIONS", bytes, GOLDEN_PERMISSIONS);
    QCOMPARE(uint(readBack(bytes, NodePermissions()).permissions), uint(permissions.permissions));
}

void WireFormatTests::node() {
    Node node(NODE_ID, NodeType::Agent, PUBLIC_SOCKET, LOCAL_SOCKET);
    NodePermissions permissions;
    permissions.set(NodePermissions::Permission::canConnectToDomain);
    node.setPermissions(permissions);
    node.setIsReplicated(true);
    node.setLocalID(42);
    const QByteArray bytes = written(node);
    checkGolden("NODE", bytes, GOLDEN_NODE);

    Node back(QUuid(), NodeType::Unassigned, HifiSockAddr(), HifiSockAddr());
    QDataStream in(bytes);
    hifi::stream::pin(in);
    in >> back;
    QCOMPARE(back.getUUID(), NODE_ID);
    QCOMPARE(back.getType(), NodeType::Agent);
    QCOMPARE(back.getPublicSocket(), PUBLIC_SOCKET);
    QCOMPARE(back.getLocalSocket(), LOCAL_SOCKET);
    QCOMPARE(uint(back.getPermissions().permissions), uint(permissions.permissions));
    QVERIFY(back.isReplicated());
    QCOMPARE(back.getLocalID(), Node::LocalID(42));
}

void WireFormatTests::assignment() {
    Assignment create(Assignment::CreateCommand, Assignment::EntityServerType, "pool-a");
    create.setUUID(ASSIGNMENT_ID);
    create.setPayload(QByteArray("payload"));
    const QByteArray bytes = written(create);
    checkGolden("ASSIGNMENT", bytes, GOLDEN_ASSIGNMENT);

    Assignment back(Assignment::CreateCommand, Assignment::AllTypes);
    QDataStream in(bytes);
    hifi::stream::pin(in);
    in >> back;
    QCOMPARE(back.getType(), Assignment::EntityServerType);
    QCOMPARE(back.getUUID(), ASSIGNMENT_ID);
    QCOMPARE(back.getPool(), QString("pool-a"));
    QCOMPARE(back.getPayload(), QByteArray("payload"));
}

void WireFormatTests::anAssignmentRequestReads() {
    const QByteArray bytes = QByteArray::fromHex(GOLDEN_ASSIGNMENT_REQUEST);
    Assignment back(Assignment::RequestCommand, Assignment::AllTypes);
    QDataStream in(bytes);
    hifi::stream::pin(in);
    in >> back;
    QCOMPARE(in.status(), QDataStream::Ok);
    QVERIFY(in.atEnd());
    QCOMPARE(back.getType(), Assignment::EntityServerType);
    QCOMPARE(back.getUUID(), ASSIGNMENT_ID);
    QCOMPARE(back.getPool(), QString("pool-a"));
    QCOMPARE(back.getPayload(), QByteArray("payload"));
    QCOMPARE(back.getNodeVersion(), QString("dev"));
}

void WireFormatTests::networkPeer() {
    NetworkPeer peer(NODE_ID, PUBLIC_SOCKET, LOCAL_SOCKET);
    const QByteArray bytes = peer.toByteArray();
    checkGolden("NETWORK_PEER", bytes, GOLDEN_NETWORK_PEER);
    NetworkPeer back;
    QDataStream in(bytes);
    hifi::stream::pin(in);
    in >> back;
    QCOMPARE(back.getUUID(), NODE_ID);
    QCOMPARE(back.getPublicSocket(), PUBLIC_SOCKET);
    QCOMPARE(back.getLocalSocket(), LOCAL_SOCKET);
}

// the connect request's node types, sorted as NodeList writes them (they come from a set)
void WireFormatTests::nodeTypesOfInterest() {
    QList<NodeType_t> types { NodeType::EntityServer, NodeType::AudioMixer, NodeType::AvatarMixer, NodeType::MessagesMixer };
    std::sort(types.begin(), types.end());
    const QByteArray bytes = written(types);
    checkGolden("NODE_TYPES", bytes, GOLDEN_NODE_TYPES);
    QCOMPARE(readBack(bytes, QList<NodeType_t>()), types);
}

void WireFormatTests::theAccountFile() {
    qRegisterMetaType<OAuthAccessToken>("OAuthAccessToken");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)  // Qt 6 registers the stream operators with the type
    qRegisterMetaTypeStreamOperators<OAuthAccessToken>("OAuthAccessToken");
#endif
    qRegisterMetaType<DataServerAccountInfo>("DataServerAccountInfo");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)  // Qt 6 registers the stream operators with the type
    qRegisterMetaTypeStreamOperators<DataServerAccountInfo>("DataServerAccountInfo");
#endif

    OAuthAccessToken token;
    token.token = "made-up-token";
    token.refreshToken = "made-up-refresh";
    token.expiryTimestamp = 1793664000000LL;
    token.tokenType = "Bearer";
    DataServerAccountInfo account;
    account.setAccessToken(token);
    account.setUsername("wiretest");
    QVariantMap accounts;
    accounts.insert("https://vr.darkride.ai", QVariant::fromValue(account));

    const QByteArray bytes = written(accounts);
    checkGolden("ACCOUNT_FILE", bytes, GOLDEN_ACCOUNT_FILE);

    const QVariantMap back = readBack(bytes, QVariantMap());
    QVERIFY(back.contains("https://vr.darkride.ai"));
    const DataServerAccountInfo backAccount = back.value("https://vr.darkride.ai").value<DataServerAccountInfo>();
    QCOMPARE(backAccount.getUsername(), QString("wiretest"));
    QCOMPARE(backAccount.getAccessToken().token, QString("made-up-token"));
    QCOMPARE(backAccount.getAccessToken().refreshToken, QString("made-up-refresh"));
    QCOMPARE(backAccount.getAccessToken().expiryTimestamp, 1793664000000LL);
}
