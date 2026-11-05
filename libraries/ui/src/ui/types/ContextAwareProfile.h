//
//  Created by Bradley Austin Davis on 2018/07/27
//  Copyright 2013-2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_ContextAwareProfile_h
#define hifi_ContextAwareProfile_h

#include <atomic>
#include <QtCore/QHash>
#include <QtCore/QReadWriteLock>
#include <QtCore/QSet>
#include <QtCore/QSharedPointer>

#if !defined(Q_OS_ANDROID)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtWebEngineQuick/QQuickWebEngineProfile>   // Qt 6: the QML web view's module is WebEngineQuick
#else
#include <QtWebEngine/QQuickWebEngineProfile>
#endif
#include <QtWebEngineCore/QWebEngineUrlRequestInterceptor>

using ContextAwareProfileParent = QQuickWebEngineProfile;
using RequestInterceptorParent = QWebEngineUrlRequestInterceptor;
#else
#include <QtCore/QObject>

using ContextAwareProfileParent = QObject;
using RequestInterceptorParent = QObject;
#endif

class QQmlContext;

// Qt 6's Chromium can't run two browser contexts on one data folder: their Local and Session Storage fight over LevelDB's
// locks for the whole session, and at quit the thread pool never drains. So on Qt 6 a class has one object per setting,
// each on a folder of its own, shared by every QML context with that setting; Qt 5 makes one object per QML context.
#define SHARED_WEB_PROFILES (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))

class ContextAwareProfile : public ContextAwareProfileParent {
    Q_OBJECT
public:
    static void restrictContext(QQmlContext* context, bool restrict = true);
    bool isRestricted();
    Q_INVOKABLE bool isRestrictedGetProperty();
protected:

    class RequestInterceptor : public RequestInterceptorParent {
    public:
        RequestInterceptor(ContextAwareProfile* parent) : RequestInterceptorParent(parent), _profile(parent) { }
        bool isRestricted() { return _profile->isRestricted(); }
    protected:
        ContextAwareProfile* _profile;
    };

#if SHARED_WEB_PROFILES
    // No parent, never deleted: views in any context use the object, and a parent context's death re-ran their profile
    // bindings into dying views. WebEngineContext::destroy deletes what's left at quit. The flag never changes.
    explicit ContextAwareProfile(bool restricted);

    // The file-access flag set on a context or on one of its parents
    static bool isContextRestricted(QQmlContext* context);
#else
    ContextAwareProfile(QQmlContext* parent);
#endif
    ~ContextAwareProfile();

#if !defined(Q_OS_ANDROID)
    // Puts the profile on its storage: on disk, under that name
    void useStorage(const QString& storageName);
#endif

private:
    std::atomic<bool> _isRestricted{ false };

#if !SHARED_WEB_PROFILES
    typedef QSet<ContextAwareProfile*> ContextAwareProfileSet;
    typedef QHash<QQmlContext*, ContextAwareProfileSet> ContextMap;

    QQmlContext* _context{ nullptr };

    static QReadWriteLock _contextMapProtect;
    static ContextMap _contextMap;
#endif
};

#endif // hifi_FileTypeProfile_h
