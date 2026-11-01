//
//  HostInterface.h
//  libraries/wasm-scripting/src
//
//  The functions a module imports from the host (module name "darkride"), and the calls the host makes into
//  a module. The host side is reflection: an object's methods, properties and signals are reached by name
//  through its QMetaObject, with the values crossing as CBOR (ValueCodec), the objects as handles
//  (ObjectTable) and the signals through a relay (SignalRelay). Nothing here knows which objects exist.
//
//  The ABI (version 1). Every pointer is an offset in the module's exported memory; a byte range is a CBOR
//  item. A host function that returns bytes has the guest allocate them (the guest's exported
//  `alloc(size) -> ptr`) and returns `(ptr << 32) | len` as an i64; the guest frees them when done. A result of
//  0 is nothing (a method that returns nothing, a property write that worked): no bytes cross.
//
//    imports, module "darkride":
//      version() -> i32                                  this ABI's version
//      globals() -> i64                                  the registered globals, a map of name to handle
//      describe(ptr, len) -> i64                         a global's name or a handle record -> its description
//      call(object, name_ptr, name_len, args_ptr, args_len) -> i64
//                                                        invoke a method: the result, or {"@error": {type, message}}
//      get(object, name_ptr, name_len) -> i64            read a property: the value, or an error record
//      set(object, name_ptr, name_len, value_ptr, value_len) -> i64
//                                                        write a property: nothing, or an error record
//      connect(object, signal_ptr, signal_len) -> i32    subscribe: the connection id, 0 on failure (logged)
//      disconnect(connection)
//      release(object)                                   forget a handle
//      log(level, text_ptr, text_len)                    0 print, 1 info, 2 warning, 3 error, 4 debug
//      interrupted() -> i32                              1 when the running call should stop now (a stop, a time
//                                                        limit): asked often by a module compiled for cooperative
//                                                        interruption (WasmRuntime::Interruption), which then
//                                                        unwinds by itself
//
//    exports the host calls:
//      alloc(size) -> ptr, free(ptr, size)
//      script_start(), script_stop()
//      on_signal(connection, args_ptr, args_len)         a subscribed signal fired
//      on_callback(callback, args_ptr, args_len)         a guest function the host was given is called
//      call_callback(callback, args_ptr, args_len) -> i64
//                                                        the same, with the function's result returned packed
//                                                        (optional; the host frees the result through free)
//    (the arguments are placed through the guest's alloc and are the guest's to free)
//
//    the entity-script contract, exported by a module that hosts entity scripts (optional; each takes one
//    CBOR map placed through alloc, the module's to free):
//      entity_load(ptr, len) -> i64       {"entity": id, "url": url, "source": text or absent}; null, or an
//                                         error record, packed (the host frees it through free)
//      entity_unload(ptr, len)            {"entity": id}
//      entity_has(ptr, len) -> i32        {"entity": id, "method": name}: 1 when the method exists
//      entity_call(ptr, len) -> i64       {"entity": id, "method": name, "args": [...], "remoteCallerID"?: id};
//                                         the method's result packed, or 0
//      interrupted_entity() -> i64        the entity script that was running when the host last interrupted a
//                                         call (the time limit), as a packed text, taken once; 0 for none
//
//  A method is named by its name ("getEntityProperties"; the overloads are tried in order, the first whose
//  arguments convert wins), its signature ("add(int,int)") or its index from the description ("#12").
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_HostInterface_h
#define hifi_HostInterface_h

#include <cstdint>
#include <functional>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QCborArray>
#include <QtCore/QCborValue>
#include <QtCore/QHash>
#include <QtCore/QMetaMethod>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>
#include <QtCore/QVector>

#include "ObjectTable.h"
#include "WasmRuntime.h"

struct QMetaObject;
class SignalRelay;
class ValueCodec;

class HostInterface {
public:
    static const int32_t ABI_VERSION = 1;
    static const QString MODULE_NAME;  // "darkride"
    static const QString ERROR_KEY;    // "@error"

    enum LogLevel { Print = 0, Info = 1, Warning = 2, Error = 3, Debug = 4 };

    using Logger = std::function<void(int level, const QString& text)>;
    /// Told when a call into the module fails (a trap, a time limit, a missing export): the entry that failed
    /// and the error. The engine decides what to do with the script.
    using FailureHandler = std::function<void(const QString& entry, const QString& error)>;
    /// Told when a call into a cooperative module ended at the module's time limit: the module unwound by itself
    /// and runs on. `entity` is the entity script the module says was running (through its interrupted_entity
    /// export; empty when the script itself was, or the module does not say). Without one, the failure handler
    /// is told instead. The engine decides what to do with the script.
    using TimeLimitHandler = std::function<void(const QString& entry, const QString& entity, const QString& reason)>;

    /// The relay must live on the thread the instance is used from.
    HostInterface(WasmInstance& instance, ObjectTable& objects, ValueCodec& codec, SignalRelay& relay);
    ~HostInterface();

    void setLogger(Logger logger) { _logger = std::move(logger); }
    void setFailureHandler(FailureHandler handler) { _failureHandler = std::move(handler); }
    void setTimeLimitHandler(TimeLimitHandler handler) { _timeLimitHandler = std::move(handler); }
    /// The members a module may not use, by class, for an engine held to the script API policy (a world's entity
    /// scripts on a visitor's client). Consulted where the host describes an object and on every call, read, write
    /// and signal connection, so the gate holds whatever the guest does on its side. Unset: everything a module
    /// may call by the rules of callable().
    struct MemberGate {
        std::function<bool(const QMetaObject*)> allowsClass;    // default-deny: a class not allowed has no members
        std::function<bool(const QMetaObject*, const QByteArray&)> deniesMethod;
        std::function<bool(const QMetaObject*, const QByteArray&)> deniesPropertyWrite;
        std::function<bool(const QMetaObject*, const QByteArray&)> deniesPropertyRead;
        std::function<bool(const QMetaObject*, const QByteArray&)> deniesSignal;   // unset: an allowed class's every signal
        // a signal whose deliveries are checked one by one, and a delivery (its arguments) the script doesn't get;
        // unset: every delivery of an allowed signal
        std::function<bool(const QMetaObject*, const QByteArray&)> checksDelivery;
        std::function<bool(const QMetaObject*, const QByteArray&, const QVariantList&)> holdsBackDelivery;
        std::function<QString(const QString&, const QString&)> refusalLine;
    };
    void setMemberGate(MemberGate gate) { _gate = std::move(gate); }
    bool hasMemberGate() const { return bool(_gate.allowsClass); }

    /// Defines the imports on the instance. Before instantiate().
    void install();

    // --- the host's calls into the module (after instantiate) ---------------------------------------------
    bool hasGuestExport(const QString& name) const;
    /// script_start / script_stop; true when the export exists and returned normally.
    bool callScriptStart(QString& error);
    bool callScriptStop(QString& error);
    /// on_signal and on_callback. Failures also reach the failure handler.
    bool deliverSignal(int connection, const QVariantList& arguments, QString& error);
    bool deliverCallback(int32_t callback, const QVariantList& arguments, QString& error);
    /// on_callback with the function's result, through the module's call_callback export when it has one
    /// (the result is undefined otherwise).
    bool callCallback(int32_t callback, const QVariantList& arguments, QVariant& result, QString& error);
    /// An export taking (id, ptr, len): the payload encoded, allocated in the guest, the call made.
    bool callGuest(const QString& name, int32_t id, const QVariant& payload, QString& error);
    /// An export taking (ptr, len) and returning a packed result (an i64, 0 for none) or an i32 or nothing:
    /// the payload placed through alloc (the guest's to free), the call made, a packed result read back,
    /// freed through the guest's free and decoded into `result` (invalid when there is none; an i32 result
    /// is given as an int). Failures reach the failure handler.
    bool callWithPayload(const QString& name, const QVariant& payload, QVariant& result, QString& error);
    /// The same call with the result as the CBOR item the guest wrote (undefined for none; an i32 result as an
    /// integer): for a caller that keeps the order of a map's keys or reads nulls as the guest sent them.
    bool callWithPayload(const QString& name, const QVariant& payload, QCborValue& result, QString& error);

    // --- the operations behind the imports, also usable from C++ ------------------------------------------
    /// The registered globals: name to handle.
    QVariantMap globals() const;
    /// By a global's name or a handle; an invalid QVariant when there is no such object.
    QVariant describe(const QVariant& target) const;
    QVariant call(ObjectTable::Handle object, const QString& method, const QVariantList& arguments, QString& error);
    QVariant get(ObjectTable::Handle object, const QString& property, QString& error) const;
    bool set(ObjectTable::Handle object, const QString& property, const QVariant& value, QString& error);
    int connect(ObjectTable::Handle object, const QString& signal, QString& error);
    bool disconnect(int connection);
    void release(ObjectTable::Handle object);

    /// An error as the guest receives it: {"@error": {"type": type, "message": message}}.
    static QVariantMap errorRecord(const QString& type, const QString& message);
    static bool isErrorRecord(const QVariant& value);

    struct Statistics {
        uint64_t calls { 0 };
        uint64_t gets { 0 };
        uint64_t sets { 0 };
        uint64_t signalsDelivered { 0 };
        uint64_t callbacksDelivered { 0 };
        uint64_t bytesToGuest { 0 };
        uint64_t bytesFromGuest { 0 };
    };
    const Statistics& statistics() const { return _statistics; }

private:
    struct MethodCandidates {
        QVector<int> indices;
    };

    bool enterGuest(const QString& name, const std::vector<WasmValue>& args, std::vector<WasmValue>& results, QString& error);
    QByteArray readBytes(int32_t pointer, int32_t length, QString& error);
    /// Encodes and places the bytes in the guest's memory through its alloc; 0 with the error when it cannot.
    int64_t writeToGuest(const QVariant& value, QString& error);
    int64_t writeResult(const QVariant& value);
    bool callForResultBytes(const QString& name, const QVariant& payload, QByteArray& bytes, bool& hasBytes,
                            int32_t& small, bool& isSmall, QString& error);
    void log(int level, const QString& text) const;
    void fail(const QString& entry, const QString& error);

    const QVector<int>& candidates(const QMetaObject* meta, const QByteArray& name);
    /// Whether a module may call the method: the rules describe() gives the guest, held for every form of a call.
    static bool callable(const QMetaMethod& method);
    /// callable(), and the member gate's word on the method for this engine.
    bool allowed(const QMetaObject* meta, const QMetaMethod& method) const;
    /// Whether a gate is set (then every class is judged).
    bool gated(const QMetaObject* meta) const;
    /// The gate's word on the class, looked up once per class.
    bool classAllowed(const QMetaObject* meta) const;
    /// Logs the refusal and gives the line for the error.
    QString refuse(const QMetaObject* meta, const QByteArray& member) const;
    /// A description with the members the gate denies left out (writes denied: writable false).
    QVariantMap gatedDescription(const QMetaObject* meta, QVariantMap description) const;
    /// A method's parameter types, resolved once (a type no library has registered yet is looked up again next time).
    QVector<int> parameterTypes(const QMetaObject* meta, int methodIndex);
    /// A property's index by name, looked up once per class (-1 when it has none).
    int propertyIndex(const QMetaObject* meta, const QByteArray& name) const;
    QVariant getByName(ObjectTable::Handle handle, const QByteArray& propertyName, QString& error) const;
    /// The value as it came from the guest, decoded straight to the property's type.
    bool setWithCbor(ObjectTable::Handle handle, const QByteArray& name, const QCborValue& value, QString& error);
    /// The arguments as they came from the guest, each decoded straight to its parameter's type.
    QVariant callWithCbor(ObjectTable::Handle handle, const QByteArray& methodName, const QCborArray& arguments, QString& error);
    bool convertArguments(const QMetaMethod& method, const QVector<int>& types, const QCborArray& arguments,
                          QVariantList& converted, int& cost, QString& error);
    QVariant invoke(QObject* object, int methodIndex, const QVector<int>& types, QVariantList& converted, QString& error);

    WasmInstance& _instance;
    ObjectTable& _objects;
    ValueCodec& _codec;
    SignalRelay& _relay;
    /// After a call ended at the module's time limit: the entity script the module names as the one that was
    /// running (its interrupted_entity export, when it has one), cleared in the module.
    QString takeInterruptedEntity();

    Logger _logger;
    FailureHandler _failureHandler;
    TimeLimitHandler _timeLimitHandler;
    MemberGate _gate;
    mutable QHash<const QMetaObject*, bool> _classAllowed;
    bool _timeLimitHit { false };   // the call that just failed ended at the time limit, not by a stop or a trap
    // The JavaScript error type of the last failed call, as QtScript names the same failure: SyntaxError when every
    // candidate took more arguments than were given, TypeError for anything else
    QString _callErrorType;
    QString _timeLimitEntity;
    QHash<QPair<const QMetaObject*, QByteArray>, QVector<int>> _candidates;
    QHash<QPair<const QMetaObject*, int>, QVector<int>> _parameterTypes;
    mutable QHash<QPair<const QMetaObject*, QByteArray>, int> _propertyIndices;
    std::vector<WasmValue> _allocResults;
    Statistics _statistics;
};

#endif // hifi_HostInterface_h
