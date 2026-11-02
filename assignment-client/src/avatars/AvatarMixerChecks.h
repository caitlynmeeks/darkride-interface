//
//  AvatarMixerChecks.h
//  assignment-client/src/avatars
//
//  Created by servers on 2026-11-02.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  Checks the avatar mixer makes on what an app sends before the shared avatar code reads it: an avatar's skeleton
//  (the SkeletonData trait) and its identity. Each walks the data once, reading only lengths and counts, copies
//  nothing and allocates nothing, and returns why the data isn't what an app sends, or an empty string when it is.
//  The mixer drops data that fails, so nothing past a buffer is read here or relayed to anyone's app.
//

#ifndef hifi_AvatarMixerChecks_h
#define hifi_AvatarMixerChecks_h

#include <cstdint>
#include <cstring>

#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QString>
#include <QtCore/QUuid>

namespace avatar_mixer_checks {

// AvatarSkeletonTrait's packed layout (libraries/avatars AvatarData.h; AvatarMixerClientData.cpp checks these against
// the structs): Header = maxTranslationDimension, maxScaleDimension (floats), numJoints (uint8), stringTableLength
// (uint16); JointData = stringStart (uint16), stringLength (uint8), then 19 bytes this check doesn't need.
const int SKELETON_HEADER_SIZE = 11;
const int SKELETON_NUM_JOINTS_AT = 8;
const int SKELETON_TABLE_LENGTH_AT = 9;
const int SKELETON_JOINT_SIZE = 22;
const int SKELETON_STRING_START_AT = 0;
const int SKELETON_STRING_LENGTH_AT = 2;

// Identity limits, well past what an app sends: an avatar's address (a signed link included), its attachments (an old
// feature; the apps send none) and each name.
const quint32 MAX_IDENTITY_URL_BYTES = 16 * 1024;
const quint32 MAX_IDENTITY_ATTACHMENTS = 128;
const quint32 MAX_IDENTITY_NAME_CHARS = 256;

/// Why a SkeletonData trait isn't one an app sends, or empty when it is: the header, every joint and the name table
/// inside the data, and every joint's name inside the table as AvatarData::unpackSkeletonData reads it.
inline QString skeletonRefusal(const QByteArray& data) {
    const int size = data.size();
    if (size < SKELETON_HEADER_SIZE) {
        return QStringLiteral("a skeleton shorter than its header");
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.constData());
    const int numJoints = bytes[SKELETON_NUM_JOINTS_AT];
    uint16_t tableLength;
    memcpy(&tableLength, bytes + SKELETON_TABLE_LENGTH_AT, sizeof(tableLength));
    const qint64 tableAt = SKELETON_HEADER_SIZE + qint64(numJoints) * SKELETON_JOINT_SIZE;
    if (tableAt + tableLength > size) {
        return QStringLiteral("a skeleton whose joints or names run past its end");
    }
    // the table is read with QString::fromUtf8, then each name as a piece of it counted in characters: every byte
    // that doesn't continue a UTF-8 sequence gives at least one character (a bad one gives U+FFFD), so a piece inside
    // that count is inside the string the app reads; a leading byte-order mark gives none
    const unsigned char* table = bytes + tableAt;
    int tableChars = 0;
    for (int i = 0; i < tableLength; i++) {
        if ((table[i] & 0xC0) != 0x80) {
            tableChars++;
        }
    }
    if (tableLength >= 3 && table[0] == 0xEF && table[1] == 0xBB && table[2] == 0xBF) {
        tableChars--;
    }
    for (int joint = 0; joint < numJoints; joint++) {
        const unsigned char* record = bytes + SKELETON_HEADER_SIZE + joint * SKELETON_JOINT_SIZE;
        uint16_t stringStart;
        memcpy(&stringStart, record + SKELETON_STRING_START_AT, sizeof(stringStart));
        const int stringLength = record[SKELETON_STRING_LENGTH_AT];
        if (int(stringStart) + stringLength > tableChars) {
            return QStringLiteral("a skeleton joint whose name is outside the name table");
        }
    }
    return QString();
}

/// Skips one length-prefixed QByteArray or QString of at most maxBytes (0xFFFFFFFF = a null one); false when it is
/// longer or runs past the end.
inline bool skipCounted(QDataStream& in, quint32 maxBytes) {
    quint32 length = 0;
    in >> length;
    if (in.status() != QDataStream::Ok) {
        return false;
    }
    if (length == 0xFFFFFFFF) {
        return true;
    }
    return length <= maxBytes && in.skipRawData(int(length)) == int(length);
}

/// Why an identity (AvatarData::identityByteArray's layout: session id, sequence number, model address, attachments,
/// display name, session display name, flags) isn't one an app sends, or empty when it is. Reads the attachment count
/// before Qt would reserve room for it, and every length, without copying a string.
inline QString identityRefusal(const QByteArray& identity) {
    QDataStream in(identity);
    QUuid sessionID;
    qint32 sequenceNumber;
    in >> sessionID >> sequenceNumber;
    if (in.status() != QDataStream::Ok) {
        return QStringLiteral("an identity cut short");
    }
    if (!skipCounted(in, MAX_IDENTITY_URL_BYTES)) {
        return QStringLiteral("an identity with an avatar address too long or cut short");
    }
    quint32 attachments = 0;
    in >> attachments;
    if (in.status() != QDataStream::Ok) {
        return QStringLiteral("an identity cut short");
    }
    if (attachments > MAX_IDENTITY_ATTACHMENTS) {
        return QStringLiteral("an identity with %1 attachments").arg(attachments);
    }
    for (quint32 i = 0; i < attachments; i++) {
        if (!skipCounted(in, MAX_IDENTITY_URL_BYTES) || !skipCounted(in, MAX_IDENTITY_NAME_CHARS * 2)) {
            return QStringLiteral("an identity with an attachment too long or cut short");
        }
        // translation (3), rotation (4), scale: floats at the stream's precision, as AttachmentData writes them
        float value;
        for (int f = 0; f < 8; f++) {
            in >> value;
        }
        bool isSoft;
        in >> isSoft;
        if (in.status() != QDataStream::Ok) {
            return QStringLiteral("an identity cut short");
        }
    }
    if (!skipCounted(in, MAX_IDENTITY_NAME_CHARS * 2) || !skipCounted(in, MAX_IDENTITY_NAME_CHARS * 2)) {
        return QStringLiteral("an identity with a name too long or cut short");
    }
    return QString();
}

}

#endif // hifi_AvatarMixerChecks_h
