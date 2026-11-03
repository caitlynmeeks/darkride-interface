//
//  WasmEditFilterEngine.h
//  libraries/script-engine/src
//
//  An edit filter's script on the WebAssembly engine's JavaScript guest, behind the EntityEditFilterEngine
//  interface. The guest is instantiated once per filter and used on the thread that calls the filter, with no
//  event loop: the filter source goes in through the entity-script contract as a one-method entity script, and
//  each edit is an entity_call that returns its result on this thread, so the call is synchronous as it has
//  always been. No timers, no signals, no Script API (a filter has none on QtScript either); Entities is a plain
//  value with the four filter-type constants. A filter that runs past the call's time limit traps, and the
//  filter refuses every edit from then on, where the QtScript engine hangs the entity server.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_WasmEditFilterEngine_h
#define hifi_WasmEditFilterEngine_h

#include <chrono>
#include <memory>

#include <QtCore/QCborValue>
#include <QtCore/QObject>
#include <QtCore/QUrl>
#include <QtCore/QVariant>

#include <EntityEditFilterEngine.h>

/// The guest's Script object for a filter: what the shim asks at its start, and nothing a filter could use.
class EditFilterScriptObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString fileName READ fileName CONSTANT)
public:
    QString url;
    QString fileName() const { return url; }
    /// No source: the filter arrives through the entity-script contract, as an entities engine's scripts do.
    Q_INVOKABLE QVariantMap scriptSource() const;
    /// Entities with the four filter-type constants, as the QtScript filter engine defines them.
    Q_INVOKABLE QVariantMap scriptGlobalValues() const;
    Q_INVOKABLE QString resolvePathFrom(const QString& path, const QString& parent) const {
        return QUrl(parent.isEmpty() ? url : parent).resolved(QUrl(path)).toString();
    }
};

class WasmEditFilterEngine : public EntityEditFilterEngine {
public:
    explicit WasmEditFilterEngine(const EntityItemID& entityID,
                                  std::chrono::milliseconds callTimeout = std::chrono::milliseconds(5000));
    ~WasmEditFilterEngine() override;

    bool load(const QString& source, const QString& url, QString& error) override;
    QVariantMap flags() override { return _flags; }
    Result call(EntityItemProperties& in, EntityItemProperties& out, int filterType,
                const EntityItemProperties* original, const EntityItemProperties* zone, const AABox* zoneBox) override;

    /// The engine for a filter (EntityEditFilters::setEngineFactory): this one.
    static EntityEditFilterEnginePointer makeForSetting(const EntityItemID& entityID);

private:
    struct Guest;  // the module, the instance, the object table, the codec, the relay and the host interface

    bool callFilterMethod(const QString& method, const QVariantList& arguments, QCborValue& result, QString& error);

    EntityItemID _entityID;
    QString _url;
    std::chrono::milliseconds _callTimeout;
    std::unique_ptr<Guest> _guest;
    EditFilterScriptObject _script;
    QVariantMap _flags;
    bool _broken { false };  // a trap (the time limit) or a failed entry: the filter refuses every edit from then on
    QString _failure;
};

#endif // hifi_WasmEditFilterEngine_h
