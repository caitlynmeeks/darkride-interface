//
//  GuardedObject.cpp
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "GuardedObject.h"

#include <QtCore/QMetaMethod>
#include <QtCore/QMetaObject>
#include <QtCore/QMetaProperty>
#include <QtScript/QScriptContext>
#include <QtScript/QScriptString>

#include "ScriptEngineLogging.h"

GuardedObjectClass::GuardedObjectClass(QScriptEngine* engine, HostInterface::MemberGate gate) :
    QScriptClass(engine), _gate(std::move(gate)) {
}

QScriptValue GuardedObjectClass::wrap(const QScriptValue& raw) {
    if (!raw.isQObject() || !raw.toQObject()) {
        return raw;
    }
    QScriptValue guard = engine()->newObject(this);
    guard.setData(raw);   // C++ only: a script cannot read an object's data
    return guard;
}

GuardedObjectClass::Kind GuardedObjectClass::kindOf(const QScriptValue& raw, const QByteArray& name, bool& writable) const {
    writable = false;
    QObject* object = raw.toQObject();
    if (!object) {
        return Absent;
    }
    const QMetaObject* meta = object->metaObject();
    if (!_gate.allowsClass(meta)) {
        return Absent;
    }
    const int propertyIndex = meta->indexOfProperty(name.constData());
    if (propertyIndex >= 0) {
        if (_gate.deniesPropertyRead(meta, name)) {
            return Absent;
        }
        QMetaProperty property = meta->property(propertyIndex);
        writable = property.isWritable() && !_gate.deniesPropertyWrite(meta, name);
        return Property;
    }
    for (int i = meta->methodCount() - 1; i >= 0; --i) {
        QMetaMethod method = meta->method(i);
        if (method.name() != name || method.access() == QMetaMethod::Private ||
            method.methodType() == QMetaMethod::Constructor) {
            continue;
        }
        if (method.methodType() == QMetaMethod::Signal) {
            return Signal;
        }
        return _gate.deniesMethod(meta, name) ? Absent : Method;
    }
    // not a QObject member: nothing of the wrapper's is forwarded (toString and the like come from the guard's own
    // Object.prototype)
    return Absent;
}

QScriptClass::QueryFlags GuardedObjectClass::queryProperty(const QScriptValue& object, const QScriptString& name,
                                                           QueryFlags flags, uint* id) {
    Q_UNUSED(flags);
    bool writable = false;
    const Kind kind = kindOf(object.data(), name.toString().toUtf8(), writable);
    *id = uint(kind);
    if (kind == Absent) {
        return QueryFlags();   // not there at all: no read, and a write would make nothing but an own property
    }
    // a write is always ours to handle: a refused one is logged and dropped rather than becoming an own property
    // that would shadow the live value
    return HandlesReadAccess | HandlesWriteAccess;
}

QScriptValue GuardedObjectClass::property(const QScriptValue& object, const QScriptString& name, uint id) {
    const QScriptValue raw = object.data();
    const QString member = name.toString();
    switch (Kind(id)) {
        case Method: {
            // a function bound to the real object, so the call runs on it; what it hands back is guarded too
            QScriptValue bundle = engine()->newObject();
            bundle.setProperty("raw", raw);
            bundle.setProperty("member", member);
            bundle.setProperty("guard", object);
            QScriptValue function = engine()->newFunction(forward);
            function.setData(bundle);
            return function;
        }
        case Property: {
            QScriptValue value = raw.property(member);
            return value.isQObject() ? wrap(value) : value;
        }
        case Signal:
            return raw.property(member);   // connect() and disconnect() live on the signal itself
        case Absent:
        default:
            return QScriptValue();
    }
}

void GuardedObjectClass::setProperty(QScriptValue& object, const QScriptString& name, uint id, const QScriptValue& value) {
    QScriptValue raw = object.data();
    const QByteArray member = name.toString().toUtf8();
    bool writable = false;
    const Kind kind = kindOf(raw, member, writable);
    if (kind != Property || !writable) {
        refuse(raw, member);
        return;
    }
    Q_UNUSED(id);
    raw.setProperty(name.toString(), value);
}

QScriptValue::PropertyFlags GuardedObjectClass::propertyFlags(const QScriptValue& object, const QScriptString& name, uint id) {
    Q_UNUSED(object);
    Q_UNUSED(name);
    return Kind(id) == Property ? QScriptValue::PropertyFlags() : QScriptValue::ReadOnly;
}

void GuardedObjectClass::refuse(const QScriptValue& raw, const QByteArray& member) const {
    QObject* object = raw.toQObject();
    const QString className = object ? QString::fromUtf8(object->metaObject()->className()) : QString("an object");
    qCWarning(scriptengine) << (_gate.refusalLine ? _gate.refusalLine(className, QString::fromUtf8(member))
                                                  : className + "." + QString::fromUtf8(member) + " is not available to this script");
}

QScriptValue GuardedObjectClass::forward(QScriptContext* context, QScriptEngine* engine) {
    QScriptValue bundle = context->callee().data();
    QScriptValue raw = bundle.property("raw");
    QScriptValue guard = bundle.property("guard");
    QScriptValue method = raw.property(bundle.property("member").toString());
    QScriptValueList arguments;
    for (int i = 0; i < context->argumentCount(); ++i) {
        arguments << context->argument(i);
    }
    QScriptValue result = method.call(raw, arguments);
    if (engine->hasUncaughtException()) {
        return result;   // the exception stays raised for the caller
    }
    auto guardClass = static_cast<GuardedObjectClass*>(guard.scriptClass());
    return (result.isQObject() && guardClass) ? guardClass->wrap(result) : result;
}
