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

#include <ScriptCaller.h>

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
            return _gate.deniesSignal && _gate.deniesSignal(meta, name) ? Absent : Signal;
        }
        return _gate.deniesMethod(meta, name) ? Absent : Method;
    }
    // not a QObject member: nothing of the wrapper's is forwarded (toString and the like come from the guard's own
    // Object.prototype)
    return Absent;
}

bool GuardedObjectClass::refusedAfterWorldLeft(const QScriptValue& raw, const QByteArray& member) const {
    if (!ScriptCaller::isRetired()) {
        return false;
    }
    const QObject* object = raw.toQObject();
    const QMetaObject* meta = object ? object->metaObject() : nullptr;
    if (meta && _gate.allowsAfterWorldLeft && _gate.allowsAfterWorldLeft(meta, member)) {
        return false;
    }
    ScriptCaller::logRetiredRefusal(meta ? meta->className() : "?", member.constData());
    return true;
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
            if (refusedAfterWorldLeft(raw, member.toUtf8())) {
                return QScriptValue();
            }
            QScriptValue value = raw.property(member);
            return value.isQObject() ? wrap(value) : value;
        }
        case Signal: {
            if (refusedAfterWorldLeft(raw, member.toUtf8())) {
                return QScriptValue();   // nothing to connect to
            }
            const QObject* sender = raw.toQObject();
            if (sender && _gate.checksDelivery && _gate.holdsBackDelivery &&
                _gate.checksDelivery(sender->metaObject(), member.toUtf8())) {
                return guardSignal(object, member);
            }
            return raw.property(member);   // connect() and disconnect() live on the signal itself
        }
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
    if (refusedAfterWorldLeft(raw, member)) {
        return;
    }
    Q_UNUSED(id);
    raw.setProperty(name.toString(), unwrap(value));
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

QScriptValue GuardedObjectClass::unwrap(const QScriptValue& value) const {
    // a guard handed back to the host (a timer to clearInterval, a sound to playSound) goes as the object behind it;
    // the host is not a script, so nothing is given away
    if (value.isObject() && value.scriptClass() == this) {
        return value.data();
    }
    if (value.isArray()) {
        const quint32 length = value.property("length").toUInt32();
        bool any = false;
        for (quint32 i = 0; i < length && !any; ++i) {
            QScriptValue item = value.property(i);
            any = item.isObject() && item.scriptClass() == this;
        }
        if (any) {
            QScriptValue copy = engine()->newArray(length);
            for (quint32 i = 0; i < length; ++i) {
                copy.setProperty(i, unwrap(value.property(i)));
            }
            return copy;
        }
    }
    return value;
}

QScriptValue GuardedObjectClass::guardSignal(const QScriptValue& guard, const QString& member) {
    QScriptValue bundle = engine()->newObject();
    bundle.setProperty("guard", guard);
    bundle.setProperty("member", member);
    QScriptValue signal = engine()->newObject();
    QScriptValue connect = engine()->newFunction(connectChecked);
    connect.setData(bundle);
    QScriptValue disconnect = engine()->newFunction(disconnectChecked);
    disconnect.setData(bundle);
    signal.setProperty("connect", connect, QScriptValue::ReadOnly | QScriptValue::Undeletable);
    signal.setProperty("disconnect", disconnect, QScriptValue::ReadOnly | QScriptValue::Undeletable);
    return signal;
}

namespace {

// connect(function), connect(receiver, function) or connect(receiver, "method"), as QtScript's own signals take
bool handlerOf(QScriptContext* context, QScriptValue& receiver, QScriptValue& function) {
    if (context->argumentCount() >= 2) {
        receiver = context->argument(0);
        function = context->argument(1);
        if (function.isString() && receiver.isObject()) {
            function = receiver.property(function.toString());
        }
    } else {
        function = context->argument(0);
    }
    return function.isFunction();
}

bool sameReceiver(const QScriptValue& a, const QScriptValue& b) {
    const bool aGiven = a.isObject();
    const bool bGiven = b.isObject();
    return aGiven == bGiven && (!aGiven || a.strictlyEquals(b));
}

} // namespace

QScriptValue GuardedObjectClass::connectChecked(QScriptContext* context, QScriptEngine* engine) {
    QScriptValue bundle = context->callee().data();
    QScriptValue guard = bundle.property("guard");
    auto guardClass = static_cast<GuardedObjectClass*>(guard.scriptClass());
    const QString member = bundle.property("member").toString();
    QScriptValue receiver;
    QScriptValue function;
    if (!guardClass || !handlerOf(context, receiver, function)) {
        return context->throwError(QScriptContext::TypeError, member + ".connect: the handler is not a function");
    }
    QScriptValue data = engine->newObject();
    data.setProperty("bundle", bundle);
    data.setProperty("receiver", receiver.isObject() ? receiver : QScriptValue());
    data.setProperty("function", function);
    QScriptValue checked = engine->newFunction(deliverChecked);
    checked.setData(data);
    QScriptValue raw = guard.data();
    QScriptValue signal = raw.property(member);
    QScriptValue result = signal.property("connect").call(signal, QScriptValueList() << checked);
    if (engine->hasUncaughtException()) {
        return result;
    }
    guardClass->_checkedConnections[qMakePair(static_cast<const QObject*>(raw.toQObject()), member)].append(
        { receiver.isObject() ? receiver : QScriptValue(), function, checked });
    return result;
}

QScriptValue GuardedObjectClass::disconnectChecked(QScriptContext* context, QScriptEngine* engine) {
    Q_UNUSED(engine);
    QScriptValue bundle = context->callee().data();
    QScriptValue guard = bundle.property("guard");
    auto guardClass = static_cast<GuardedObjectClass*>(guard.scriptClass());
    const QString member = bundle.property("member").toString();
    QScriptValue receiver;
    QScriptValue function;
    if (!guardClass || !handlerOf(context, receiver, function)) {
        return context->throwError(QScriptContext::TypeError, member + ".disconnect: the handler is not a function");
    }
    QScriptValue raw = guard.data();
    auto found = guardClass->_checkedConnections.find(qMakePair(static_cast<const QObject*>(raw.toQObject()), member));
    if (found != guardClass->_checkedConnections.end()) {
        for (int i = 0; i < found->size(); ++i) {
            const CheckedConnection& connection = found->at(i);
            if (connection.function.strictlyEquals(function) && sameReceiver(connection.receiver, receiver)) {
                QScriptValue signal = raw.property(member);
                QScriptValue result = signal.property("disconnect").call(signal, QScriptValueList() << connection.checked);
                found->remove(i);
                return result;
            }
        }
    }
    return context->throwError(member + ".disconnect: the function is not connected");
}

QScriptValue GuardedObjectClass::deliverChecked(QScriptContext* context, QScriptEngine* engine) {
    Q_UNUSED(engine);
    QScriptValue data = context->callee().data();
    QScriptValue bundle = data.property("bundle");
    QScriptValue guard = bundle.property("guard");
    auto guardClass = static_cast<GuardedObjectClass*>(guard.scriptClass());
    const QObject* sender = guard.data().toQObject();
    QScriptValueList values;
    QVariantList arguments;
    for (int i = 0; i < context->argumentCount(); ++i) {
        values << context->argument(i);
        arguments << context->argument(i).toVariant();
    }
    if (!guardClass || !sender ||
        guardClass->gate().holdsBackDelivery(sender->metaObject(), bundle.property("member").toString().toUtf8(), arguments)) {
        return QScriptValue();
    }
    return data.property("function").call(data.property("receiver"), values);
}

QScriptValue GuardedObjectClass::forward(QScriptContext* context, QScriptEngine* engine) {
    QScriptValue bundle = context->callee().data();
    QScriptValue raw = bundle.property("raw");
    QScriptValue guard = bundle.property("guard");
    auto guardClass = static_cast<GuardedObjectClass*>(guard.scriptClass());
    QScriptValue method = raw.property(bundle.property("member").toString());
    QScriptValueList arguments;
    for (int i = 0; i < context->argumentCount(); ++i) {
        arguments << (guardClass ? guardClass->unwrap(context->argument(i)) : context->argument(i));
    }
    // the guard is a world's script's: what the call runs knows it (ScriptCaller)
    ScriptCaller::WorldScope caller(true);
    if (guardClass && guardClass->refusedAfterWorldLeft(raw, bundle.property("member").toString().toUtf8())) {
        return QScriptValue();   // undefined: a world that was left gets nothing done
    }
    QScriptValue result = method.call(raw, arguments);
    if (engine->hasUncaughtException()) {
        return result;   // the exception stays raised for the caller
    }
    return (result.isQObject() && guardClass) ? guardClass->wrap(result) : result;
}
