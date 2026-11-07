//
//  ScriptOpener.cpp
//  libraries/script-engine/src
//
//  Created by scripting on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptOpener.h"

#include <atomic>
#include <memory>

#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <DependencyManager.h>
#include <EntityScriptRights.h>
#include <EntityScriptingInterface.h>
#include <ResourceManager.h>
#include <ScriptNetworkPolicy.h>

#include "ScriptCache.h"
#include "ScriptEngineLogging.h"

const QString ScriptOpener::SCRIPT = "script";
const QString ScriptOpener::SERVER_SCRIPTS = "serverScripts";
const QString ScriptOpener::WRITTEN_IN = "written in";
const QString ScriptOpener::WORLD_FILES = "world files";
const QString ScriptOpener::WEB = "web";
const QString ScriptOpener::LOCAL_FILE = "file";

namespace {

// the origin a world's client entity scripts are judged by (ScriptEngine::ruleEntityScriptFetch gives the same)
const QUrl ENTITIES_ORIGIN("about:Entities");

const QString CANT_OPEN_HERE = "This script's address can't be opened here.";

}

bool ScriptOpener::isAddress(const QString& value) {
    const QString trimmed = value.trimmed();
    return !trimmed.isEmpty() && !trimmed.contains('\n') && !QUrl(trimmed).scheme().isEmpty();
}

ScriptOpener::Opened ScriptOpener::judge(const QString& value, bool worldIsLocal) {
    Opened opened;
    const QString trimmed = value.trimmed();
    if (!isAddress(trimmed)) {
        // the code itself, or an empty field (a new script typed in the editor goes into it)
        opened.where = WRITTEN_IN;
        opened.text = value;
        return opened;
    }
    const QUrl url(trimmed);
    const QString scheme = url.scheme().toLower();
    if (scheme == "file") {
        opened.where = LOCAL_FILE;
        opened.url = url;
        opened.problem = "A script file on this computer can't be opened here.";
        return opened;
    }
    if (scheme == "atp") {
        opened.where = WORLD_FILES;
    } else if (scheme == "http" || scheme == "https") {
        opened.where = WEB;
    } else {
        // javascript:, qrc:, a Windows drive letter read as a scheme, anything else: not a script file to open
        opened.problem = CANT_OPEN_HERE;
        return opened;
    }
    opened.url = url;
    if (QFileInfo(url.path()).suffix().toLower() == "wasm") {
        opened.problem = "A WebAssembly module isn't shown as text.";
        return opened;
    }
    // the world's rule, before any request goes out; a name is looked up by the request itself, under the same rule
    const ScriptNetworkPolicy::Decision decision = ScriptNetworkPolicy::check(url, ENTITIES_ORIGIN, worldIsLocal);
    if (!decision.allowed && !decision.needsLookup) {
        qCDebug(scriptengine) << "ScriptOpener: not opened," << decision.reason << url.toString();
        opened.problem = "This script's address is one a world from the network may not reach.";
    }
    return opened;
}

void ScriptOpener::open(const QUuid& entityId, const QString& which, QObject* context, Callback callback) {
    Opened opened;
    if (which != SCRIPT && which != SERVER_SCRIPTS) {
        opened.problem = "Only a script or a server script can be opened.";
        callback(opened);
        return;
    }
    if (!context) {
        Q_ASSERT_X(context, "ScriptOpener::open", "a context is required");
        opened.problem = "This script couldn't be loaded.";
        callback(opened);
        return;
    }
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    EntityPropertyFlags flags;
    flags += PROP_LOCKED;
    flags += which == SERVER_SCRIPTS ? PROP_SERVER_SCRIPTS : PROP_SCRIPT;
    const EntityItemProperties properties = entities ? entities->getEntityProperties(entityId, flags)
                                                     : EntityItemProperties();
    if (properties.getType() == EntityTypes::Unknown) {
        opened.problem = "This thing isn't in this world any more.";
        callback(opened);
        return;
    }
    if (which == SERVER_SCRIPTS && !entity_script_rights::mayChangeScriptsOf(properties.getLocked())) {
        // the world's editors only: nothing of it is read, not even its address
        opened.problem = "Only people who can change this thing can read its server script.";
        callback(opened);
        return;
    }
    QString value = which == SERVER_SCRIPTS ? properties.getServerScripts() : properties.getScript();
    if (QUrl(value.trimmed()).scheme().toLower() == "tea") {
        // the files host's own scheme, read as the script cache reads it: a web address on that host
        auto resources = DependencyManager::get<ResourceManager>();
        if (resources) {
            value = resources->normalizeURL(QUrl(value.trimmed())).toString();
        }
    }
    const bool worldIsLocal = ScriptNetworkPolicy::isWorldLocal();
    opened = judge(value, worldIsLocal);
    if (!opened.ok() || opened.where == WRITTEN_IN) {
        callback(opened);
        return;
    }
    fetch(opened, worldIsLocal, context, std::move(callback));
}

void ScriptOpener::fetch(const Opened& judged, bool worldIsLocal, QObject* context, Callback callback) {
    auto scriptCache = DependencyManager::get<ScriptCache>();
    if (!scriptCache) {
        Opened opened = judged;
        opened.problem = "This script couldn't be loaded.";
        callback(opened);
        return;
    }

    // one answer, the first of the download and the time limit, on the context's thread, only while the context lives
    struct Pending {
        QPointer<QObject> context;
        Callback callback;
        std::atomic<bool> answered { false };
    };
    auto pending = std::make_shared<Pending>();
    pending->context = context;
    pending->callback = std::move(callback);
    auto deliver = [pending](const Opened& result) {
        if (pending->answered.exchange(true)) {
            return;
        }
        QObject* target = pending->context.data();
        if (!target) {
            return;
        }
        if (QThread::currentThread() == target->thread()) {
            pending->callback(result);
            return;
        }
        Callback callback = pending->callback;
        QMetaObject::invokeMethod(target, [callback, result] { callback(result); }, Qt::QueuedConnection);
    };

    QTimer::singleShot(TIME_LIMIT_MS, context, [deliver, judged] {
        Opened late = judged;
        late.problem = "This script took too long to load.";
        qCDebug(scriptengine) << "ScriptOpener: time limit" << judged.url.toString();
        deliver(late);
    });

    // the download is held to the world's rule as the entities engine's own would be (ScriptCache::ruleFetch, read
    // by the request the cache makes: each hop judged, a name's addresses checked, the request pinned to one)
    scriptCache->ruleFetch(judged.url, ENTITIES_ORIGIN, worldIsLocal);
    const int noRetries = 0;
    scriptCache->getScriptContents(judged.url.toString(),
        [deliver, judged](const QString&, const QString& contents, bool, bool success, const QString& status) {
            Opened result = judged;
            if (!success) {
                qCDebug(scriptengine) << "ScriptOpener: not loaded (" << status << ")" << judged.url.toString();
                result.problem = "This script couldn't be loaded.";
            } else if (contents.toUtf8().size() > MAX_BYTES) {
                result.problem = "This script is too big to show here (1 MB at most).";
            } else {
                result.text = contents;
            }
            deliver(result);
        }, false, noRetries);
}
