//
//  AvatarMixerChecksTests.cpp
//  tests/avatars/src
//
//  Created by servers on 2026-11-02.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "AvatarMixerChecksTests.h"

#include <QtCore/QUrl>
#include <QtCore/QUuid>

#include "../../../assignment-client/src/avatars/AvatarMixerChecks.h"

QTEST_MAIN(AvatarMixerChecksTests)

using namespace avatar_mixer_checks;

namespace {

struct Joint {
    int stringStart;
    int stringLength;
};

// A skeleton laid out as AvatarData::packSkeletonData writes it: the header, a 22-byte record per joint, the names.
QByteArray skeleton(const QList<Joint>& joints, const QByteArray& table, int numJoints = -1, int tableLength = -1) {
    QByteArray data(SKELETON_HEADER_SIZE, 0);
    data[SKELETON_NUM_JOINTS_AT] = char(numJoints < 0 ? joints.size() : numJoints);
    const uint16_t length = uint16_t(tableLength < 0 ? table.size() : tableLength);
    memcpy(data.data() + SKELETON_TABLE_LENGTH_AT, &length, sizeof(length));
    for (const Joint& joint : joints) {
        QByteArray record(SKELETON_JOINT_SIZE, 0);
        const uint16_t start = uint16_t(joint.stringStart);
        memcpy(record.data() + SKELETON_STRING_START_AT, &start, sizeof(start));
        record[SKELETON_STRING_LENGTH_AT] = char(joint.stringLength);
        data += record;
    }
    return data + table;
}

struct Attachment {
    QUrl modelURL;
    QString jointName;
};

// An identity laid out as AvatarData::identityByteArray writes it (AttachmentData's operator<< for each attachment).
QByteArray identity(const QUrl& model, const QList<Attachment>& attachments, const QString& displayName,
                    const QString& sessionDisplayName, qint64 attachmentCount = -1) {
    QByteArray data;
    QDataStream out(&data, QIODevice::Append);
    out << QUuid::createUuid() << qint32(7) << model;
    out << quint32(attachmentCount < 0 ? attachments.size() : attachmentCount);
    for (const Attachment& attachment : attachments) {
        out << attachment.modelURL << attachment.jointName;
        out << 0.1f << 0.2f << 0.3f;           // translation
        out << 0.0f << 0.0f << 0.0f << 1.0f;   // rotation
        out << 1.0f << false;                  // scale, isSoft
    }
    out << displayName << sessionDisplayName << qint32(0);
    return data;
}

const QUrl MODEL("https://cdn.darkride.ai/avatars/piper/piper.fst");

}

void AvatarMixerChecksTests::anAppsSkeletonPasses() {
    QCOMPARE(skeletonRefusal(skeleton({ { 0, 4 }, { 4, 5 }, { 9, 4 } }, "HipsSpineHead")), QString());
}

void AvatarMixerChecksTests::aSkeletonWithNoJointsPasses() {
    QCOMPARE(skeletonRefusal(skeleton({}, "")), QString());
}

void AvatarMixerChecksTests::aSkeletonShorterThanItsHeaderIsDropped() {
    QVERIFY(!skeletonRefusal(QByteArray()).isEmpty());
    QVERIFY(!skeletonRefusal(QByteArray(SKELETON_HEADER_SIZE - 1, 0)).isEmpty());
}

void AvatarMixerChecksTests::jointsPastTheEndAreDropped() {
    // the header says 255 joints, the data holds two
    QVERIFY(!skeletonRefusal(skeleton({ { 0, 1 }, { 1, 1 } }, "ab", 255)).isEmpty());
}

void AvatarMixerChecksTests::aNameTablePastTheEndIsDropped() {
    QVERIFY(!skeletonRefusal(skeleton({ { 0, 2 } }, "ab", -1, 60000)).isEmpty());
}

void AvatarMixerChecksTests::aNameOutsideTheTableIsDropped() {
    QVERIFY(!skeletonRefusal(skeleton({ { 0, 4 }, { 40000, 200 } }, "Hips")).isEmpty());
    QVERIFY(!skeletonRefusal(skeleton({ { 2, 3 } }, "Hips")).isEmpty());
}

void AvatarMixerChecksTests::aNameEndingAtTheTablesEndPasses() {
    QCOMPARE(skeletonRefusal(skeleton({ { 0, 2 }, { 2, 2 } }, "Hips")), QString());
    QCOMPARE(skeletonRefusal(skeleton({ { 4, 0 } }, "Hips")), QString());
}

void AvatarMixerChecksTests::namesAreCountedInCharacters() {
    // "Bein" + "Fuß": 8 bytes, 7 characters; a name reaching the 8th character would be read past the string
    const QByteArray table = QString::fromUtf8("BeinFu\xc3\x9f").toUtf8();
    QCOMPARE(table.size(), 8);
    QCOMPARE(skeletonRefusal(skeleton({ { 0, 4 }, { 4, 3 } }, table)), QString());
    QVERIFY(!skeletonRefusal(skeleton({ { 0, 4 }, { 4, 4 } }, table)).isEmpty());
}

void AvatarMixerChecksTests::aByteOrderMarkCountsForNothing() {
    const QByteArray table = QByteArray("\xef\xbb\xbf") + "Hips";
    QCOMPARE(QString::fromUtf8(table).size(), 4);
    QCOMPARE(skeletonRefusal(skeleton({ { 0, 4 } }, table)), QString());
    QVERIFY(!skeletonRefusal(skeleton({ { 0, 5 } }, table)).isEmpty());
}

void AvatarMixerChecksTests::bytesAfterTheTablePass() {
    QCOMPARE(skeletonRefusal(skeleton({ { 0, 4 } }, "Hips") + QByteArray(10, 'x')), QString());
}

void AvatarMixerChecksTests::anAppsIdentityPasses() {
    QCOMPARE(identityRefusal(identity(MODEL, {}, "Caity", "Caity")), QString());
}

void AvatarMixerChecksTests::anIdentityWithNullStringsPasses() {
    QCOMPARE(identityRefusal(identity(QUrl(), {}, QString(), QString())), QString());
}

void AvatarMixerChecksTests::anIdentityWithAttachmentsPasses() {
    const QList<Attachment> two { { QUrl("https://cdn.darkride.ai/hat.fbx"), "Head" }, { QUrl(), QString() } };
    QCOMPARE(identityRefusal(identity(MODEL, two, "Caity", "Caity")), QString());
}

void AvatarMixerChecksTests::aHugeAttachmentCountIsDropped() {
    // Qt would reserve room for 2^31 attachments before reading one
    QVERIFY(!identityRefusal(identity(MODEL, {}, "Caity", "Caity", 0x7fffffff)).isEmpty());
    QVERIFY(!identityRefusal(identity(MODEL, {}, "Caity", "Caity", MAX_IDENTITY_ATTACHMENTS + 1)).isEmpty());
}

void AvatarMixerChecksTests::moreAttachmentsThanTheDataHoldsIsDropped() {
    const QList<Attachment> two { { QUrl("https://cdn.darkride.ai/hat.fbx"), "Head" }, { QUrl(), QString() } };
    QByteArray data = identity(MODEL, two, QString(), QString(), 100);
    QVERIFY(!identityRefusal(data).isEmpty());
}

void AvatarMixerChecksTests::aLongAddressIsDropped() {
    const QUrl longModel("https://cdn.darkride.ai/" + QString(MAX_IDENTITY_URL_BYTES, 'a') + ".fst");
    QVERIFY(!identityRefusal(identity(longModel, {}, "Caity", "Caity")).isEmpty());
    const QList<Attachment> one { { longModel, "Head" } };
    QVERIFY(!identityRefusal(identity(MODEL, one, "Caity", "Caity")).isEmpty());
}

void AvatarMixerChecksTests::aLongNameIsDropped() {
    const QString name(MAX_IDENTITY_NAME_CHARS + 1, 'n');
    QVERIFY(!identityRefusal(identity(MODEL, {}, name, "Caity")).isEmpty());
    QVERIFY(!identityRefusal(identity(MODEL, {}, "Caity", name)).isEmpty());
    const QList<Attachment> one { { QUrl(), name } };
    QVERIFY(!identityRefusal(identity(MODEL, one, "Caity", "Caity")).isEmpty());
}

void AvatarMixerChecksTests::aNameAtTheLimitPasses() {
    const QString name(MAX_IDENTITY_NAME_CHARS, 'n');
    QCOMPARE(identityRefusal(identity(MODEL, {}, name, name)), QString());
}

void AvatarMixerChecksTests::aShortIdentityIsDropped() {
    QVERIFY(!identityRefusal(QByteArray()).isEmpty());
    const QByteArray whole = identity(MODEL, {}, "Caity", "Caity");
    // cut inside the session id, the address, and the attachment count
    for (int length : { 10, 22, 30, whole.size() - 25 }) {
        QVERIFY2(!identityRefusal(whole.left(length)).isEmpty(), qPrintable(QString("cut at %1").arg(length)));
    }
}

void AvatarMixerChecksTests::newIdsWithinTheLimitsAreTaken() {
    QCOMPARE(newInstanceRefusal(0, 0, false, AVATAR_ENTITY_LIMITS), QString());
    QCOMPARE(newInstanceRefusal(127, 400, false, AVATAR_ENTITY_LIMITS), QString());
    QCOMPARE(newInstanceRefusal(2, 900, false, GRAB_LIMITS), QString());
    QCOMPARE(newInstanceRefusal(2, 4000, false, GRAB_LIMITS), QString());
    QCOMPARE(newInstanceRefusal(0, 0, false, MENU_PARAMETERS_LIMITS), QString());
}

void AvatarMixerChecksTests::tooManyLiveIdsAreRefused() {
    QVERIFY(!newInstanceRefusal(AVATAR_ENTITY_LIMITS.maxLive, 300, false, AVATAR_ENTITY_LIMITS).isEmpty());
    QVERIFY(!newInstanceRefusal(GRAB_LIMITS.maxLive, 64, false, GRAB_LIMITS).isEmpty());
}

void AvatarMixerChecksTests::aDeleteForAnUnlistedIdIsRefused() {
    QVERIFY(!newInstanceRefusal(0, 0, true, AVATAR_ENTITY_LIMITS).isEmpty());
    QVERIFY(!newInstanceRefusal(1, 5, true, GRAB_LIMITS).isEmpty());
}

void AvatarMixerChecksTests::theCapSaysToReconnect() {
    QVERIFY(newInstanceRefusal(3, GRAB_LIMITS.maxAll, false, GRAB_LIMITS).contains("until this avatar reconnects"));
    QCOMPARE(GRAB_LIMITS.maxAll, 4096);
    QCOMPARE(AVATAR_ENTITY_LIMITS.maxAll, 1024);
}

void AvatarMixerChecksTests::tooManyListedIdsAreRefused() {
    QVERIFY(!newInstanceRefusal(10, AVATAR_ENTITY_LIMITS.maxAll, false, AVATAR_ENTITY_LIMITS).isEmpty());
    QVERIFY(!newInstanceRefusal(0, GRAB_LIMITS.maxAll, false, GRAB_LIMITS).isEmpty());
    QVERIFY(!newInstanceRefusal(1, MENU_PARAMETERS_LIMITS.maxAll, false, MENU_PARAMETERS_LIMITS).isEmpty());
}

void AvatarMixerChecksTests::aBurstOfNewIdsIsTakenThenTheRate() {
    NewInstanceRate rate;
    const quint64 start = 1000000;
    for (int i = 0; i < NewInstanceRate::BURST; i++) {
        QVERIFY2(rate.take(start), qPrintable(QString("id %1 of the burst").arg(i)));
    }
    QVERIFY(!rate.take(start));
    QVERIFY(!rate.take(start + 100000));   // 0.1 s: half an id back
    QVERIFY(rate.take(start + 200000));    // 0.2 s: one
    QVERIFY(!rate.take(start + 200000));
    // a second later: five more
    int taken = 0;
    while (rate.take(start + 1200000)) {
        taken++;
    }
    QCOMPARE(taken, NewInstanceRate::PER_SECOND);
}

void AvatarMixerChecksTests::theRateComesBackNoHigherThanTheBurst() {
    NewInstanceRate rate;
    QVERIFY(rate.take(1000000));
    int taken = 0;
    while (rate.take(1000000 + 3600ULL * 1000000)) {
        taken++;
    }
    QCOMPARE(taken, NewInstanceRate::BURST);
}

void AvatarMixerChecksTests::aClockGoingBackGivesNothing() {
    NewInstanceRate rate;
    for (int i = 0; i < NewInstanceRate::BURST; i++) {
        rate.take(5000000);
    }
    QVERIFY(!rate.take(1000000));
    QVERIFY(!rate.take(5000000));
    QVERIFY(rate.take(5200000));
}
