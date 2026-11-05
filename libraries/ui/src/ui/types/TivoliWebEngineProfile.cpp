//
//  TivoliWebEngineProfile.cpp
//  interface/src/networking
//
//  Created by Stephen Birarda on 2016-10-17.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TivoliWebEngineProfile.h"

#include <set>
#include <mutex>
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

#include "RequestFilters.h"

#if !defined(Q_OS_ANDROID)

static const QString QML_WEB_ENGINE_STORAGE_NAME = "qmlWebEngine";

static std::set<TivoliWebEngineProfile*> TivoliWebEngineProfile_instances;
static std::mutex TivoliWebEngineProfile_mutex;

#if SHARED_WEB_PROFILES
TivoliWebEngineProfile::TivoliWebEngineProfile(const QString& storageName, bool restricted) : Parent(restricted)
#else
TivoliWebEngineProfile::TivoliWebEngineProfile(QQmlContext* parent) : Parent(parent)
#endif
{
#if SHARED_WEB_PROFILES
    useStorage(this, storageName);
#else
    useStorage(this, QML_WEB_ENGINE_STORAGE_NAME);
#endif
    RequestFilters::disableServiceWorkers(this);

    // we use the HFWebEngineRequestInterceptor to make sure that web requests are authenticated for the interface user
    setUrlRequestInterceptor(new RequestInterceptor(this));

    std::lock_guard<std::mutex> lock(TivoliWebEngineProfile_mutex);
    TivoliWebEngineProfile_instances.insert(this);
}

TivoliWebEngineProfile::~TivoliWebEngineProfile() {
    std::lock_guard<std::mutex> lock(TivoliWebEngineProfile_mutex);
    TivoliWebEngineProfile_instances.erase(this);
}

void TivoliWebEngineProfile::RequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo& info) {
    RequestFilters::interceptTivoliWebEngineRequest(info, isRestricted());
}

void TivoliWebEngineProfile::registerWithContext(QQmlContext* context) {
#if SHARED_WEB_PROFILES
    Q_ASSERT(QThread::currentThread() == qApp->thread());   // one object per folder: never two made at once
    static TivoliWebEngineProfile* shared[2][2] { { nullptr, nullptr }, { nullptr, nullptr } };   // by Web entity, file access
    const bool webEntity = isWebEntityContext(context);
    const bool restricted = isContextRestricted(context);
    TivoliWebEngineProfile*& profile = shared[webEntity][restricted];
    if (!profile) {
        const QString storageName = QML_WEB_ENGINE_STORAGE_NAME + (webEntity ? "WebEntities" : "") + (restricted ? "Restricted" : "");
        profile = new TivoliWebEngineProfile(storageName, restricted);
        QQmlEngine::setObjectOwnership(profile, QQmlEngine::CppOwnership);
    }
    context->setContextProperty("TivoliWebEngineProfile", profile);
#else
    context->setContextProperty("TivoliWebEngineProfile", new TivoliWebEngineProfile(context));
#endif
}

void TivoliWebEngineProfile::clearCache() {
    std::lock_guard<std::mutex> lock(TivoliWebEngineProfile_mutex);
    for (auto instance : qAsConst(TivoliWebEngineProfile_instances)) {
        instance->clearHttpCache();
    }
}

#endif
