//
//  HostInterface.cpp
//  libraries/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HostInterface.h"

#include <ScriptCaller.h>
#include <QtCore/QCborValue>
#include <QtCore/QDebug>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaMethod>
#include <QtCore/QMetaObject>
#include <QtCore/QMetaProperty>
#include <QtCore/QObject>

#include "MetaObjectDescription.h"
#include "SignalRelay.h"
#include "ValueCodec.h"
#include "WasmRuntime.h"

const QString HostInterface::MODULE_NAME = QStringLiteral("darkride");
const QString HostInterface::ERROR_KEY = QStringLiteral("@error");

namespace {

const QString ALLOC_EXPORT = QStringLiteral("alloc");
const QString FREE_EXPORT = QStringLiteral("free");
const QString CALL_CALLBACK_EXPORT = QStringLiteral("call_callback");
const QString INTERRUPTED_ENTITY_EXPORT = QStringLiteral("interrupted_entity");
const int MAX_INVOKE_ARGUMENTS = 10;  // QMetaMethod::invoke's limit

int64_t pack(int32_t pointer, int32_t length) {
    return (int64_t(uint32_t(pointer)) << 32) | int64_t(uint32_t(length));
}

QString typeNameOf(int typeId) {
    const char* name = QMetaType::typeName(typeId);
    return name ? QString::fromUtf8(name) : QString("type %1").arg(typeId);
}

// An enum of the method's own class that Qt hasn't registered by name (Q_ENUM registers it only on first use, so
// "Probe::Mode" can be unknown to QMetaType::type): carried as its number, as QtScript carries it (it, too, looks
// the name up among the class's enumerators).
bool isOwnEnum(const QMetaMethod& method, QByteArray name) {
    const QMetaObject* meta = method.enclosingMetaObject();
    if (!meta) {
        return false;
    }
    int scope = name.lastIndexOf("::");
    if (scope != -1) {
        if (name.left(scope) != meta->className()) {
            return false;
        }
        name = name.mid(scope + 2);
    }
    return meta->indexOfEnumerator(name.constData()) != -1;
}

// A parameter's type id. moc leaves an enum declared with its unqualified name ("Mode") unresolved; the
// registered name is the qualified one ("Probe::Mode"), so that is tried with the method's class scope. A
// pointer to a const object ("const AudioEffectOptions*") is registered, if at all, without the const.
int parameterTypeOf(const QMetaMethod& method, int index) {
    int type = method.parameterType(index);
    if (type != QMetaType::UnknownType) {
        return type;
    }
    QByteArray name = method.parameterTypes().at(index);
    if (name.startsWith("const ") && name.endsWith('*')) {
        name = name.mid(6);
    }
    type = QMetaType::type(name.constData());
    if (type == QMetaType::UnknownType && !name.contains("::") && method.enclosingMetaObject()) {
        type = QMetaType::type((QByteArray(method.enclosingMetaObject()->className()) + "::" + name).constData());
    }
    if (type == QMetaType::UnknownType && isOwnEnum(method, name)) {
        type = QMetaType::Int;
    }
    return type;
}

// The return type's id, the same way: a method returning its class's own enum ("Mode") was refused as a type the host
// interface cannot carry (Performance.getRefreshRateRegime, once it returned Performance's own enum, 2026-10-26).
int returnTypeOf(const QMetaMethod& method) {
    int type = method.returnType();
    if (type != QMetaType::UnknownType) {
        return type;
    }
    QByteArray name = method.typeName();
    if (!name.contains("::") && method.enclosingMetaObject()) {
        type = QMetaType::type((QByteArray(method.enclosingMetaObject()->className()) + "::" + name).constData());
    }
    if (type == QMetaType::UnknownType && isOwnEnum(method, name)) {
        type = QMetaType::Int;
    }
    return type;
}

} // namespace

HostInterface::HostInterface(WasmInstance& instance, ObjectTable& objects, ValueCodec& codec, SignalRelay& relay) :
    _instance(instance), _objects(objects), _codec(codec), _relay(relay) {
}

HostInterface::~HostInterface() {
    _relay.disconnectAll();
}

QVariantMap HostInterface::errorRecord(const QString& type, const QString& message) {
    QVariantMap error;
    error["type"] = type;
    error["message"] = message;
    QVariantMap record;
    record[ERROR_KEY] = error;
    return record;
}

bool HostInterface::isErrorRecord(const QVariant& value) {
    return value.userType() == QMetaType::QVariantMap && value.toMap().contains(ERROR_KEY);
}

void HostInterface::log(int level, const QString& text) const {
    if (_logger) {
        _logger(level, text);
    } else {
        qDebug().noquote() << "[script]" << text;
    }
}

void HostInterface::fail(const QString& entry, const QString& error) {
    if (_timeLimitHit) {
        // the module ran past its time limit and unwound by itself: it runs on, and the engine hears which script
        // was running rather than that the module failed
        _timeLimitHit = false;
        const QString entity = _timeLimitEntity;
        _timeLimitEntity.clear();
        if (_timeLimitHandler) {
            _timeLimitHandler(entry, entity, error);
            return;
        }
    }
    if (_failureHandler) {
        _failureHandler(entry, error);
    } else {
        log(Error, entry + ": " + error);
    }
}

// --- memory ------------------------------------------------------------------------------------------------

QByteArray HostInterface::readBytes(int32_t pointer, int32_t length, QString& error) {
    if (length < 0) {
        error = "negative length";
        return QByteArray();
    }
    if (length == 0) {
        return QByteArray();
    }
    QByteArray bytes = _instance.readMemory(uint64_t(uint32_t(pointer)), size_t(length));
    if (bytes.size() != length) {
        error = QString("bad pointer: %1 bytes at %2 are outside the module's memory").arg(length).arg(uint32_t(pointer));
        return QByteArray();
    }
    _statistics.bytesFromGuest += uint64_t(length);
    return bytes;
}

int64_t HostInterface::writeToGuest(const QVariant& value, QString& error) {
    QByteArray bytes = _codec.encode(value);
    std::vector<WasmValue>& results = _allocResults; // alloc calls no host function, so this is never in use twice
    if (!_instance.call(ALLOC_EXPORT, { int32_t(bytes.size()) }, results, error)) {
        error = "the module's alloc failed: " + error;
        return 0;
    }
    if (results.empty() || !std::holds_alternative<int32_t>(results[0])) {
        error = "the module's alloc returned no pointer";
        return 0;
    }
    int32_t pointer = std::get<int32_t>(results[0]);
    if (!_instance.writeMemory(uint64_t(uint32_t(pointer)), bytes)) {
        error = QString("the module's alloc returned a bad pointer (%1 for %2 bytes)").arg(uint32_t(pointer)).arg(bytes.size());
        return 0;
    }
    _statistics.bytesToGuest += uint64_t(bytes.size());
    return pack(pointer, int32_t(bytes.size()));
}

// A result for the guest; when the bytes cannot be placed, the failure is logged and 0 is returned. Nothing (a void
// method's result) is 0 too, with no call into the guest: a guest reads an empty result as undefined.
int64_t HostInterface::writeResult(const QVariant& value) {
    if (!value.isValid()) {
        return 0;
    }
    QString error;
    int64_t packed = writeToGuest(value, error);
    if (packed == 0 && !error.isEmpty()) {
        // a stop requested while the module was inside a call makes its alloc trap: expected, not an error
        log(error.contains("was stopped") ? Debug : Error, "cannot return a value to the module: " + error);
    }
    return packed;
}

// --- the imports ---------------------------------------------------------------------------------------------

void HostInterface::install() {
    using Args = const std::vector<WasmValue>&;
    using Results = std::vector<WasmValue>&;
    const WasmType I32 = WasmType::I32;
    const WasmType I64 = WasmType::I64;

    _instance.defineHostFunction(MODULE_NAME, "version", { {}, { I32 } }, [](WasmInstance&, Args, Results results) {
        results.push_back(ABI_VERSION);
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "globals", { {}, { I64 } }, [this](WasmInstance&, Args, Results results) {
        results.push_back(writeResult(globals()));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "describe", { { I32, I32 }, { I64 } }, [this](WasmInstance&, Args args, Results results) {
        QString error;
        QByteArray bytes = readBytes(std::get<int32_t>(args[0]), std::get<int32_t>(args[1]), error);
        if (!error.isEmpty()) {
            return "describe: " + error;
        }
        QVariant target;
        if (!_codec.decode(bytes, target, error)) {
            return "describe: " + error;
        }
        QVariant description = describe(target);
        results.push_back(writeResult(description.isValid() ? description : QVariant::fromValue(nullptr)));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "call", { { I32, I32, I32, I32, I32 }, { I64 } }, [this](WasmInstance&, Args args, Results results) {
        QString error;
        QByteArray name = readBytes(std::get<int32_t>(args[1]), std::get<int32_t>(args[2]), error);
        QByteArray argumentBytes = error.isEmpty() ? readBytes(std::get<int32_t>(args[3]), std::get<int32_t>(args[4]), error) : QByteArray();
        if (!error.isEmpty()) {
            return "call: " + error;
        }
        QCborArray arguments;
        if (!argumentBytes.isEmpty()) {
            QCborParserError parseError;
            QCborValue value = QCborValue::fromCbor(argumentBytes, &parseError);
            if (parseError.error != QCborError::NoError) {
                return QString("call: the arguments are not CBOR: malformed CBOR at byte %1: %2").arg(parseError.offset).arg(parseError.errorString());
            }
            if (value.isArray()) {
                arguments = value.toArray();
            }
        }
        QVariant result = callWithCbor(ObjectTable::Handle(std::get<int32_t>(args[0])), name, arguments, error);
        results.push_back(writeResult(error.isEmpty() ? result : QVariant(errorRecord(_callErrorType, error))));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "get", { { I32, I32, I32 }, { I64 } }, [this](WasmInstance&, Args args, Results results) {
        QString error;
        QByteArray name = readBytes(std::get<int32_t>(args[1]), std::get<int32_t>(args[2]), error);
        if (!error.isEmpty()) {
            return "get: " + error;
        }
        QVariant value = getByName(ObjectTable::Handle(std::get<int32_t>(args[0])), name, error);
        results.push_back(writeResult(error.isEmpty() ? value : QVariant(errorRecord("PropertyError", error))));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "set", { { I32, I32, I32, I32, I32 }, { I64 } }, [this](WasmInstance&, Args args, Results results) {
        QString error;
        QByteArray name = readBytes(std::get<int32_t>(args[1]), std::get<int32_t>(args[2]), error);
        QByteArray valueBytes = error.isEmpty() ? readBytes(std::get<int32_t>(args[3]), std::get<int32_t>(args[4]), error) : QByteArray();
        if (!error.isEmpty()) {
            return "set: " + error;
        }
        QCborParserError parseError;
        QCborValue value = QCborValue::fromCbor(valueBytes, &parseError);
        if (parseError.error != QCborError::NoError) {
            return QString("set: the value is not CBOR: malformed CBOR at byte %1: %2").arg(parseError.offset).arg(parseError.errorString());
        }
        bool ok = setWithCbor(ObjectTable::Handle(std::get<int32_t>(args[0])), name, value, error);
        // a write that worked returns nothing (no bytes cross); one that failed, its error record
        results.push_back(writeResult(ok ? QVariant() : QVariant(errorRecord("PropertyError", error))));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "connect", { { I32, I32, I32 }, { I32 } }, [this](WasmInstance&, Args args, Results results) {
        QString error;
        QByteArray signal = readBytes(std::get<int32_t>(args[1]), std::get<int32_t>(args[2]), error);
        if (!error.isEmpty()) {
            return "connect: " + error;
        }
        int connection = connect(ObjectTable::Handle(std::get<int32_t>(args[0])), QString::fromUtf8(signal), error);
        if (connection == 0) {
            log(Warning, "connect: " + error);
        }
        results.push_back(int32_t(connection));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "disconnect", { { I32 }, {} }, [this](WasmInstance&, Args args, Results) {
        disconnect(std::get<int32_t>(args[0]));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "release", { { I32 }, {} }, [this](WasmInstance&, Args args, Results) {
        release(ObjectTable::Handle(std::get<int32_t>(args[0])));
        return QString();
    });

    // A module that stops by itself (the JavaScript guest, from QuickJS's interrupt handler) asks this often: 1 when
    // the running call should stop now.
    _instance.defineHostFunction(MODULE_NAME, "interrupted", { {}, { I32 } }, [](WasmInstance& instance, Args, Results results) {
        results.push_back(int32_t(instance.interruptRequested() ? 1 : 0));
        return QString();
    });

    _instance.defineHostFunction(MODULE_NAME, "log", { { I32, I32, I32 }, {} }, [this](WasmInstance&, Args args, Results) {
        QString error;
        QByteArray text = readBytes(std::get<int32_t>(args[1]), std::get<int32_t>(args[2]), error);
        if (!error.isEmpty()) {
            return "log: " + error;
        }
        log(std::get<int32_t>(args[0]), QString::fromUtf8(text));
        return QString();
    });
}

// --- calls into the module ----------------------------------------------------------------------------------

// An entry point of the module. A module that stops by itself (cooperative interruption) unwinds and returns
// normally when told to stop; the call fails then all the same, with the reason, as a trap would have.
bool HostInterface::enterGuest(const QString& name, const std::vector<WasmValue>& args, std::vector<WasmValue>& results,
                               QString& error) {
    if (!_instance.call(name, args, results, error)) {
        return false;
    }
    QString interruption = _instance.takeInterruptionReason();
    if (!interruption.isEmpty()) {
        error = interruption;
        if (!_instance.isStopRequested()) {
            // the time limit, not a stop: the module is whole; which script was running is its to say
            _timeLimitHit = true;
            _timeLimitEntity = takeInterruptedEntity();
        }
        return false;
    }
    return true;
}

QString HostInterface::takeInterruptedEntity() {
    if (!hasGuestExport(INTERRUPTED_ENTITY_EXPORT)) {
        return QString();
    }
    std::vector<WasmValue> results;
    QString error;
    if (!_instance.call(INTERRUPTED_ENTITY_EXPORT, {}, results, error) || results.empty() || !std::holds_alternative<int64_t>(results[0])) {
        return QString();
    }
    int64_t packed = std::get<int64_t>(results[0]);
    if (packed == 0) {
        return QString();
    }
    int32_t pointer = int32_t(uint32_t(uint64_t(packed) >> 32));
    int32_t length = int32_t(uint32_t(uint64_t(packed) & 0xffffffffu));
    QByteArray bytes = readBytes(pointer, length, error);
    std::vector<WasmValue> ignored;
    QString freeError;
    _instance.call(FREE_EXPORT, { pointer, length }, ignored, freeError);
    if (!error.isEmpty()) {
        return QString();
    }
    return QCborValue::fromCbor(bytes).toString();
}

bool HostInterface::hasGuestExport(const QString& name) const {
    return _instance.hasExport(name);
}

bool HostInterface::callScriptStart(QString& error) {
    std::vector<WasmValue> results;
    if (!enterGuest("script_start", {}, results, error)) {
        fail("script_start", error);
        return false;
    }
    return true;
}

bool HostInterface::callScriptStop(QString& error) {
    if (!_instance.hasExport("script_stop")) {
        return true;
    }
    std::vector<WasmValue> results;
    if (!enterGuest("script_stop", {}, results, error)) {
        fail("script_stop", error);
        return false;
    }
    return true;
}

bool HostInterface::callGuest(const QString& name, int32_t id, const QVariant& payload, QString& error) {
    int64_t packed = writeToGuest(payload, error);
    if (packed == 0 && !error.isEmpty()) {
        fail(name, error);
        return false;
    }
    std::vector<WasmValue> results;
    int32_t pointer = int32_t(uint32_t(uint64_t(packed) >> 32));
    int32_t length = int32_t(uint32_t(uint64_t(packed) & 0xffffffffu));
    if (!enterGuest(name, { id, pointer, length }, results, error)) {
        fail(name, error);
        return false;
    }
    return true;
}

bool HostInterface::callWithPayload(const QString& name, const QVariant& payload, QVariant& result, QString& error) {
    result = QVariant();
    QByteArray bytes;
    bool hasBytes = false;
    int32_t small = 0;
    bool isSmall = false;
    if (!callForResultBytes(name, payload, bytes, hasBytes, small, isSmall, error)) {
        return false;
    }
    if (isSmall) {
        result = QVariant(small);
        return true;
    }
    if (!hasBytes) {
        return true;
    }
    return _codec.decode(bytes, result, error);
}

bool HostInterface::callWithPayload(const QString& name, const QVariant& payload, QCborValue& result, QString& error) {
    result = QCborValue(QCborValue::Undefined);
    QByteArray bytes;
    bool hasBytes = false;
    int32_t small = 0;
    bool isSmall = false;
    if (!callForResultBytes(name, payload, bytes, hasBytes, small, isSmall, error)) {
        return false;
    }
    if (isSmall) {
        result = QCborValue(qint64(small));
        return true;
    }
    if (!hasBytes) {
        return true;
    }
    QCborParserError parseError;
    result = QCborValue::fromCbor(bytes, &parseError);
    if (parseError.error != QCborError::NoError) {
        error = "the result of " + name + " is not CBOR: " + parseError.errorString();
        return false;
    }
    return true;
}

// The call behind both: the payload placed through alloc, the export called, a packed result read back and freed
// through the guest's free. `hasBytes` says a result was read (an empty one included); `isSmall` that the export
// returned an i32 instead.
bool HostInterface::callForResultBytes(const QString& name, const QVariant& payload, QByteArray& bytes, bool& hasBytes,
                                       int32_t& small, bool& isSmall, QString& error) {
    bytes = QByteArray();
    hasBytes = false;
    isSmall = false;
    int64_t packed = writeToGuest(payload, error);
    if (packed == 0 && !error.isEmpty()) {
        fail(name, error);
        return false;
    }
    std::vector<WasmValue> results;
    int32_t pointer = int32_t(uint32_t(uint64_t(packed) >> 32));
    int32_t length = int32_t(uint32_t(uint64_t(packed) & 0xffffffffu));
    if (!enterGuest(name, { pointer, length }, results, error)) {
        fail(name, error);
        return false;
    }
    if (results.empty()) {
        return true;
    }
    if (std::holds_alternative<int32_t>(results[0])) {
        small = std::get<int32_t>(results[0]);
        isSmall = true;
        return true;
    }
    if (!std::holds_alternative<int64_t>(results[0])) {
        return true;
    }
    int64_t resultPacked = std::get<int64_t>(results[0]);
    if (resultPacked == 0) {
        return true;
    }
    int32_t resultPointer = int32_t(uint32_t(uint64_t(resultPacked) >> 32));
    int32_t resultLength = int32_t(uint32_t(uint64_t(resultPacked) & 0xffffffffu));
    bytes = readBytes(resultPointer, resultLength, error);
    std::vector<WasmValue> ignored;
    QString freeError;
    _instance.call(FREE_EXPORT, { resultPointer, resultLength }, ignored, freeError);
    if (!error.isEmpty()) {
        return false;
    }
    hasBytes = true;
    _statistics.bytesFromGuest += uint64_t(bytes.size());
    return true;
}

bool HostInterface::deliverSignal(int connection, const QVariantList& arguments, QString& error) {
    ++_statistics.signalsDelivered;
    return callGuest("on_signal", int32_t(connection), arguments, error);
}

bool HostInterface::callCallback(int32_t callback, const QVariantList& arguments, QVariant& result, QString& error) {
    result = QVariant();
    if (!hasGuestExport(CALL_CALLBACK_EXPORT)) {
        return deliverCallback(callback, arguments, error);
    }
    ++_statistics.callbacksDelivered;
    int64_t packed = writeToGuest(arguments, error);
    if (packed == 0 && !error.isEmpty()) {
        fail(CALL_CALLBACK_EXPORT, error);
        return false;
    }
    std::vector<WasmValue> results;
    int32_t pointer = int32_t(uint32_t(uint64_t(packed) >> 32));
    int32_t length = int32_t(uint32_t(uint64_t(packed) & 0xffffffffu));
    if (!enterGuest(CALL_CALLBACK_EXPORT, { callback, pointer, length }, results, error)) {
        fail(CALL_CALLBACK_EXPORT, error);
        return false;
    }
    if (results.empty() || !std::holds_alternative<int64_t>(results[0])) {
        return true;  // no result
    }
    int64_t resultPacked = std::get<int64_t>(results[0]);
    if (resultPacked == 0) {
        return true;
    }
    int32_t resultPointer = int32_t(uint32_t(uint64_t(resultPacked) >> 32));
    int32_t resultLength = int32_t(uint32_t(uint64_t(resultPacked) & 0xffffffffu));
    QByteArray bytes = readBytes(resultPointer, resultLength, error);
    std::vector<WasmValue> ignored;
    QString freeError;
    _instance.call(FREE_EXPORT, { resultPointer, resultLength }, ignored, freeError);
    if (!error.isEmpty()) {
        return false;
    }
    _statistics.bytesFromGuest += uint64_t(bytes.size());
    return _codec.decode(bytes, result, error);
}

bool HostInterface::deliverCallback(int32_t callback, const QVariantList& arguments, QString& error) {
    ++_statistics.callbacksDelivered;
    return callGuest("on_callback", callback, arguments, error);
}

// --- the operations -------------------------------------------------------------------------------------------

QVariantMap HostInterface::globals() const {
    QVariantMap handles;
    for (const QString& name : _objects.globalNames()) {
        handles.insert(name, int(_objects.global(name)));
    }
    return handles;
}

QVariant HostInterface::describe(const QVariant& target) const {
    QObject* object = nullptr;
    if (target.userType() == QMetaType::QString) {
        object = _objects.object(_objects.global(target.toString()));
    } else if (target.userType() == QMetaType::QObjectStar) {
        object = target.value<QObject*>();
    } else if (target.canConvert<int>()) {
        object = _objects.object(ObjectTable::Handle(target.toInt()));
    }
    if (!object) {
        return QVariant();
    }
    QVariant description = MetaObjectDescription::describe(object);
    if (gated(object->metaObject())) {
        return gatedDescription(object->metaObject(), description.toMap());
    }
    return description;
}

bool HostInterface::gated(const QMetaObject* meta) const {
    return meta && bool(_gate.allowsClass);
}

bool HostInterface::classAllowed(const QMetaObject* meta) const {
    auto found = _classAllowed.constFind(meta);
    if (found != _classAllowed.constEnd()) {
        return found.value();
    }
    const bool allowed = _gate.allowsClass(meta);
    _classAllowed.insert(meta, allowed);
    return allowed;
}

bool HostInterface::allowed(const QMetaObject* meta, const QMetaMethod& method) const {
    if (!callable(method)) {
        return false;
    }
    if (!gated(meta)) {
        return true;
    }
    // a signal is connected to, not called (callable() keeps it out of calls)
    if (method.methodType() == QMetaMethod::Signal) {
        return classAllowed(meta) && !(_gate.deniesSignal && _gate.deniesSignal(meta, method.name()));
    }
    return classAllowed(meta) && !_gate.deniesMethod(meta, method.name());
}

QString HostInterface::refuse(const QMetaObject* meta, const QByteArray& member) const {
    const QString line = _gate.refusalLine ? _gate.refusalLine(QString::fromUtf8(meta->className()), QString::fromUtf8(member))
                                           : QString("%1%2 is not available to this script").arg(QString::fromUtf8(meta->className()), member.isEmpty() ? QString() : "." + QString::fromUtf8(member));
    log(Warning, line);
    return line;
}

bool HostInterface::refusedAfterWorldLeft(const QMetaObject* meta, const QByteArray& member) const {
    if (!gated(meta) || !ScriptCaller::isRetired()) {
        return false;
    }
    if (_gate.allowsAfterWorldLeft && _gate.allowsAfterWorldLeft(meta, member)) {
        return false;
    }
    ScriptCaller::logRetiredRefusal(meta->className(), member.constData());
    return true;
}

QVariantMap HostInterface::gatedDescription(const QMetaObject* meta, QVariantMap description) const {
    if (!classAllowed(meta)) {
        // the class name alone: nothing to call, read, write or connect; logged, so a content class missing from
        // the list shows up in testing
        refuse(meta, QByteArray());
        QVariantMap bare;
        bare["class"] = description.value("class");
        return bare;
    }
    QVariantList methods;
    for (const QVariant& entry : description.value("methods").toList()) {
        if (!_gate.deniesMethod(meta, entry.toMap().value("name").toString().toUtf8())) {
            methods.append(entry);
        }
    }
    description["methods"] = methods;
    QVariantList signalList;
    for (const QVariant& entry : description.value("signals").toList()) {
        if (!_gate.deniesSignal || !_gate.deniesSignal(meta, entry.toMap().value("name").toString().toUtf8())) {
            signalList.append(entry);
        }
    }
    description["signals"] = signalList;
    QVariantList properties;
    for (const QVariant& entry : description.value("properties").toList()) {
        QVariantMap property = entry.toMap();
        const QByteArray name = property.value("name").toString().toUtf8();
        if (_gate.deniesPropertyRead(meta, name)) {
            continue;
        }
        if (_gate.deniesPropertyWrite(meta, name)) {
            property["writable"] = false;
        }
        properties.append(property);
    }
    description["properties"] = properties;
    return description;
}

const QVector<int>& HostInterface::candidates(const QMetaObject* meta, const QByteArray& name) {
    auto key = qMakePair(meta, name);
    auto found = _candidates.find(key);
    if (found != _candidates.end()) {
        return found.value();
    }
    // the most derived class's overloads first: an engine's own method for a module (setTimeout taking a
    // callback) is tried before the base's of the same name (setTimeout taking a QScriptValue)
    QVector<int> indices;
    for (int i = meta->methodCount() - 1; i >= 0; --i) {
        QMetaMethod method = meta->method(i);
        if (allowed(meta, method) && method.name() == name) {
            indices.append(i);
        }
    }
    return _candidates.insert(key, indices).value();
}

// The rules describe() gives the guest, enforced for every form of a call (a plain name, "#index", a signature):
// a module that does not go through the shim must not reach what the shim never shows. A private method and a
// constructor are not a script's; a signal is not callable; and nothing QObject itself declares is, deleteLater
// above all (an object the application owns, deleted under it).
bool HostInterface::callable(const QMetaMethod& method) {
    return method.access() != QMetaMethod::Private && method.methodType() != QMetaMethod::Constructor &&
           method.methodType() != QMetaMethod::Signal && method.enclosingMetaObject() != &QObject::staticMetaObject &&
           method.name() != "deleteLater";
}

namespace {

bool isIntegralType(int type) {
    switch (type) {
        case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong: case QMetaType::ULongLong:
        case QMetaType::Short: case QMetaType::UShort: case QMetaType::Long: case QMetaType::ULong:
        case QMetaType::Char: case QMetaType::UChar: case QMetaType::SChar:
            return true;
        default:
            return false;
    }
}

bool isFloatingType(int type) {
    return type == QMetaType::Double || type == QMetaType::Float;
}

bool isScriptValueType(int type) {
    return qstrcmp(QMetaType::typeName(type), "QScriptValue") == 0;
}

const int SURPLUS_ARGUMENT_COST = 3;  // an argument an overload ignores costs as much as the worst coercion

// How far a value is from a parameter's type: 0 for the type itself or its family, more for a coercion.
// With overloads, the one whose arguments cost least is called, as QtScript scores its candidates.
int conversionCost(int from, int type) {
    if (type == QMetaType::QVariant || from == type) {
        return 0;
    }
    if (isIntegralType(from) && isIntegralType(type)) {
        return 0;
    }
    if (isFloatingType(from) && isFloatingType(type)) {
        return 0;
    }
    if ((isIntegralType(from) || isFloatingType(from)) && (isIntegralType(type) || isFloatingType(type))) {
        return 1;
    }
    if (type == QMetaType::QString) {
        if (from == QMetaType::QByteArray) {
            return 1;
        }
        return (isIntegralType(from) || isFloatingType(from) || from == QMetaType::Bool) ? 2 : 3;
    }
    if (type == QMetaType::Bool) {
        return 3;
    }
    if (from == QMetaType::QString) {
        return 2;  // text to a number, a uuid, a url, an enum key, a colour name
    }
    if (from == QMetaType::QVariantMap || from == QMetaType::QVariantList || from == QMetaType::QObjectStar) {
        return 1;  // a record to a struct, a list to a container, a handle to an object
    }
    return 2;
}

} // namespace

QVector<int> HostInterface::parameterTypes(const QMetaObject* meta, int methodIndex) {
    auto key = qMakePair(meta, methodIndex);
    auto found = _parameterTypes.constFind(key);
    if (found != _parameterTypes.constEnd()) {
        return found.value();
    }
    QMetaMethod method = meta->method(methodIndex);
    QVector<int> types;
    bool complete = true;
    for (int i = 0; i < method.parameterCount(); ++i) {
        int type = parameterTypeOf(method, i);
        complete = complete && type != QMetaType::UnknownType;
        types.append(type);
    }
    if (complete) {
        _parameterTypes.insert(key, types);
    }
    return types;
}

bool HostInterface::convertArguments(const QMetaMethod& method, const QVector<int>& types, const QCborArray& arguments,
                                     QVariantList& converted, int& cost, QString& error) {
    const int count = types.size();
    const int given = int(arguments.size());
    cost = 0;
    converted.clear();
    if (count > MAX_INVOKE_ARGUMENTS) {
        error = QString("%1 takes %2 parameters, more than a call can carry").arg(QString::fromUtf8(method.methodSignature())).arg(count);
        return false;
    }
    if (given < count) {
        error = QString("%1 takes %2 arguments, %3 given").arg(QString::fromUtf8(method.methodSignature())).arg(count).arg(given);
        return false;
    }
    // surplus arguments are dropped, as QtScript drops them, but an overload that takes them all is preferred
    cost = SURPLUS_ARGUMENT_COST * (given - count);
    converted.reserve(count);
    for (int i = 0; i < count; ++i) {
        const int type = types[i];
        const QCborValue argument = arguments.at(i);
        if (type == QMetaType::UnknownType) {
            error = QString("%1: parameter %2 has a type the host interface cannot carry (%3)")
                .arg(QString::fromUtf8(method.methodSignature())).arg(i + 1).arg(QString::fromUtf8(method.parameterTypes().at(i)));
            return false;
        }
        if (isScriptValueType(type)) {
            // only the QtScript engine has such values: the method is the QtScript engine's (its overload or twin
            // for this engine, when it has one, is a candidate of its own)
            error = QString("%1 takes a QtScript value as parameter %2, which a module cannot give")
                .arg(QString::fromUtf8(method.methodSignature())).arg(i + 1);
            return false;
        }
        // scored by the type the argument has as a plain value, as QtScript scores its candidates; converted from
        // the guest's CBOR straight to the parameter's type
        const int from = _codec.genericTypeOf(argument);
        cost += conversionCost(from, type);
        if (type == QMetaType::QVariant || from == type) {
            converted.append(_codec.fromCbor(argument));
            continue;
        }
        bool ok = true;
        QVariant value = _codec.fromCbor(argument, type, &ok);
        if (!ok) {
            error = QString("%1: argument %2 cannot be converted to %3")
                .arg(QString::fromUtf8(method.methodSignature())).arg(i + 1).arg(typeNameOf(type));
            return false;
        }
        converted.append(value);
    }
    return true;
}

QVariant HostInterface::invoke(QObject* object, int methodIndex, const QVector<int>& types, QVariantList& converted, QString& error) {
    const QMetaObject* meta = object->metaObject();
    QMetaMethod method = meta->method(methodIndex);
    const int count = types.size();
    QGenericArgument generic[MAX_INVOKE_ARGUMENTS];
    for (int i = 0; i < count; ++i) {
        const int type = types[i];
        // QGenericArgument keeps the name pointer and the data pointer: both outlive the invoke below
        generic[i] = QGenericArgument(QMetaType::typeName(type), type == QMetaType::QVariant ? &converted[i] : converted[i].constData());
    }

    const int returnType = returnTypeOf(method);
    bool invoked = false;
    QVariant result;
    if (returnType == QMetaType::Void) {
        invoked = method.invoke(object, Qt::DirectConnection, generic[0], generic[1], generic[2], generic[3], generic[4],
                                generic[5], generic[6], generic[7], generic[8], generic[9]);
    } else if (returnType == QMetaType::UnknownType) {
        error = QString("%1 returns a type the host interface cannot carry (%2)")
            .arg(QString::fromUtf8(method.methodSignature())).arg(QString::fromUtf8(method.typeName()));
        return QVariant();
    } else {
        void* buffer = QMetaType::create(returnType);
        // the method's own spelling of the type: QMetaMethod::invoke compares it with the name given here
        QGenericReturnArgument returnArgument(method.typeName(), buffer);
        invoked = method.invoke(object, Qt::DirectConnection, returnArgument, generic[0], generic[1], generic[2], generic[3],
                                generic[4], generic[5], generic[6], generic[7], generic[8], generic[9]);
        if (invoked) {
            result = returnType == QMetaType::QVariant ? *static_cast<QVariant*>(buffer) : QVariant(returnType, buffer);
        }
        QMetaType::destroy(returnType, buffer);
    }
    if (!invoked) {
        error = QString("%1 could not be invoked").arg(QString::fromUtf8(method.methodSignature()));
        return QVariant();
    }
    return result;
}

QVariant HostInterface::call(ObjectTable::Handle handle, const QString& methodName, const QVariantList& arguments, QString& error) {
    QCborArray encoded;
    for (const QVariant& argument : arguments) {
        encoded.append(_codec.toCbor(argument));
    }
    return callWithCbor(handle, methodName.toUtf8(), encoded, error);
}

QVariant HostInterface::callWithCbor(ObjectTable::Handle handle, const QByteArray& methodName, const QCborArray& arguments, QString& error) {
    ++_statistics.calls;
    // a gated host is a world's script's: what the call runs knows it (ScriptCaller)
    ScriptCaller::WorldScope caller(hasMemberGate());
    // a failure is a TypeError unless every candidate wanted more arguments than were given: QtScript's
    // "too few arguments in call to ..." is a SyntaxError
    _callErrorType = QStringLiteral("TypeError");
    QObject* object = _objects.object(handle);
    if (!object) {
        error = _objects.contains(handle) ? QString("the object behind handle %1 (%2) no longer exists").arg(handle).arg(_objects.className(handle))
                                          : QString("unknown object handle %1").arg(handle);
        return QVariant();
    }
    const QMetaObject* meta = object->metaObject();
    const QByteArray& name = methodName;
    QVector<int> indices;
    if (name.startsWith('#')) {
        bool ok = false;
        int index = name.mid(1).toInt(&ok);
        if (!ok || index < 0 || index >= meta->methodCount() || !allowed(meta, meta->method(index))) {
            error = (ok && index >= 0 && index < meta->methodCount() && callable(meta->method(index)))
                        ? refuse(meta, meta->method(index).name())
                        : QString("%1 has no method %2").arg(QString::fromUtf8(meta->className()), QString::fromUtf8(methodName));
            return QVariant();
        }
        indices.append(index);
    } else if (name.contains('(')) {
        int index = meta->indexOfMethod(QMetaObject::normalizedSignature(name.constData()).constData());
        if (index < 0 || !allowed(meta, meta->method(index))) {
            error = (index >= 0 && callable(meta->method(index)))
                        ? refuse(meta, meta->method(index).name())
                        : QString("%1 has no method %2").arg(QString::fromUtf8(meta->className()), QString::fromUtf8(methodName));
            return QVariant();
        }
        indices.append(index);
    } else {
        indices = candidates(meta, name);
        if (indices.isEmpty()) {
            error = (gated(meta) && (!classAllowed(meta) || _gate.deniesMethod(meta, name)))
                        ? refuse(meta, name)
                        : QString("%1 has no method %2").arg(QString::fromUtf8(meta->className()), QString::fromUtf8(methodName));
            return QVariant();
        }
    }
    // the overload whose arguments convert at the least cost; the first found wins a tie
    int bestIndex = -1;
    int bestCost = 0;
    QVariantList bestArguments;
    QString firstError;
    QVector<int> bestTypes;
    bool everyFailureTooFew = true;
    for (int index : indices) {
        QVariantList converted;
        int cost = 0;
        QString candidateError;
        QVector<int> types = parameterTypes(meta, index);
        if (int(arguments.size()) >= types.size()) {
            everyFailureTooFew = false;  // this candidate fails, if it does, on a type, not on the count
        }
        if (convertArguments(meta->method(index), types, arguments, converted, cost, candidateError)) {
            if (bestIndex < 0 || cost < bestCost) {
                bestIndex = index;
                bestCost = cost;
                bestArguments = converted;
                bestTypes = types;
                if (cost == 0) {
                    break;
                }
            }
        } else if (firstError.isEmpty()) {
            firstError = candidateError;
        }
    }
    if (bestIndex >= 0 && refusedAfterWorldLeft(meta, meta->method(bestIndex).name())) {
        return QVariant();  // undefined: a world that was left gets nothing done
    }
    if (bestIndex < 0) {
        if (everyFailureTooFew) {
            _callErrorType = QStringLiteral("SyntaxError");
        }
        error = indices.size() == 1 ? firstError
                                    : QString("no overload of %1.%2 takes these %3 arguments (%4)")
                                          .arg(QString::fromUtf8(meta->className()), QString::fromUtf8(methodName)).arg(arguments.size()).arg(firstError);
        return QVariant();
    }
    return invoke(object, bestIndex, bestTypes, bestArguments, error);
}

int HostInterface::propertyIndex(const QMetaObject* meta, const QByteArray& name) const {
    auto key = qMakePair(meta, name);
    auto found = _propertyIndices.constFind(key);
    if (found != _propertyIndices.constEnd()) {
        return found.value();
    }
    int index = meta->indexOfProperty(name.constData());
    _propertyIndices.insert(key, index);
    return index;
}

QVariant HostInterface::get(ObjectTable::Handle handle, const QString& propertyName, QString& error) const {
    return getByName(handle, propertyName.toUtf8(), error);
}

QVariant HostInterface::getByName(ObjectTable::Handle handle, const QByteArray& propertyName, QString& error) const {
    ++const_cast<HostInterface*>(this)->_statistics.gets;
    QObject* object = _objects.object(handle);
    if (!object) {
        error = QString("unknown or dead object handle %1").arg(handle);
        return QVariant();
    }
    const QMetaObject* meta = object->metaObject();
    int index = propertyIndex(meta, propertyName);
    if (index < 0) {
        error = QString("%1 has no property %2").arg(QString::fromUtf8(meta->className()), QString::fromUtf8(propertyName));
        return QVariant();
    }
    QMetaProperty property = meta->property(index);
    if (!property.isReadable()) {
        error = QString("%1.%2 is not readable").arg(QString::fromUtf8(meta->className()), QString::fromUtf8(propertyName));
        return QVariant();
    }
    if (gated(meta) && (!classAllowed(meta) || _gate.deniesPropertyRead(meta, propertyName))) {
        error = refuse(meta, propertyName);
        return QVariant();
    }
    if (refusedAfterWorldLeft(meta, propertyName)) {
        return QVariant();
    }
    return property.read(object);
}

bool HostInterface::set(ObjectTable::Handle handle, const QString& propertyName, const QVariant& value, QString& error) {
    return setWithCbor(handle, propertyName.toUtf8(), _codec.toCbor(value), error);
}

bool HostInterface::setWithCbor(ObjectTable::Handle handle, const QByteArray& name, const QCborValue& value, QString& error) {
    ++_statistics.sets;
    QObject* object = _objects.object(handle);
    if (!object) {
        error = QString("unknown or dead object handle %1").arg(handle);
        return false;
    }
    const QMetaObject* meta = object->metaObject();
    const QString propertyName = QString::fromUtf8(name);
    int index = propertyIndex(meta, name);
    if (index < 0) {
        error = QString("%1 has no property %2").arg(QString::fromUtf8(meta->className()), propertyName);
        return false;
    }
    QMetaProperty property = meta->property(index);
    if (!property.isWritable()) {
        error = QString("%1.%2 is read-only").arg(QString::fromUtf8(meta->className()), propertyName);
        return false;
    }
    if (gated(meta) && (!classAllowed(meta) || _gate.deniesPropertyWrite(meta, name))) {
        error = refuse(meta, name);
        return false;
    }
    if (refusedAfterWorldLeft(meta, name)) {
        return true;  // dropped, as a refused write is on QtScript
    }
    QVariant typed;
    const int type = property.userType();
    if (property.isEnumType()) {
        // QMetaProperty::write takes an int for an enum property (it resolves the enum by its unqualified type
        // name, which fails for a nested enum), so the value becomes its integer here: a key, with or without
        // the class scope, or a number
        QMetaEnum enumerator = property.enumerator();
        if (value.isString()) {
            QByteArray key = value.toString().toUtf8();
            int scope = key.lastIndexOf("::");
            if (scope >= 0) {
                key = key.mid(scope + 2);
            }
            bool ok = false;
            int number = property.isFlagType() ? enumerator.keysToValue(key.constData(), &ok) : enumerator.keyToValue(key.constData(), &ok);
            if (!ok) {
                error = QString("%1.%2: \"%3\" is not a value of %4").arg(QString::fromUtf8(meta->className()), propertyName, value.toString(), QString::fromUtf8(enumerator.name()));
                return false;
            }
            typed = QVariant(number);
        } else {
            bool ok = true;
            typed = _codec.fromCbor(value, QMetaType::Int, &ok);
            if (!ok) {
                error = QString("%1.%2: the value cannot be converted to %3").arg(QString::fromUtf8(meta->className()), propertyName, QString::fromUtf8(enumerator.name()));
                return false;
            }
        }
    } else if (type == QMetaType::QVariant || _codec.genericTypeOf(value) == type) {
        typed = _codec.fromCbor(value);
    } else {
        // from the guest's CBOR straight to the property's type
        bool ok = true;
        typed = _codec.fromCbor(value, type, &ok);
        if (!ok) {
            error = QString("%1.%2: the value cannot be converted to %3").arg(QString::fromUtf8(meta->className()), propertyName, typeNameOf(type));
            return false;
        }
    }
    if (!property.write(object, typed)) {
        error = QString("%1.%2 could not be set").arg(QString::fromUtf8(meta->className()), propertyName);
        return false;
    }
    return true;
}

int HostInterface::connect(ObjectTable::Handle handle, const QString& signal, QString& error) {
    QObject* object = _objects.object(handle);
    if (!object) {
        error = QString("unknown or dead object handle %1").arg(handle);
        return 0;
    }
    if (gated(object->metaObject()) && !classAllowed(object->metaObject())) {
        error = refuse(object->metaObject(), signal.toUtf8());
        return 0;
    }
    const QMetaObject* meta = object->metaObject();
    QByteArray name;
    if (gated(meta)) {
        // the signal the relay would join, by whichever name it is given (its name, its signature, its index)
        int index = -1;
        if (signal.startsWith('#')) {
            index = signal.mid(1).toInt();
        } else if (signal.contains('(')) {
            index = meta->indexOfSignal(QMetaObject::normalizedSignature(signal.toUtf8().constData()).constData());
        }
        name = (index >= 0 && index < meta->methodCount()) ? meta->method(index).name() : signal.toUtf8();
        if (_gate.deniesSignal && _gate.deniesSignal(meta, name)) {
            error = refuse(meta, name);
            return 0;
        }
        if (refusedAfterWorldLeft(meta, name)) {
            error = QString("%1.%2: the world was left").arg(QString::fromUtf8(meta->className()), QString::fromUtf8(name));
            return 0;
        }
    }
    // a signal the gate checks at each delivery (one about an entity the world may not see doesn't arrive)
    const bool checkEach = gated(meta) && _gate.checksDelivery && _gate.holdsBackDelivery && _gate.checksDelivery(meta, name);
    return _relay.connect(object, signal, [this, checkEach, meta, name](int connection, const QVariantList& arguments) {
        if (checkEach && _gate.holdsBackDelivery(meta, name, arguments)) {
            return;
        }
        QString deliveryError;
        deliverSignal(connection, arguments, deliveryError);
    }, error);
}

bool HostInterface::disconnect(int connection) {
    return _relay.disconnect(connection);
}

void HostInterface::release(ObjectTable::Handle handle) {
    // a registered global (Entities, MyAvatar, the engine itself) stays: every script of the engine shares the table
    if (_objects.isGlobal(handle)) {
        return;
    }
    _objects.release(handle);
}
