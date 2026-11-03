//
//  WireStream.h
//  libraries/shared/src/shared
//
//  Created by servers on 2026-11-03.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The one Qt data stream format our network packets and our own files are written in, so a Qt 6 build and a Qt 5 one
//  read each other's bytes (the Qt 6 move's stage 0, servers + scripting; team/editor-ui/QT6-PLAN.md). Each stream that
//  goes on the wire or to disk calls pin() right after it is made. It is opt-in, never a global default: a few file
//  formats carry their own older version as part of the format (FBX, glTF, the settings file) and keep it.
//

#ifndef hifi_WireStream_h
#define hifi_WireStream_h

#include <QtCore/QDataStream>

namespace hifi {
namespace stream {

/// The format: what a Qt 5.15 build writes by default today.
const QDataStream::Version WIRE_VERSION = QDataStream::Qt_5_15;

/// Pins a stream to the wire format: floats as 8 bytes and big-endian (Qt's defaults in 5 and 6, stated here so they
/// can't drift). Call it before anything is read or written; a stream that wants something else (a little-endian file
/// format, say) sets it after.
inline QDataStream& pin(QDataStream& stream) {
    stream.setVersion(WIRE_VERSION);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream.setByteOrder(QDataStream::BigEndian);
    return stream;
}

}
}

#endif // hifi_WireStream_h
