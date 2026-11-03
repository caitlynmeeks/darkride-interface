//
//  QtScriptEditFilterEngine.cpp
//  libraries/entities/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "QtScriptEditFilterEngine.h"

#include <QtScript/QScriptProgram>

#include <DependencyManager.h>
#include <RegisteredMetaTypes.h>
#include <shared/ScriptInitializerMixin.h>

#include "EntityTree.h"

QtScriptEditFilterEngine::QtScriptEditFilterEngine(const EntityItemID& entityID) : _entityID(entityID) {
}

QtScriptEditFilterEngine::~QtScriptEditFilterEngine() {
    delete _engine;
}

// Copied from ScriptEngine.cpp. We should make this a class method for reuse.
static bool hasCorrectSyntax(const QScriptProgram& program, QString& message) {
    const auto syntaxCheck = QScriptEngine::checkSyntax(program.sourceCode());
    if (syntaxCheck.state() != QScriptSyntaxCheckResult::Valid) {
        const auto error = syntaxCheck.errorMessage();
        const auto line = QString::number(syntaxCheck.errorLineNumber());
        const auto column = QString::number(syntaxCheck.errorColumnNumber());
        message = QString("[SyntaxError] %1 in %2:%3(%4)").arg(error, program.fileName(), line, column);
        qCritical() << qPrintable(message);
        return false;
    }
    return true;
}

bool QtScriptEditFilterEngine::hadUncaughtExceptions(QString& message) {
    if (_engine->hasUncaughtException()) {
        const auto backtrace = _engine->uncaughtExceptionBacktrace();
        const auto exception = _engine->uncaughtException().toString();
        const auto line = QString::number(_engine->uncaughtExceptionLineNumber());
        _engine->clearExceptions();

        static const QString SCRIPT_EXCEPTION_FORMAT = "[UncaughtException] %1 in %2:%3";
        message = QString(SCRIPT_EXCEPTION_FORMAT).arg(exception, _url, line);
        if (!backtrace.empty()) {
            static const auto lineSeparator = "\n    ";
            message += QString("\n[Backtrace]%1%2").arg(lineSeparator, backtrace.join(lineSeparator));
        }
        qCritical() << qPrintable(message);
        return true;
    }
    return false;
}

bool QtScriptEditFilterEngine::load(const QString& source, const QString& url, QString& error) {
    _url = url;
    QScriptProgram program(source, url);
    if (!hasCorrectSyntax(program, error)) {
        return false;
    }
    // create a QScriptEngine for this script
    _engine = new QScriptEngine();
    _engine->setObjectName("filter:" + _entityID.toString());
    _engine->setProperty("type", "edit_filter");
    _engine->setProperty("fileName", url);
    _engine->setProperty("entityID", _entityID);
    // Script without QObject's own methods, as the script engines' globals (ScriptEngine.cpp)
    _engine->globalObject().setProperty("Script", _engine->newQObject(_engine, QScriptEngine::QtOwnership,
        QScriptEngine::ExcludeDeleteLater | QScriptEngine::ExcludeSuperClassMethods | QScriptEngine::ExcludeChildObjects));
    if (DependencyManager::isSet<ScriptInitializers>()) {
        DependencyManager::get<ScriptInitializers>()->runScriptInitializers([this](const QString& name, QObject* object) {
            _engine->globalObject().setProperty(name, _engine->newQObject(object, QScriptEngine::QtOwnership,
                                                                          QScriptEngine::ExcludeDeleteLater));
        });
    }
    _engine->evaluate(source, url);
    if (hadUncaughtExceptions(error)) {
        return false;
    }
    // now get the filter function
    auto global = _engine->globalObject();
    auto entitiesObject = _engine->newObject();
    entitiesObject.setProperty("ADD_FILTER_TYPE", EntityTree::FilterType::Add);
    entitiesObject.setProperty("EDIT_FILTER_TYPE", EntityTree::FilterType::Edit);
    entitiesObject.setProperty("PHYSICS_FILTER_TYPE", EntityTree::FilterType::Physics);
    entitiesObject.setProperty("DELETE_FILTER_TYPE", EntityTree::FilterType::Delete);
    global.setProperty("Entities", entitiesObject);
    _filterFn = global.property("filter");
    if (!_filterFn.isFunction()) {
        error = "Filter function specified but not found";
        return false;
    }
    return true;
}

QVariantMap QtScriptEditFilterEngine::flags() {
    QVariantMap flags;
    for (const char* name : { "wantsToFilterAdd", "wantsToFilterEdit", "wantsToFilterPhysics", "wantsToFilterDelete",
                              "wantsOriginalProperties", "wantsZoneProperties" }) {
        const QScriptValue value = _filterFn.property(name);
        if (value.isBool()) {
            flags[name] = value.toBool();
        } else if (value.isString()) {
            flags[name] = value.toString();
        } else if (value.isArray()) {
            QVariantList names;
            const int length = value.property("length").toInteger();
            for (int i = 0; i < length; i++) {
                names << value.property(i).toString();
            }
            flags[name] = names;
        }
    }
    return flags;
}

EntityEditFilterEngine::Result QtScriptEditFilterEngine::call(EntityItemProperties& in, EntityItemProperties& out, int filterType,
                                                               const EntityItemProperties* original, const EntityItemProperties* zone,
                                                               const AABox* zoneBox) {
    Result result;
    QScriptValue inputValues = in.copyToScriptValue(_engine, false, true, true);
    // grab json copy now, because the inputValues might be side effected by the filter.
    result.inAsJson = QJsonValue::fromVariant(inputValues.toVariant());

    QScriptValueList args;
    args << inputValues;
    args << filterType;
    if (original) {
        args << original->copyToScriptValue(_engine, false, true, true);
    }
    if (zone) {
        QScriptValue zoneValues = zone->copyToScriptValue(_engine, false, true, true);
        if (zoneBox) {
            QScriptValue boundingBox = _engine->newObject();
            boundingBox.setProperty("brn", vec3ToScriptValue(_engine, zoneBox->getCorner()));
            boundingBox.setProperty("tfl", vec3ToScriptValue(_engine, zoneBox->calcTopFarLeft()));
            boundingBox.setProperty("center", vec3ToScriptValue(_engine, zoneBox->calcCenter()));
            boundingBox.setProperty("dimensions", vec3ToScriptValue(_engine, zoneBox->getDimensions()));
            zoneValues.setProperty("boundingBox", boundingBox);
        }
        // If this is an add or delete, or original properties weren't requested there won't be original
        // properties in the args, but zone properties need to be the fourth parameter, so we need to pad
        // the args accordingly
        const int EXPECTED_ARGS = 3;
        if (args.length() < EXPECTED_ARGS) {
            args << QScriptValue();
        }
        args << zoneValues;
    }

    QScriptValue value = _filterFn.call(QScriptValue(), args);
    if (hadUncaughtExceptions(result.error)) {
        result.kind = Result::Failed;
        return result;
    }
    if (value.isObject()) {
        in.copyFromScriptValue(value, false);
        out.copyFromScriptValue(value, false);
        // Javascript objects are == only if they are the same object. To compare arbitrary values, we need to use JSON.
        result.outAsJson = QJsonValue::fromVariant(value.toVariant());
        result.kind = Result::Properties;
    } else if (value.isBool()) {
        result.kind = value.toBool() ? Result::Accepted : Result::Rejected;
    } else {
        result.kind = Result::Rejected;
    }
    return result;
}
