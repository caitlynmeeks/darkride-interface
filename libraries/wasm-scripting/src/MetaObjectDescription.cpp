//
//  MetaObjectDescription.cpp
//  libraries/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "MetaObjectDescription.h"

#include <QtCore/QHash>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaMethod>
#include <QtCore/QMetaObject>
#include <QtCore/QMetaProperty>
#include <QtCore/QObject>
#include <QtCore/QVariantList>

#include <RegisteredMetaTypes.h>

namespace MetaObjectDescription {

namespace {

const QMetaObject* QOBJECT_META = &QObject::staticMetaObject;

// A property's type as the script API names it, the same on both Qts: the declared name. Qt 6 reports the metatype's
// own name instead, which spells a class's enum with its class ("Probe::Mode" for Mode) and a glm alias as the
// template it stands for ("glm::vec<3,float>" for glm::vec3).
QString propertyTypeName(const QMetaProperty& property, const QMetaObject* declaring) {
    static const QHash<int, QByteArray> GLM_ALIASES {
        { qMetaTypeId<glm::vec2>(), "glm::vec2" }, { qMetaTypeId<glm::vec3>(), "glm::vec3" },
        { qMetaTypeId<glm::vec4>(), "glm::vec4" }, { qMetaTypeId<glm::quat>(), "glm::quat" },
        { qMetaTypeId<glm::mat4>(), "glm::mat4" }, { qMetaTypeId<glm::u8vec3>(), "glm::u8vec3" }
    };
    const auto alias = GLM_ALIASES.constFind(property.userType());
    if (alias != GLM_ALIASES.constEnd()) {
        return QString::fromUtf8(alias.value());
    }
    QByteArray name = property.typeName();
    const QByteArray scope = QByteArray(declaring->className()) + "::";
    if (name.startsWith(scope)) {
        name = name.mid(scope.size());
    }
    return QString::fromUtf8(name);
}

QVariantList parameters(const QMetaMethod& method) {
    QVariantList list;
    const auto types = method.parameterTypes();
    const auto names = method.parameterNames();
    for (int p = 0; p < types.size(); ++p) {
        QVariantMap parameter;
        parameter["type"] = QString::fromUtf8(types[p]);
        parameter["name"] = QString::fromUtf8(names.value(p));
        list.append(parameter);
    }
    return list;
}

} // namespace

QVariantMap describe(const QObject* object, const Options& options) {
    return object ? describe(object->metaObject(), options) : QVariantMap();
}

QVariantMap describe(const QMetaObject* meta, const Options& options) {
    QVariantMap description;
    if (!meta) {
        return description;
    }
    description["class"] = QString::fromUtf8(meta->className());
    QVariantList methods, signalList, properties, enums;
    for (int i = 0; i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        // the members QtScript shows a script: public and protected ones (AudioScriptingInterface's playSound is
        // a protected invokable), not private ones
        if (method.access() == QMetaMethod::Private || method.methodType() == QMetaMethod::Constructor) {
            continue;
        }
        const QMetaObject* declaring = method.enclosingMetaObject();
        if (declaring == QOBJECT_META && !options.includeQObjectMembers) {
            continue;
        }
        if (method.name() == "deleteLater" && !options.includeDeleteLater) {
            continue;
        }
        QVariantMap entry;
        entry["name"] = QString::fromUtf8(method.name());
        entry["signature"] = QString::fromUtf8(method.methodSignature());
        entry["index"] = i;
        entry["declaringClass"] = QString::fromUtf8(declaring->className());
        entry["parameters"] = parameters(method);
        if (method.methodType() == QMetaMethod::Signal) {
            signalList.append(entry);
        } else {
            entry["returns"] = QString::fromUtf8(method.typeName());
            entry["kind"] = method.methodType() == QMetaMethod::Slot ? "slot" : "invokable";
            methods.append(entry);
        }
    }
    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty property = meta->property(i);
        const QMetaObject* declaring = property.enclosingMetaObject();
        if (declaring == QOBJECT_META && !options.includeQObjectMembers) {
            continue;
        }
        QVariantMap entry;
        entry["name"] = QString::fromUtf8(property.name());
        entry["type"] = propertyTypeName(property, declaring);
        entry["index"] = i;
        entry["readable"] = property.isReadable();
        entry["writable"] = property.isWritable();
        entry["constant"] = property.isConstant();
        entry["declaringClass"] = QString::fromUtf8(declaring->className());
        if (property.hasNotifySignal()) {
            entry["notify"] = QString::fromUtf8(property.notifySignal().name());
        }
        if (property.isEnumType()) {
            entry["enum"] = QString::fromUtf8(property.enumerator().name());
        }
        properties.append(entry);
    }
    for (int i = 0; i < meta->enumeratorCount(); ++i) {
        QMetaEnum enumerator = meta->enumerator(i);
        QVariantMap entry;
        entry["name"] = QString::fromUtf8(enumerator.name());
        QVariantMap values;
        for (int k = 0; k < enumerator.keyCount(); ++k) {
            values[QString::fromUtf8(enumerator.key(k))] = enumerator.value(k);
        }
        entry["values"] = values;
        enums.append(entry);
    }
    description["methods"] = methods;
    description["signals"] = signalList;
    description["properties"] = properties;
    description["enums"] = enums;
    return description;
}

} // namespace MetaObjectDescription
