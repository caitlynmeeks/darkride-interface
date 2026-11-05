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

    ContextAwareProfile(QQmlContext* parent);
    ~ContextAwareProfile();

#if !defined(Q_OS_ANDROID)
    // Puts the profile on its storage: on disk, under that name
    void useStorage(const QString& storageName);
#endif

private:
    typedef QSet<ContextAwareProfile*> ContextAwareProfileSet;
    typedef QHash<QQmlContext*, ContextAwareProfileSet> ContextMap;

    QQmlContext* _context{ nullptr };
    std::atomic<bool> _isRestricted{ false };

    static QReadWriteLock _contextMapProtect;
    static ContextMap _contextMap;
};

#endif // hifi_FileTypeProfile_h
