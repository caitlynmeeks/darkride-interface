//
//  TestObjects.h
//  tests/wasm-scripting/src
//
//  A QObject with the kinds of members the scripting interfaces have: invokables (one overloaded), a slot, a
//  non-public invokable, properties (notifying, constant, enum-typed, vec3-typed), signals with the argument
//  types the relay must carry, and an enum. Shared by the codec, description and relay tests.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_TestObjects_h
#define hifi_TestObjects_h

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUuid>
#include <QtCore/QVariant>

#include <RegisteredMetaTypes.h>

/// Another class, for the checks that a handle of one class is not accepted as another, and for the objects a
/// method makes new for each call (counted, so a test can tell they are deleted).
class Other : public QObject {
    Q_OBJECT
public:
    explicit Other(QObject* parent = nullptr) : QObject(parent) { ++alive; }
    ~Other() override { --alive; }
    static inline int alive { 0 };
};

/// An enum Qt doesn't know (no Q_ENUM, in a class that is not a QObject), as RefreshRateManager's were before
/// Performance mirrored them: a method returning one can't be called from a script, on either engine.
struct PlainLevels {
    enum Level { Low, High = 7 };
};

class Probe : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count WRITE setCount NOTIFY countChanged)
    Q_PROPERTY(QString label READ label CONSTANT)
    Q_PROPERTY(Mode mode READ mode WRITE setMode)
    Q_PROPERTY(glm::vec3 position READ position WRITE setPosition NOTIFY positionChanged)
public:
    enum Mode { Idle, Running, Stopped = 10 };
    Q_ENUM(Mode)

    explicit Probe(QObject* parent = nullptr) : QObject(parent) {}

    int count() const { return _count; }
    QString label() const { return "probe"; }
    Mode mode() const { return _mode; }
    void setMode(Mode mode) { _mode = mode; }
    glm::vec3 position() const { return _position; }

    Q_INVOKABLE int add(int a, int b) { return a + b; }
    Q_INVOKABLE QString add(const QString& a, const QString& b) { return a + b; }
    Q_INVOKABLE glm::vec3 scale(const glm::vec3& v, float by) { return v * by; }
    Q_INVOKABLE QObject* self() { return this; }
    Q_INVOKABLE QVariant echo(const QVariant& value) { return value; }
    Q_INVOKABLE QStringList join(const QStringList& names, const QString& extra) { return QStringList(names) << extra; }
    Q_INVOKABLE QUuid uuidOf(const QString& text) { return QUuid(text); }
    Q_INVOKABLE int countOf(QObject* object) { Probe* probe = qobject_cast<Probe*>(object); return probe ? probe->count() : -1; }
    Q_INVOKABLE int countOfProbe(const Probe* probe) { return probe ? probe->count() : -1; }
    Q_INVOKABLE QVariantMap describeSelf() { QVariantMap m; m["count"] = _count; m["mode"] = int(_mode); return m; }
    Q_INVOKABLE Other* makeOther() { return new Other(); }
    // an enum result: Qt's own (Q_ENUM) reaches a script as its number; one Qt doesn't know can't be carried
    Q_INVOKABLE Mode currentMode() const { return _mode; }
    Q_INVOKABLE void useMode(Mode mode) { _mode = mode; }
    Q_INVOKABLE PlainLevels::Level plainLevel() const { return PlainLevels::High; }

public slots:
    void setPosition(const glm::vec3& position) {
        _position = position;
        emit positionChanged(position);
    }
    void setCount(int count) {
        _count = count;
        emit countChanged(count);
    }
    void reset() { setCount(0); }
    void send(const QString& name, bool flag) { emit named(name, flag); }
    void carry(const QVariant& value) { emit carried(value); }
    void sendObject(QObject* object) { emit objectSent(object); }
    void ping() { emit bare(); }

signals:
    void countChanged(int count);
    void positionChanged(const glm::vec3& position);
    void named(const QString& name, bool flag);
    void bare();
    void carried(const QVariant& value);
    void objectSent(QObject* object);

protected:
    // QtScript shows a script protected invokables (and hides private ones): so does the WebAssembly engine
    Q_INVOKABLE int guarded() { return 9; }

private:
    Q_INVOKABLE void hidden() {}

    int _count { 0 };
    Mode _mode { Idle };
    glm::vec3 _position { 0.0f };
};


#endif // hifi_TestObjects_h
