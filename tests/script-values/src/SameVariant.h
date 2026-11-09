//
//  SameVariant.h
//  tests/script-values/src
//
//  Whether two variants agree in type and value, nested values included, for the tests that compare two writings
//  of the same values.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_SameVariant_h
#define hifi_SameVariant_h

#include <cmath>

#include <QtCore/QString>
#include <QtCore/QVariant>

/// True when a and b agree in type and value, nested values included; `where` names the first difference.
inline bool sameVariant(const QVariant& a, const QVariant& b, QString& where) {
    if (a.userType() != b.userType()) {
        where = QString("type %1 vs %2").arg(a.typeName()).arg(b.typeName());
        return false;
    }
    if (a.userType() == QMetaType::QVariantMap) {
        const QVariantMap mapA = a.toMap();
        const QVariantMap mapB = b.toMap();
        if (mapA.keys() != mapB.keys()) {
            where = QString("keys %1 vs %2").arg(mapA.keys().join(",")).arg(mapB.keys().join(","));
            return false;
        }
        for (auto it = mapA.begin(); it != mapA.end(); ++it) {
            if (!sameVariant(it.value(), mapB.value(it.key()), where)) {
                where = it.key() + ": " + where;
                return false;
            }
        }
        return true;
    }
    if (a.userType() == QMetaType::QVariantList) {
        const QVariantList listA = a.toList();
        const QVariantList listB = b.toList();
        if (listA.size() != listB.size()) {
            where = QString("length %1 vs %2").arg(listA.size()).arg(listB.size());
            return false;
        }
        for (int i = 0; i < listA.size(); ++i) {
            if (!sameVariant(listA.at(i), listB.at(i), where)) {
                where = QString("[%1]: ").arg(i) + where;
                return false;
            }
        }
        return true;
    }
    if (a.userType() == QMetaType::Double) {
        const double x = a.toDouble();
        const double y = b.toDouble();
        if (std::isnan(x) && std::isnan(y)) {
            return true;
        }
        if (x != y || std::signbit(x) != std::signbit(y)) {
            where = QString("%1 vs %2").arg(x, 0, 'g', 17).arg(y, 0, 'g', 17);
            return false;
        }
        return true;
    }
    if (a != b) {
        where = QString("%1 vs %2").arg(a.toString()).arg(b.toString());
        return false;
    }
    return true;
}

#endif // hifi_SameVariant_h
