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

#include "FileTypeProfile.h"

#include <set>
#include <mutex>
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

#include "BuildInfo.h"
#include "RequestFilters.h"

#if !defined(Q_OS_ANDROID)
#if SHARED_WEB_PROFILES
static const QString QML_WEB_ENGINE_STORAGE_NAME = "qmlWebEngineFiles";
static const QString QML_WEB_ENGINE_RESTRICTED_STORAGE_NAME = "qmlWebEngineFilesRestricted";
#else
static const QString QML_WEB_ENGINE_STORAGE_NAME = "qmlWebEngine";
#endif

static std::set<FileTypeProfile*> FileTypeProfile_instances;
static std::mutex FileTypeProfile_mutex;

#if SHARED_WEB_PROFILES
FileTypeProfile::FileTypeProfile(const QString& storageName, bool restricted) :
    ContextAwareProfile(restricted)
#else
FileTypeProfile::FileTypeProfile(QQmlContext* parent) :
    ContextAwareProfile(parent)
#endif
{
    static const QString WEB_ENGINE_USER_AGENT = QString(
        "Darkride/" + 
        (BuildInfo::BUILD_TYPE == BuildInfo::BuildType::Stable ? BuildInfo::VERSION : "dev")
    );
    setHttpUserAgent(WEB_ENGINE_USER_AGENT);

#if SHARED_WEB_PROFILES
    useStorage(storageName);
#else
    useStorage(QML_WEB_ENGINE_STORAGE_NAME);
#endif
    RequestFilters::disableServiceWorkers(this);

    auto requestInterceptor = new RequestInterceptor(this);
    setUrlRequestInterceptor(requestInterceptor);

    std::lock_guard<std::mutex> lock(FileTypeProfile_mutex);
    FileTypeProfile_instances.insert(this);
}

FileTypeProfile::~FileTypeProfile() {
    std::lock_guard<std::mutex> lock(FileTypeProfile_mutex);
    FileTypeProfile_instances.erase(this);
}

void FileTypeProfile::RequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo& info) {
    RequestFilters::interceptTivoliWebEngineRequest(info, isRestricted());
    RequestFilters::interceptFileType(info);
}

void FileTypeProfile::registerWithContext(QQmlContext* context) {
#if SHARED_WEB_PROFILES
    Q_ASSERT(QThread::currentThread() == qApp->thread());   // one object per folder: never two made at once
    static FileTypeProfile* shared[2] { nullptr, nullptr };   // by the file-access flag
    const bool restricted = isContextRestricted(context);
    FileTypeProfile*& profile = shared[restricted];
    if (!profile) {
        profile = new FileTypeProfile(restricted ? QML_WEB_ENGINE_RESTRICTED_STORAGE_NAME : QML_WEB_ENGINE_STORAGE_NAME, restricted);
        QQmlEngine::setObjectOwnership(profile, QQmlEngine::CppOwnership);
    }
    context->setContextProperty("FileTypeProfile", profile);
#else
    context->setContextProperty("FileTypeProfile", new FileTypeProfile(context));
#endif
}

void FileTypeProfile::clearCache() {
    std::lock_guard<std::mutex> lock(FileTypeProfile_mutex);
    for (auto instance : qAsConst(FileTypeProfile_instances)) {
        instance->clearHttpCache();
    }
}

#endif