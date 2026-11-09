//
//  ScriptInitializerMixin.h
//  libraries/shared/src/shared
//
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.

#pragma once

#include <functional>
#include <mutex>
#include <QSharedPointer>
#include <QString>
#include "../DependencyManager.h"

class QObject;
class ScriptEngine;

template <typename T> class ScriptInitializerMixin {
public:
    using ScriptInitializer = std::function<void(T)>;
    virtual void registerScriptInitializer(ScriptInitializer initializer) {
        InitializerLock lock(_scriptInitializerMutex);
        _scriptInitializers.push_back(initializer);
    }
    virtual int runScriptInitializers(T engine) {
        InitializerLock lock(_scriptInitializerMutex);
        return std::count_if(_scriptInitializers.begin(), _scriptInitializers.end(),
            [engine](auto initializer){ initializer(engine); return true; }
        );
    }
    virtual ~ScriptInitializerMixin() {}
protected:
    std::mutex _scriptInitializerMutex;
    using InitializerLock = std::lock_guard<std::mutex>;
    std::list<ScriptInitializer> _scriptInitializers;
};

/// What a plugin's script initializer is handed for each new script engine: a function that gives the engine's scripts
/// a global object, through the engine's own registerGlobalObject (so either engine, and the script API policy for a
/// world's scripts, apply). The object stays the plugin's.
using ScriptGlobalObjectAdder = std::function<void(const QString& name, QObject* object)>;

// Lightweight initializer for code that doesn't link the script engine (a plugin), e.g.:
//     initializers->registerScriptInitializer([this](const ScriptGlobalObjectAdder& addGlobalObject) {
//         addGlobalObject("API", &_instance);
//     });
class ScriptInitializers : public ScriptInitializerMixin<const ScriptGlobalObjectAdder&>, public Dependency {
};
