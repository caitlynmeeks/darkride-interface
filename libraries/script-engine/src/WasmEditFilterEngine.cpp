//
//  WasmEditFilterEngine.cpp
//  libraries/script-engine/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "WasmEditFilterEngine.h"

#include <QtCore/QCborMap>
#include <QtCore/QJsonValue>

#include <EntityTree.h>
#include <HostInterface.h>
#include <ObjectTable.h>
#include <PlainScriptValue.h>
#include <PlainScriptValueConversions.h>
#include <QtScriptEditFilterEngine.h>
#include <SignalRelay.h>
#include <ValueCodec.h>
#include <WasmRuntime.h>

#include "ScriptEngineLogging.h"
#include "WasmScriptEngine.h"

struct WasmEditFilterEngine::Guest {
    std::shared_ptr<WasmModule> module;
    std::unique_ptr<WasmInstance> instance;
    ObjectTable objects;
    ValueCodec codec { &objects };
    SignalRelay relay;
    std::unique_ptr<HostInterface> host;
};

namespace {

// The filter source as a one-method entity script: the global filter function and its flag properties come back
// as the script's methods. The source goes inside a function body, so its `function filter` is a local there
// (and a `filter = ...` without var a global, found all the same); the newline keeps a trailing line comment
// from eating the rest. The function is called with `this` null, as the QtScript engine calls it with no object.
const QString WRAPPER_HEAD = QStringLiteral("(function () {\n");
const QString WRAPPER_TAIL = QStringLiteral(
    "\n;var __filter = (typeof filter === 'function') ? filter : null;\n"
    "this.filter = function () { return __filter.apply(null, Array.prototype.slice.call(arguments, 1)); };\n"
    "this.flags = function () {\n"
    "    if (!__filter) { return null; }\n"
    "    var flags = {};\n"
    "    ['wantsToFilterAdd', 'wantsToFilterEdit', 'wantsToFilterPhysics', 'wantsToFilterDelete',\n"
    "     'wantsOriginalProperties', 'wantsZoneProperties'].forEach(function (name) {\n"
    "        if (__filter[name] !== undefined) { flags[name] = __filter[name]; }\n"
    "    });\n"
    "    return flags;\n"
    "};\n"
    "})");

}  // namespace

QVariantMap EditFilterScriptObject::scriptSource() const {
    QVariantMap program;
    program["fileName"] = url;
    return program;
}

QVariantMap EditFilterScriptObject::scriptGlobalValues() const {
    QVariantMap entities;
    entities["ADD_FILTER_TYPE"] = (int)EntityTree::FilterType::Add;
    entities["EDIT_FILTER_TYPE"] = (int)EntityTree::FilterType::Edit;
    entities["PHYSICS_FILTER_TYPE"] = (int)EntityTree::FilterType::Physics;
    entities["DELETE_FILTER_TYPE"] = (int)EntityTree::FilterType::Delete;
    QVariantMap values;
    values["Entities"] = entities;
    return values;
}

WasmEditFilterEngine::WasmEditFilterEngine(const EntityItemID& entityID, std::chrono::milliseconds callTimeout) :
    _entityID(entityID),
    _callTimeout(callTimeout) {
}

WasmEditFilterEngine::~WasmEditFilterEngine() {
    if (_guest && _guest->host && _guest->instance) {
        QString error;
        _guest->host->callScriptStop(error);
    }
}

bool WasmEditFilterEngine::load(const QString& source, const QString& url, QString& error) {
    _url = url;
    _guest = std::make_unique<Guest>();
    _guest->module = WasmScriptEngine::javascriptGuestModule(error);
    if (!_guest->module) {
        return false;
    }
    WasmInstance::Limits limits;
    limits.callTimeout = _callTimeout;
    _guest->instance = std::make_unique<WasmInstance>(_guest->module, limits);
    _guest->host = std::make_unique<HostInterface>(*_guest->instance, _guest->objects, _guest->codec, _guest->relay);
    HostInterface& host = *_guest->host;
    host.setLogger([this](int level, const QString& text) {
        if (level == HostInterface::Error) {
            qCCritical(scriptengine) << "Edit filter" << _url << ":" << qPrintable(text);
        } else if (level == HostInterface::Warning) {
            qCWarning(scriptengine) << "Edit filter" << _url << ":" << qPrintable(text);
        } else {
            qCInfo(scriptengine) << "Edit filter" << _url << ":" << qPrintable(text);
        }
    });
    host.setFailureHandler([this](const QString& entry, const QString& failure) {
        _broken = true;
        _failure = entry + ": " + failure;
        qCCritical(scriptengine) << "Edit filter" << _url << "failed and refuses every edit from now on:" << qPrintable(_failure);
    });
    host.install();
    _guest->instance->enableWasi();
    _script.url = url;
    _guest->objects.registerGlobal("Script", &_script);
    if (!_guest->instance->instantiate(error)) {
        return false;
    }
    std::vector<WasmValue> results;
    if (_guest->instance->hasExport("_initialize") && !_guest->instance->call("_initialize", {}, results, error)) {
        return false;
    }
    if (!host.callScriptStart(error)) {
        return false;
    }
    if (_broken) {
        error = _failure;
        return false;
    }

    // the filter as an entity script: a syntax error or a throw at load is an error record
    QVariantMap payload;
    payload["entity"] = _entityID.toString();
    payload["url"] = url;
    payload["source"] = WRAPPER_HEAD + source + WRAPPER_TAIL;
    QVariant loaded;
    if (!host.callWithPayload("entity_load", payload, loaded, error)) {
        return false;
    }
    if (HostInterface::isErrorRecord(loaded)) {
        const QVariantMap record = loaded.toMap().value(HostInterface::ERROR_KEY).toMap();
        error = "[" + record.value("type").toString() + "] " + record.value("message").toString() + " in " + url;
        qCCritical(scriptengine) << qPrintable(error);
        return false;
    }
    // the function and its flags
    QCborValue flags;
    if (!callFilterMethod("flags", QVariantList(), flags, error)) {
        return false;
    }
    if (!flags.isMap()) {
        error = "Filter function specified but not found";
        return false;
    }
    _flags = _guest->codec.fromCbor(flags).toMap();
    return true;
}

bool WasmEditFilterEngine::callFilterMethod(const QString& method, const QVariantList& arguments, QCborValue& result, QString& error) {
    if (_broken) {
        error = _failure;
        return false;
    }
    QVariantMap payload;
    payload["entity"] = _entityID.toString();
    payload["method"] = method;
    payload["args"] = arguments;
    if (!_guest->host->callWithPayload("entity_call", payload, result, error)) {
        // a trap (the time limit among them) leaves the guest in no state to run again
        _broken = true;
        if (_failure.isEmpty()) {
            _failure = error;
        }
        return false;
    }
    return true;
}

EntityEditFilterEngine::Result WasmEditFilterEngine::call(EntityItemProperties& in, EntityItemProperties& out, int filterType,
                                                           const EntityItemProperties* original, const EntityItemProperties* zone,
                                                           const AABox* zoneBox) {
    Result result;
    if (_broken) {
        result.kind = Result::Failed;
        result.error = _failure;
        return result;
    }
    PlainScriptEngine plain;
    const PlainScriptValue inValue = in.copyToScriptValue(&plain, false, true, true);
    result.inAsJson = QJsonValue::fromVariant(inValue.toVariant());

    // the arguments as the QtScript engine gives them: the edit, the type, the original properties when asked
    // for, and the zone's (with its bounding box) as the fourth, padded with undefined when the third is absent
    QVariantList args;
    args << QVariant::fromValue(inValue.toCbor()) << filterType;
    if (original) {
        args << QVariant::fromValue(original->copyToScriptValue(&plain, false, true, true).toCbor());
    }
    if (zone) {
        PlainScriptValue zoneValue = zone->copyToScriptValue(&plain, false, true, true);
        if (zoneBox) {
            PlainScriptValue boundingBox = plain.newObject();
            boundingBox.setProperty("brn", vec3ToScriptValue(&plain, zoneBox->getCorner()));
            boundingBox.setProperty("tfl", vec3ToScriptValue(&plain, zoneBox->calcTopFarLeft()));
            boundingBox.setProperty("center", vec3ToScriptValue(&plain, zoneBox->calcCenter()));
            boundingBox.setProperty("dimensions", vec3ToScriptValue(&plain, zoneBox->getDimensions()));
            zoneValue.setProperty("boundingBox", boundingBox);
        }
        const int EXPECTED_ARGS = 3;
        if (args.size() < EXPECTED_ARGS) {
            args << QVariant();
        }
        args << QVariant::fromValue(zoneValue.toCbor());
    }

    QCborValue value;
    QString error;
    if (!callFilterMethod("filter", args, value, error)) {
        result.kind = Result::Failed;
        result.error = error;
        return result;
    }
    if (value.isMap() || value.isArray()) {
        const PlainScriptValue object = PlainScriptValue::fromCbor(value);
        in.copyFromScriptValue(object, false);
        out.copyFromScriptValue(object, false);
        result.outAsJson = QJsonValue::fromVariant(object.toVariant());
        result.kind = Result::Properties;
    } else if (value.isBool()) {
        result.kind = value.toBool() ? Result::Accepted : Result::Rejected;
    } else {
        // undefined: nothing returned, or an exception, which the guest reported to the log
        result.kind = Result::Rejected;
    }
    return result;
}

EntityEditFilterEnginePointer WasmEditFilterEngine::makeForSetting(const EntityItemID& entityID) {
    if (WasmScriptEngine::hasJavaScriptGuest()) {
        return std::make_unique<WasmEditFilterEngine>(entityID);
    }
    return std::make_unique<QtScriptEditFilterEngine>(entityID);
}
