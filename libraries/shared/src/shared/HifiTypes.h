//
//  HifiTypes.h
//  libraries/shared/src/shared
//
//  Created by Sabrina Shanman on 2018/11/12.
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_HifiTypes_h
#define hifi_HifiTypes_h

#include <QVarLengthArray>
#include <QVariant>
#include <QHash>
#include <QUrl>

namespace hifi {
    using ByteArray = QByteArray;
    using VariantHash = QVariantHash;
    // An .fst mapping, where a key can repeat: several "script" or "freeJoint" lines, several source shapes for one
    // face shape under "bs".
    using VariantMultiHash = QMultiHash<QString, QVariant>;
    using URL = QUrl;

    // A heading of a mapping ("bs", "joint", ...) stored as a value of the mapping, keeping its repeated keys. On
    // Qt 5 a QVariantHash holds repeated keys, so the stored form is the QVariantHash as always; Qt 6's QHash can't,
    // so there the multi-hash itself is stored.
    inline QVariant variantFromMultiHash(const VariantMultiHash& hash) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        return QVariant(static_cast<const QVariantHash&>(hash));
#else
        return QVariant::fromValue(hash);
#endif
    }

    // A heading read back, whichever form it was stored in (empty when the value isn't a heading).
    inline VariantMultiHash multiHashFromVariant(const QVariant& value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (value.metaType() == QMetaType::fromType<VariantMultiHash>()) {
            return value.value<VariantMultiHash>();
        }
#endif
        return VariantMultiHash(value.toHash());
    }
};

#endif // hifi_HifiTypes_h
