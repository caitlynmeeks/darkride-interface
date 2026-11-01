//
//  GuardedObject.h
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The QtScript side of the script API policy (ScriptApiPolicy): a guard object in front of every global a world's
//  entity engine gets. A world's script sees the guard, never the wrapper QtScript made for the real QObject: the
//  guard answers each member by the policy's word (a forwarding function for an allowed method, the live value for
//  an allowed property, the signal itself; nothing at all for a denied member or a class the policy doesn't allow),
//  forwards writes it allows and logs the ones it doesn't, and wraps every object an allowed call hands back the
//  same way, so an object is guarded wherever it came from. The WebAssembly engine has the same gate in its host
//  (HostInterface::MemberGate); this is the same list, consulted the same way, on the other engine.
//

#ifndef hifi_GuardedObject_h
#define hifi_GuardedObject_h

#include <QtCore/QHash>
#include <QtCore/QPair>
#include <QtCore/QVector>
#include <QtScript/QScriptClass>
#include <QtScript/QScriptEngine>
#include <QtScript/QScriptValue>

#include <HostInterface.h>

class GuardedObjectClass : public QScriptClass {
public:
    GuardedObjectClass(QScriptEngine* engine, HostInterface::MemberGate gate);

    /// The guard in front of `raw`, a QObject wrapper the engine made (raw itself when it wraps no QObject).
    QScriptValue wrap(const QScriptValue& raw);

    QueryFlags queryProperty(const QScriptValue& object, const QScriptString& name, QueryFlags flags, uint* id) override;
    QScriptValue property(const QScriptValue& object, const QScriptString& name, uint id) override;
    void setProperty(QScriptValue& object, const QScriptString& name, uint id, const QScriptValue& value) override;
    QScriptValue::PropertyFlags propertyFlags(const QScriptValue& object, const QScriptString& name, uint id) override;
    QString name() const override { return "GuardedObject"; }

    const HostInterface::MemberGate& gate() const { return _gate; }

    /// The object behind a guard (or behind each guard in an array), for a value going back to the host.
    QScriptValue unwrap(const QScriptValue& value) const;

private:
    enum Kind { Absent = 0, Method = 1, Property = 2, Signal = 3 };
    /// A signal whose deliveries the gate checks (HostInterface::MemberGate::checksDelivery): the world's script gets
    /// connect and disconnect, never the signal itself, and each function it connects hears only the deliveries the
    /// gate lets through.
    QScriptValue guardSignal(const QScriptValue& guard, const QString& member);
    static QScriptValue connectChecked(QScriptContext* context, QScriptEngine* engine);
    static QScriptValue disconnectChecked(QScriptContext* context, QScriptEngine* engine);
    static QScriptValue deliverChecked(QScriptContext* context, QScriptEngine* engine);
    struct CheckedConnection {
        QScriptValue receiver;  // the `this` the script gave, or invalid
        QScriptValue function;  // the script's function
        QScriptValue checked;   // what is connected to the real signal in its place
    };
    // by the real object and the signal's name: what disconnect looks the script's function up in
    QHash<QPair<const QObject*, QString>, QVector<CheckedConnection>> _checkedConnections;
    /// What the name is on the real object, by the policy: Absent when the world's script may not have it.
    Kind kindOf(const QScriptValue& raw, const QByteArray& name, bool& writable) const;
    static QScriptValue forward(QScriptContext* context, QScriptEngine* engine);
    void refuse(const QScriptValue& raw, const QByteArray& member) const;

    HostInterface::MemberGate _gate;
};

#endif // hifi_GuardedObject_h
