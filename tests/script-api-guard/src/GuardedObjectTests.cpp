//
//  GuardedObjectTests.cpp
//  tests/script-api-guard
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The QtScript guard (GuardedObjectClass) holds the script API policy the way the WebAssembly host's gate does:
//  allowed members work, denied ones are absent, writes it refuses never land, an object an allowed call hands back
//  is guarded too, and nothing a script does on its side reaches the wrapper behind the guard.
//

#include "GuardedObjectTests.h"

#include <QtScript/QScriptEngine>

#include <GuardedObject.h>

QTEST_MAIN(GuardedObjectTests)

namespace {

class Other : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE int secret() { return 42; }
};

class Probe : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count WRITE setCount NOTIFY countChanged)
    Q_PROPERTY(QString label READ label CONSTANT)
    Q_PROPERTY(int limit READ limit WRITE setLimit)
public:
    int count() const { return _count; }
    void setCount(int count) { _count = count; emit countChanged(count); }
    QString label() const { return "the label"; }
    int limit() const { return _limit; }
    void setLimit(int limit) { _limit = limit; }
    Q_INVOKABLE int add(int a, int b) { return a + b; }
    Q_INVOKABLE int scale(int v, int by) { return v * by; }
    Q_INVOKABLE QObject* self() { return this; }
    Q_INVOKABLE QObject* makeOther() { return new Other(); }
    Q_INVOKABLE bool isSelf(QObject* object) { return object == this; }
    Q_INVOKABLE int countSelves(const QVariantList& objects) { int n = 0; for (const QVariant& v : objects) { n += v.value<QObject*>() == this; } return n; }
signals:
    void countChanged(int count);
    void secretChanged(const QString& secret);
    void pointed(const QString& id, int n);
private:
    int _count { 0 };
    int _limit { 0 };
};

struct Fixture {
    QScriptEngine engine;
    Probe probe;
    GuardedObjectClass* guard { nullptr };
    Fixture() {
        HostInterface::MemberGate gate;
        gate.allowsClass = [](const QMetaObject* meta) { return QByteArray(meta->className()) == "Probe"; };
        gate.deniesMethod = [](const QMetaObject*, const QByteArray& name) { return name == "scale"; };
        gate.deniesPropertyWrite = [](const QMetaObject*, const QByteArray& name) { return name == "count"; };
        gate.deniesPropertyRead = [](const QMetaObject*, const QByteArray& name) { return name == "label"; };
        gate.deniesSignal = [](const QMetaObject*, const QByteArray& name) { return name == "secretChanged"; };
        gate.checksDelivery = [](const QMetaObject*, const QByteArray& name) { return name == "pointed"; };
        gate.holdsBackDelivery = [](const QMetaObject*, const QByteArray& name, const QVariantList& arguments) {
            return name == "pointed" && arguments.value(0).toString() == "hidden";
        };
        gate.refusalLine = [](const QString& object, const QString& member) { return object + "." + member + " is for the visitor's own scripts"; };
        guard = new GuardedObjectClass(&engine, gate);   // owned by the engine's lifetime in the fixture
        QScriptValue raw = engine.newQObject(&probe, QScriptEngine::QtOwnership,
                                             QScriptEngine::ExcludeSuperClassMethods | QScriptEngine::ExcludeDeleteLater);
        engine.globalObject().setProperty("probe", guard->wrap(raw));
    }
    ~Fixture() { delete guard; }
    QString run(const QString& program) {
        QScriptValue result = engine.evaluate(program);
        if (engine.hasUncaughtException()) {
            QString message = "EXCEPTION: " + result.toString();
            engine.clearExceptions();
            return message;
        }
        return result.toString();
    }
};

} // namespace

#include "GuardedObjectTests.moc"

void GuardedObjectTests::testAllowedAndDenied() {
    Fixture f;
    QCOMPARE(f.run("probe.add(2, 3)"), QString("5"));
    QCOMPARE(f.run("typeof probe.scale"), QString("undefined"));
    QCOMPARE(f.run("String(probe.scale === undefined)"), QString("true"));
    QVERIFY(f.run("probe.scale(2, 3)").startsWith("EXCEPTION"));
    // a readable, unwritable property: the live value, and a write that never lands and never shadows it
    f.probe.setCount(7);
    QCOMPARE(f.run("probe.count"), QString("7"));
    QCOMPARE(f.run("probe.count = 9; probe.count"), QString("7"));
    QCOMPARE(f.probe.count(), 7);
    f.probe.setCount(8);
    QCOMPARE(f.run("probe.count"), QString("8"));
    // a writable property goes through
    QCOMPARE(f.run("probe.limit = 5; probe.limit"), QString("5"));
    QCOMPARE(f.probe.limit(), 5);
    // an unreadable property is absent
    QCOMPARE(f.run("typeof probe.label"), QString("undefined"));
    QCOMPARE(f.run("'label' in probe ? 'in' : 'out'"), QString("out"));
    // a signal connects and fires through the guard
    QCOMPARE(f.run("var seen = -1; probe.countChanged.connect(function (c) { seen = c; }); 'ok'"), QString("ok"));
    f.probe.setCount(3);
    QCOMPARE(f.run("seen"), QString("3"));
}

void GuardedObjectTests::testBypassAttempts() {
    Fixture f;
    // the wrapper is not the prototype, not enumerable, not reachable by any name
    QCOMPARE(f.run("String(Object.getPrototypeOf(probe) === Object.prototype)"), QString("true"));
    QCOMPARE(f.run("var names = []; for (var k in probe) { names.push(k); } names.length"), QString("0"));
    QCOMPARE(f.run("Object.keys(probe).length"), QString("0"));
    QCOMPARE(f.run("typeof probe['scale']"), QString("undefined"));
    QCOMPARE(f.run("typeof probe.data"), QString("undefined"));
    QCOMPARE(f.run("typeof probe.__proto__.scale"), QString("undefined"));
    QCOMPARE(f.run("typeof probe.constructor"), QString("function"));   // Object: not the wrapper's
    QCOMPARE(f.run("probe.constructor === Object ? 'Object' : 'other'"), QString("Object"));
    // the raw wrapper's own methods are not on the guard's chain
    QCOMPARE(f.run("typeof Object.getPrototypeOf(probe).add"), QString("undefined"));
    // a script may hang its own property on the guard (as it can on any object); that is its own function, not the
    // wrapper's method, which stays out of reach
    QCOMPARE(f.run("probe.scale = function () { return 'mine'; }; probe.scale(2, 3)"), QString("mine"));
    QCOMPARE(f.run("delete probe.scale; typeof probe.scale"), QString("undefined"));
    // the object a method hands back is the guard again, never the wrapper: self() returns this
    QCOMPARE(f.run("typeof probe.self().scale"), QString("undefined"));
    QCOMPARE(f.run("probe.self().add(1, 1)"), QString("2"));
    QCOMPARE(f.run("probe.self().count = 99; probe.count"), QString(f.probe.count() == 99 ? "99" : "0"));
    QCOMPARE(f.probe.count(), 0);
    // a guard handed back to the host goes as the object behind it (a timer to clearInterval, a sound to playSound)
    QCOMPARE(f.run("String(probe.isSelf(probe))"), QString("true"));
    QCOMPARE(f.run("String(probe.isSelf(probe.self()))"), QString("true"));
    QCOMPARE(f.run("probe.countSelves([probe, probe.self(), 1])"), QString("2"));
    // JSON and toString give nothing away
    QCOMPARE(f.run("JSON.stringify(probe)"), QString("{}"));
    QVERIFY(!f.run("String(probe)").startsWith("EXCEPTION"));
}

void GuardedObjectTests::testHandedBackObjects() {
    Fixture f;
    // a class the policy doesn't allow, handed back by an allowed call: an object with nothing in it
    QCOMPARE(f.run("var o = probe.makeOther(); typeof o"), QString("object"));
    QCOMPARE(f.run("typeof o.secret"), QString("undefined"));
    QVERIFY(f.run("o.secret()").startsWith("EXCEPTION"));
    QCOMPARE(f.run("Object.keys(o).length"), QString("0"));
}

void GuardedObjectTests::testDeniedSignals() {
    Fixture f;
    // a denied signal is not there by its name or its signature: it can be neither heard nor fired
    QCOMPARE(f.run("typeof probe.secretChanged"), QString("undefined"));
    QCOMPARE(f.run("typeof probe['secretChanged(QString)']"), QString("undefined"));
    QVERIFY(f.run("probe.secretChanged.connect(function () {})").startsWith("EXCEPTION"));
    // an allowed one is heard
    QCOMPARE(f.run("var heard = -1; probe.countChanged.connect(function (count) { heard = count; }); typeof probe.countChanged"),
             QString("function"));
    f.probe.setCount(5);
    QCOMPARE(f.run("heard"), QString("5"));
}

void GuardedObjectTests::testCheckedSignals() {
    Fixture f;
    // a signal checked at each delivery: connect and disconnect, each connected function hears only what the gate
    // lets through
    QCOMPARE(f.run("var heard = []; function hear(id, n) { heard.push(id + n); } probe.pointed.connect(hear); "
                   "typeof probe.pointed.connect"), QString("function"));
    emit f.probe.pointed("hidden", 1);
    emit f.probe.pointed("seen", 2);
    QCOMPARE(f.run("heard.join(',')"), QString("seen2"));
    // with a receiver, by function and by method name: `this` is the receiver
    QCOMPARE(f.run("var listener = { got: [], take: function (id, n) { this.got.push(id + n); } }; "
                   "probe.pointed.connect(listener, listener.take); probe.pointed.connect(listener, 'take'); 'ok'"),
             QString("ok"));
    emit f.probe.pointed("hidden", 3);
    emit f.probe.pointed("seen", 4);
    QCOMPARE(f.run("heard.join(',') + ' ' + listener.got.join(',')"), QString("seen2,seen4 seen4,seen4"));
    // disconnect finds what connect made: no more deliveries to that function; an unknown one is an error
    QCOMPARE(f.run("probe.pointed.disconnect(hear); probe.pointed.disconnect(listener, 'take'); 'ok'"), QString("ok"));
    emit f.probe.pointed("seen", 5);
    QCOMPARE(f.run("heard.join(',') + ' ' + listener.got.join(',')"), QString("seen2,seen4 seen4,seen4,seen5"));
    QVERIFY(f.run("probe.pointed.disconnect(hear)").startsWith("EXCEPTION"));
    QVERIFY(f.run("probe.pointed.connect(42)").startsWith("EXCEPTION"));
    // the signal itself is out of reach: it can't be fired
    QVERIFY(f.run("probe.pointed('seen', 6)").startsWith("EXCEPTION"));
    QCOMPARE(f.run("listener.got.length"), QString("3"));
    // a signal that isn't checked is the signal itself, as before
    QCOMPARE(f.run("typeof probe.countChanged"), QString("function"));
}
