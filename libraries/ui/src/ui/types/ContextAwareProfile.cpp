//
//  FileTypeProfile.cpp
//  interface/src/networking
//
//  Created by Kunal Gosar on 2017-03-10.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ContextAwareProfile.h"

#include <cassert>
#include <QtCore/QReadLocker>
#include <QtCore/QThread>
#include <QtCore/QWriteLocker>
#include <QtQml/QQmlContext>

#include <shared/QtHelpers.h>
#include <PathUtils.h>
#include <SharedUtil.h>

#if SHARED_WEB_PROFILES && !defined(Q_OS_ANDROID)
#include "FileTypeProfile.h"
#include "TivoliWebEngineProfile.h"
#endif

static const QString RESTRICTED_FLAG_PROPERTY = "RestrictFileAccess";
#if SHARED_WEB_PROFILES
static const QString WEB_ENTITY_FLAG_PROPERTY = "WebEntitySurface";
#endif

#if SHARED_WEB_PROFILES
ContextAwareProfile::ContextAwareProfile(bool restricted) : _isRestricted(restricted) {
}

ContextAwareProfile::~ContextAwareProfile() {
}

bool ContextAwareProfile::isContextRestricted(QQmlContext* context) {
    // no flag means unrestricted: see the Qt 5 isRestrictedGetProperty below
    return context->contextProperty(RESTRICTED_FLAG_PROPERTY).toBool();
}

bool ContextAwareProfile::isWebEntityContext(QQmlContext* context) {
    return context->contextProperty(WEB_ENTITY_FLAG_PROPERTY).toBool();
}
#else
QReadWriteLock ContextAwareProfile::_contextMapProtect;
ContextAwareProfile::ContextMap ContextAwareProfile::_contextMap;

ContextAwareProfile::ContextAwareProfile(QQmlContext* context) : ContextAwareProfileParent(context), _context(context) {
    assert(context);

    {   // register our object for future updates
        QWriteLocker guard(&_contextMapProtect);
        ContextMap::iterator setLookup = _contextMap.find(_context);
        if (setLookup == _contextMap.end()) {
            setLookup = _contextMap.insert(_context, ContextAwareProfileSet());
        }
        assert(setLookup != _contextMap.end());
        ContextAwareProfileSet& profileSet = setLookup.value();
        assert(profileSet.find(this) == profileSet.end());
        profileSet.insert(this);
    }

    _isRestricted.store(isRestrictedGetProperty());
}

ContextAwareProfile::~ContextAwareProfile() {
    // deregister our object
    QWriteLocker guard(&_contextMapProtect);
    ContextMap::iterator setLookup = _contextMap.find(_context);
    assert(setLookup != _contextMap.end());
    if (setLookup != _contextMap.end()) {
        ContextAwareProfileSet& profileSet = setLookup.value();
        assert(profileSet.find(this) != profileSet.end());
        profileSet.remove(this);
        if (profileSet.isEmpty()) {
            _contextMap.erase(setLookup);
        }
    }
}
#endif

#if !defined(Q_OS_ANDROID)
void ContextAwareProfile::useStorage(const QString& storageName) {
    setStorageName(storageName);

    // A walled-off client (TIVOLI_APP_DATA_DIR) keeps its web storage in its own folder, away from the user's. Its
    // cache too: Qt puts that in the system's cache folder otherwise. As RadioSkinPicker::walledStoragePath does.
    if (!qEnvironmentVariableIsEmpty("TIVOLI_APP_DATA_DIR")) {
        const QString folder = PathUtils::getAppDataPath() + "QtWebEngine/" + storageName;
        setPersistentStoragePath(folder);
        setCachePath(folder);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Qt 6.8+ stores a page's permission answers per profile by default, on disk, for good: on a shared profile one yes
    // would cover every view on it. Each page asks each time. Before going on the record, so no stored answers load.
    setPersistentPermissionsPolicy(QQuickWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
#endif

    // Last: going on the record opens the preferences file at the data path of that moment (ProfileAdapter::
    // setOffTheRecord, setupPrefService), so a walled client's path has to be in place first.
    setOffTheRecord(false);
}
#endif

void ContextAwareProfile::separateWebEntitySurface(QQmlContext* context) {
#if SHARED_WEB_PROFILES
    // on the surface's root context, so a context made under it later finds the flag too; only this class's views are
    // made on these surfaces
    context->setContextProperty(WEB_ENTITY_FLAG_PROPERTY, true);
#if !defined(Q_OS_ANDROID)
    TivoliWebEngineProfile::registerWithContext(context);
#endif
#else
    Q_UNUSED(context)
#endif
}

void ContextAwareProfile::restrictContext(QQmlContext* context, bool restrict) {

    // set the QML property
    context->setContextProperty(RESTRICTED_FLAG_PROPERTY, restrict);

#if SHARED_WEB_PROFILES
    // The context takes the objects for this setting. Before anything in it has bound them, always: setContextProperty
    // re-runs every view's profile: binding, and a live view rebuilds its page on a new profile. The desktop surface, the
    // one context that had objects already, is restricted as its root context is made (Application::initializeUi).
#if !defined(Q_OS_ANDROID)
    FileTypeProfile::registerWithContext(context);
    TivoliWebEngineProfile::registerWithContext(context);
#endif
#else
    // broadcast the new value to any registered ContextAwareProfile objects
    QReadLocker guard(&_contextMapProtect);
    ContextMap::const_iterator setLookup = _contextMap.find(context);
    if (setLookup != _contextMap.end()) {
        const ContextAwareProfileSet& profileSet = setLookup.value();
        for (ContextAwareProfileSet::const_iterator profileIterator = profileSet.begin();
                profileIterator != profileSet.end(); profileIterator++) {
            (*profileIterator)->_isRestricted.store(restrict);
        }
    }
#endif
}

bool ContextAwareProfile::isRestrictedGetProperty() {
#if SHARED_WEB_PROFILES
    return _isRestricted.load();
#else
    if (QThread::currentThread() != thread()) {
        bool restrictedResult = false;
        BLOCKING_INVOKE_METHOD(this, "isRestrictedGetProperty", Q_RETURN_ARG(bool, restrictedResult));
        return restrictedResult;
    }

    QVariant variant = _context->contextProperty(RESTRICTED_FLAG_PROPERTY);
    if (variant.isValid()) {
        return variant.toBool();
    }

    // BUGZ-1365 - we MUST defalut to restricted mode in the absence of a flag, or it's too easy for someone to make 
    // a new mechanism for loading web content that fails to restrict access to local files
    //return true;

    // Tivoli: local files are necessary
    return false; 
#endif
}

bool ContextAwareProfile::isRestricted() {
    return _isRestricted.load();
}
