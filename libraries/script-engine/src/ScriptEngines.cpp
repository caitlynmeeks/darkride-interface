//
//  Created by Bradley Austin Davis on 2016/01/08
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ScriptEngines.h"

#include <ScriptNetworkPolicy.h>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>

#include <QtCore/QStandardPaths>

#include <QtWidgets/QApplication>

#include <shared/QtHelpers.h>
#include <SettingHandle.h>
#include <PathUtils.h>
#include <ScriptCaller.h>
#include <shared/FileUtils.h>
#include <NetworkingConstants.h>

#include "ScriptEngine.h"
#include "ScriptEngineLogging.h"
#include "WasmScriptEngine.h"

#define __STR2__(x) #x
#define __STR1__(x) __STR2__(x)
#define __LOC__ __FILE__ "(" __STR1__(__LINE__) ") : Warning Msg: "

static const QString DESKTOP_LOCATION = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
static const QString SETTINGS_KEY { "RunningScripts" };
static const QUrl DEFAULT_SCRIPTS_LOCATION { "file:///~//defaultScripts.js" };

// Using a QVariantList so this is human-readable in the settings file
static Setting::Handle<QVariantList> runningScriptsHandle(SETTINGS_KEY, { QVariant(DEFAULT_SCRIPTS_LOCATION) });
// The one review of the saved scripts (security finding #30) is done once. Its mark is a file in the app's data
// folder, which no script can write: a setting could have been set by a world before this app.
static QString savedScriptsReviewedMark() {
    return PathUtils::getAppDataFilePath("saved-scripts-reviewed");
}
static bool savedScriptsReviewed() {
    return QFileInfo::exists(savedScriptsReviewedMark());
}
static void markSavedScriptsReviewed() {
    QFile mark(savedScriptsReviewedMark());
    if (mark.open(QIODevice::WriteOnly)) {
        mark.write("reviewed\n");
    }
}
// A file a world could have had the app write (a download into its caches, its data folder, the system's temp
// folder): held for the review like a script from the network. Compared on canonical paths.
static bool inAFolderTheAppWrites(const QUrl& url) {
    QFileInfo file(url.toLocalFile());
    const QString path = file.exists() ? file.canonicalFilePath() : QDir::cleanPath(file.absoluteFilePath());
    const QStringList folders {
        PathUtils::getAppDataPath(), PathUtils::getAppLocalDataPath(),
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation), QDir::tempPath()
    };
    for (const QString& folder : folders) {
        if (folder.isEmpty()) {
            continue;
        }
        QString canonical = QFileInfo(folder).canonicalFilePath();
        if (canonical.isEmpty()) {
            canonical = QDir::cleanPath(folder);
        }
        if (path == canonical || path.startsWith(canonical + "/")) {
            return true;
        }
    }
    return false;
}

const int RELOAD_ALL_SCRIPTS_TIMEOUT = 1000;


ScriptsModel& getScriptsModel() {
    static ScriptsModel scriptsModel;
    return scriptsModel;
}

void ScriptEngines::onPrintedMessage(const QString& message, const QString& scriptName) {
    emit printedMessage(message, scriptName);
}

void ScriptEngines::onErrorMessage(const QString& message, const QString& scriptName) {
    emit errorMessage(message, scriptName);
}

void ScriptEngines::onWarningMessage(const QString& message, const QString& scriptName) {
    emit warningMessage(message, scriptName);
}

void ScriptEngines::onInfoMessage(const QString& message, const QString& scriptName) {
    emit infoMessage(message, scriptName);
}

void ScriptEngines::onClearDebugWindow() {
    emit clearDebugWindow();
}

void ScriptEngines::onErrorLoadingScript(const QString& url) {
    emit errorLoadingScript(url);
}

ScriptEngines::ScriptEngines(ScriptEngine::Context context, const QUrl& defaultScriptsOverride)
    : _context(context), _defaultScriptsOverride(defaultScriptsOverride)
{
    _scriptsModelFilter.setSourceModel(&_scriptsModel);
    _scriptsModelFilter.sort(0, Qt::AscendingOrder);
    _scriptsModelFilter.setDynamicSortFilter(true);
}

QUrl normalizeScriptURL(const QUrl& rawScriptURL) {
    if (rawScriptURL.scheme() == "file") {
        QUrl fullNormal = rawScriptURL;
        QUrl defaultScriptLoc = PathUtils::defaultScriptsLocation();

        // if this url is something "beneath" the default script url, replace the local path with ~
        if (fullNormal.scheme() == defaultScriptLoc.scheme() &&
            fullNormal.host() == defaultScriptLoc.host() &&
            fullNormal.path().startsWith(defaultScriptLoc.path())) {
            fullNormal.setPath("/~/" + fullNormal.path().mid(defaultScriptLoc.path().size()));
        }
        return fullNormal;
    } else if (rawScriptURL.scheme() != "about" && KNOWN_SCHEMES.contains(rawScriptURL.scheme(), Qt::CaseInsensitive)) {
        return rawScriptURL;
    } else {
        // don't accidently support gopher
        return QUrl("");
    }
}

QString expandScriptPath(const QString& rawPath) {
    QStringList splitPath = rawPath.split("/");
    QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
    return defaultScriptsLoc.path() + "/" + splitPath.mid(2).join("/"); // 2 to skip the slashes in /~/
}

QUrl expandScriptUrl(const QUrl& rawScriptURL) {
    QUrl normalizedScriptURL = normalizeScriptURL(rawScriptURL);
    if (normalizedScriptURL.scheme() == "file") {
        if (normalizedScriptURL.path().startsWith("/~/")) {
            QUrl url = normalizedScriptURL;
            url.setPath(expandScriptPath(url.path()));

            // stop something like Script.include(["/~/../Desktop/naughty.js"]); from working
            QFileInfo fileInfo(url.toLocalFile());
            url = QUrl::fromLocalFile(fileInfo.canonicalFilePath());

            QUrl defaultScriptsLoc = PathUtils::defaultScriptsLocation();
            if (!defaultScriptsLoc.isParentOf(url) && defaultScriptsLoc != url) {
                qCWarning(scriptengine) << "Script.include() ignoring file path"
                                        << "-- outside of standard libraries: "
                                        << url.path()
                                        << defaultScriptsLoc.path();
                return rawScriptURL;
            }
            if (rawScriptURL.path().endsWith("/") && !url.path().endsWith("/")) {
                url.setPath(url.path() + "/");
            }
            return url;
        }
        return normalizedScriptURL;
    } else if (normalizedScriptURL.scheme() != "about" && KNOWN_SCHEMES.contains(normalizedScriptURL.scheme(), Qt::CaseInsensitive)) {
        return normalizedScriptURL;
    } else {
        return QUrl("");
    }
}


QObject* scriptsModel();

void ScriptEngines::addScriptEngine(ScriptEnginePointer engine) {
    if (!_isStopped) {
        QMutexLocker locker(&_allScriptsMutex);
        _allKnownScriptEngines.insert(engine);
    }
}

void ScriptEngines::releaseWhenFinished(ScriptEnginePointer engine) {
    connect(engine.data(), &ScriptEngine::finished, this, &ScriptEngines::onScriptFinished, Qt::DirectConnection);
}

void ScriptEngines::removeScriptEngine(ScriptEnginePointer engine) {
    // If we're not already in the middle of stopping all scripts, then we should remove ourselves
    // from the list of running scripts. We don't do this if we're in the process of stopping all scripts
    // because that method removes scripts from its list as it iterates them
    if (!_isStopped) {
        QMutexLocker locker(&_allScriptsMutex);
        _allKnownScriptEngines.remove(engine);
    }
}

void ScriptEngines::shutdownScripting() {
    _isStopped = true;
    QElapsedTimer stopping;
    stopping.start();
    QMutexLocker locker(&_allScriptsMutex);
    qCDebug(scriptengine) << "Stopping all scripts.... currently known scripts:" << _allKnownScriptEngines.size();

    // Every script is asked to stop first, so they all wind down at once, rather than each waiting for the one before
    QList<ScriptEnginePointer> stoppedEngines;
    for (const auto& scriptEngine : _allKnownScriptEngines) {
        // NOTE: typically all script engines are running. But there's at least one known exception to this, the
        // "entities sandbox" which is only used to evaluate entities scripts to test their validity before using
        // them. We don't need to stop scripts that aren't running.
        if (scriptEngine->isRunning()) {
            qCDebug(scriptengine) << "about to shutdown script:" << scriptEngine->getFilename();

            // We disconnect any script engine signals from the application because we don't want to do any
            // extra stopScript/loadScript processing that the Application normally does when scripts start
            // and stop. We can safely short circuit this because we know we're in the "quitting" process
            scriptEngine->disconnect(this);

            // Gracefully stop the engine's scripting thread
            scriptEngine->stop();
            stoppedEngines.append(scriptEngine);
        }
    }

    // We need to wait for each engine we stopped to be done running before we proceed, because we don't want any of
    // the scripts final "scriptEnding()" or pending "update()" methods from accessing any application state after we
    // leave this shutdownScripting() method. Every one of them is waited on, not just those still running: a script that
    // finished while we waited on an earlier one still has its thread to quit and join, which takes no time.
    for (const auto& scriptEngine : stoppedEngines) {
        QString scriptName = scriptEngine->getFilename();
        qCDebug(scriptengine) << "waiting on script:" << scriptName;
        scriptEngine->waitTillDoneRunning();
        qCDebug(scriptengine) << "done waiting on script:" << scriptName;
    }

    // Once the scripts are stopped, we can remove them from our set
    _allKnownScriptEngines.clear();
    qCDebug(scriptengine) << "DONE Stopping all scripts...." << stopping.elapsed() << "ms";
}

/**jsdoc
 * Information on a public script, i.e., a script that's included in the Interface installation.
 * @typedef {object} ScriptDiscoveryService.PublicScript
 * @property {string} name - The script's file name.
 * @property {string} type - <code>"script"</code> or <code>"folder"</code>.
 *     <p class="important">Deprecated: This property is deprecated and will be removed. It currently always has the value, 
 *     <code>"script"</code>.</p>
 * @property {ScriptDiscoveryService.PublicScript[]} [children] - Only present if <code>type == "folder"</code>.
 *     <p class="important">Deprecated: This property is deprecated and will be removed. It currently is never present.
 * @property {string} [url] - The full URL of the script &mdash; including the <code>"file:///"</code> scheme at the start.
 *     <p>Only present if <code>type == "script"</code>.</p>
 */
QVariantList getPublicChildNodes(TreeNodeFolder* parent) {
    QVariantList result;
    QList<TreeNodeBase*> treeNodes = getScriptsModel().getFolderNodes(parent);
    for (int i = 0; i < treeNodes.size(); i++) {
        TreeNodeBase* node = treeNodes.at(i);
        if (node->getType() == TREE_NODE_TYPE_FOLDER) {
            TreeNodeFolder* folder = static_cast<TreeNodeFolder*>(node);
            QVariantMap resultNode;
            resultNode.insert("name", node->getName());
            resultNode.insert("type", "folder");
            resultNode.insert("children", getPublicChildNodes(folder));
            result.append(resultNode);
            continue;
        }
        TreeNodeScript* script = static_cast<TreeNodeScript*>(node);
        if (script->getOrigin() == ScriptOrigin::SCRIPT_ORIGIN_LOCAL) {
            continue;
        }
        QVariantMap resultNode;
        resultNode.insert("name", node->getName());
        resultNode.insert("type", "script");
        resultNode.insert("url", script->getFullPath());
        result.append(resultNode);
    }
    return result;
}

QVariantList ScriptEngines::getPublic() {
    return getPublicChildNodes(NULL);
}

/**jsdoc
 * Information on a local script.
 * @typedef {object} ScriptDiscoveryService.LocalScript
 * @property {string} name - The script's file name.
 * @property {string} path - The script's path.
 * @deprecated This type is deprecated and will be removed.
 */
QVariantList ScriptEngines::getLocal() {
    QVariantList result;
    QList<TreeNodeBase*> treeNodes = getScriptsModel().getFolderNodes(NULL);
    for (int i = 0; i < treeNodes.size(); i++) {
        TreeNodeBase* node = treeNodes.at(i);
        if (node->getType() != TREE_NODE_TYPE_SCRIPT) {
            continue;
        }
        TreeNodeScript* script = static_cast<TreeNodeScript*>(node);
        if (script->getOrigin() != ScriptOrigin::SCRIPT_ORIGIN_LOCAL) {
            continue;
        }
        QVariantMap resultNode;
        resultNode.insert("name", node->getName());
        resultNode.insert("path", script->getFullPath());
        result.append(resultNode);
    }
    return result;
}

/**jsdoc
 * Information on a running script.
 * @typedef {object} ScriptDiscoveryService.RunningScript
 * @property {boolean} local - <code>true</code> if the script is a local file (i.e., the scheme is "file"), <code>false</code> 
 *     if it isn't (e.g., the scheme is "http").
 * @property {string} name - The script's file name.
 * @property {string} path - The script's path and file name &mdash; excluding the scheme if a local file.
 * @property {string} url - The full URL of the script &mdash; including the scheme if a local file.
 */
QVariantList ScriptEngines::getRunning() {
    QVariantList result;
    auto runningScripts = getRunningScripts();
    foreach(const QString& runningScript, runningScripts) {
        QUrl runningScriptURL = QUrl(runningScript);
        if (!runningScriptURL.isValid()) {
            runningScriptURL = QUrl::fromLocalFile(runningScriptURL.toDisplayString(QUrl::FormattingOptions(QUrl::FullyEncoded)));
        }
        QVariantMap resultNode;
        resultNode.insert("name", runningScriptURL.fileName());
        QUrl displayURL = expandScriptUrl(runningScriptURL);
        QString displayURLString;
        if (displayURL.isLocalFile()) {
            displayURLString = displayURL.toLocalFile();
        } else {
            displayURLString = displayURL.toDisplayString(QUrl::FormattingOptions(QUrl::FullyEncoded));
        }
        // The path contains the exact path/URL of the script, which also is used in the stopScript function.
        resultNode.insert("path", displayURLString);
        resultNode.insert("url", normalizeScriptURL(runningScript).toString());
        resultNode.insert("local", runningScriptURL.isLocalFile());
        result.append(resultNode);
    }
    return result;
}

void ScriptEngines::loadDefaultScripts() {
    loadScript(DEFAULT_SCRIPTS_LOCATION);
}

void ScriptEngines::loadOneScript(const QString& scriptFilename) {
    loadScript(scriptFilename);
}

void ScriptEngines::loadScripts() {
    // START BACKWARD COMPATIBILITY CODE
    // The following code makes sure people don't lose all their scripts
    // This should be removed after a reasonable ammount of time went by
    // Load old setting format if present
    bool foundDeprecatedSetting = false;
    Settings settings;
    int size = settings.beginReadArray(SETTINGS_KEY);
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QString string = settings.value("script").toString();
        if (!string.isEmpty()) {
            loadScript(string);
            foundDeprecatedSetting = true;
        }
    }
    settings.endArray();
    if (foundDeprecatedSetting) {
        // Remove old settings found and return
        settings.beginWriteArray(SETTINGS_KEY);
        settings.remove("");
        settings.endArray();
        settings.remove(SETTINGS_KEY + "/size");
        return;
    }
    // END BACKWARD COMPATIBILITY CODE

    // loads all saved scripts
    auto runningScripts = runningScriptsHandle.get();
    bool defaultScriptsOverrideSet = !_defaultScriptsOverride.isEmpty();
    // before the review, a saved script from the network waits for the visitor's word: only the app's own scripts and
    // files of this machine start on their own
    const bool reviewing = !savedScriptsReviewed();

    for (auto script : runningScripts) {
        auto url = script.toUrl();
        if (!url.isEmpty() && reviewing && url != DEFAULT_SCRIPTS_LOCATION) {
            const QUrl normalized = normalizeScriptURL(url);
            const QString scheme = normalized.scheme().toLower();
            // the app's own scripts and the visitor's own files start; anything from the network, or a file in a
            // folder the app writes, waits
            QUrl local = normalized;
            if (scheme == "file" && normalized.path().startsWith("/~/")) {
                local.setPath(expandScriptPath(normalized.path()));   // "/~/" is the app's scripts folder
            }
            // "/~/" counts only when it resolves inside the app's scripts (canonical: "/~/../" reaches nothing)
            const bool appScript = scheme == "qrc" || (scheme == "file" && ScriptNetworkPolicy::isAppScript(local));
            if (!appScript && (scheme != "file" || inAFolderTheAppWrites(local))) {
                _heldForReview << url.toString();
                continue;
            }
        }
        if (!url.isEmpty()) {
            // a script of the app's own that the app no longer has (deleted from the repo, as the old Create app's
            // system/create/edit.js was, 10-24) is left out, and so drops off the list when it is next saved.
            // expandScriptPath gives a URL path ("/C:/..." on Windows), so the file is looked for by its local path.
            const QUrl normalized = normalizeScriptURL(url);
            if (normalized.scheme() == "file" && normalized.path().startsWith("/~/")) {
                QUrl expanded = normalized;
                expanded.setPath(expandScriptPath(normalized.path()));
                if (!QFileInfo::exists(expanded.toLocalFile())) {
                    qCDebug(scriptengine) << "A saved script the app no longer has is left out:" << url;
                    continue;
                }
            }
            if (defaultScriptsOverrideSet && url == DEFAULT_SCRIPTS_LOCATION) {
                _defaultScriptsWasRunning = true;
            } else {
                loadScript(url);
            }
        }
    }

    if (defaultScriptsOverrideSet) {
        loadScript(_defaultScriptsOverride, false);
    }
    if (reviewing) {
        if (_heldForReview.isEmpty()) {
            markSavedScriptsReviewed();   // nothing to ask: silent, and not asked again
        } else {
            qCWarning(scriptengine) << _heldForReview.size() << "saved scripts wait for the visitor's review before they start:" << _heldForReview;
            emit savedScriptsNeedReview(_heldForReview);
        }
    }
}

void ScriptEngines::finishSavedScriptsReview(const QStringList& kept) {
    const QStringList held = _heldForReview;
    _heldForReview.clear();
    markSavedScriptsReviewed();   // only an answer marks it: a dialog closed unanswered asks again next start
    for (const QString& script : held) {
        if (kept.contains(script)) {
            loadScript(QUrl(script));
        }
    }
    saveScripts();   // the kept ones are running now, so they stay saved; the rest drop off the list
}

void ScriptEngines::saveScripts() {
    // Do not save anything if we are in the process of shutting down
    if (qApp->closingDown()) {
        qWarning() << "Trying to save scripts during shutdown.";
        return;
    }

    // don't save scripts if we started with --scripts, as we would overwrite
    // the scripts that the user expects to be there when launched without the
    // --scripts override.
    if (_defaultScriptsLocationOverridden) {
        runningScriptsHandle.set(QVariantList{ DEFAULT_SCRIPTS_LOCATION });
        return;
    }

    // Saves all currently running user-loaded scripts
    QVariantList list;

    {
        QReadLocker lock(&_scriptEnginesHashLock);
        for (auto it = _scriptEnginesHash.begin(); it != _scriptEnginesHash.end(); ++it) {
            // Save user-loaded scripts, only if they are set to quit when finished
            if (it.value() && it.value()->isUserLoaded()  && !it.value()->isQuitWhenFinished()) {
                auto normalizedUrl = normalizeScriptURL(it.key());
                list.append(normalizedUrl.toString());
            }
        }
    }

    if (_defaultScriptsWasRunning) {
        list.append(DEFAULT_SCRIPTS_LOCATION);
    }
    // a script held for the review stays saved until the visitor has answered (quitting first doesn't drop it)
    for (const QString& script : _heldForReview) {
        if (!list.contains(script)) {
            list.append(script);
        }
    }

    runningScriptsHandle.set(list);
}

QStringList ScriptEngines::getRunningScripts() {
    QReadLocker lock(&_scriptEnginesHashLock);
    QList<QUrl> urls = _scriptEnginesHash.keys();
    QStringList result;
    for (auto url : urls) {
        result.append(url.toString());
    }
    return result;
}

void ScriptEngines::stopAllScripts(bool restart) {
    QReadLocker lock(&_scriptEnginesHashLock);

    if (_isReloading) {
        return;
    }

    for (auto it = _scriptEnginesHash.constBegin();
        it != _scriptEnginesHash.constEnd(); it++) {
        ScriptEnginePointer scriptEngine = it.value();
        // skip already stopped scripts
        if (scriptEngine->isFinished() || scriptEngine->isStopping()) {
            continue;
        }

        bool isOverrideScript = it.key().toString().compare(this->_defaultScriptsOverride.toString()) == 0;
        // queue user scripts if restarting
        if (restart && (scriptEngine->isUserLoaded() || isOverrideScript)) {
            _isReloading = true;
            ScriptEngine::Type type = scriptEngine->getType();

            connect(scriptEngine.data(), &ScriptEngine::finished, this, [this, type, isOverrideScript] (QString scriptName) {
                reloadScript(scriptName, !isOverrideScript)->setType(type);
            });
        }

        // stop all scripts
        scriptEngine->stop();
    }

    if (restart) {
        qCDebug(scriptengine) << "stopAllScripts -- emitting scriptsReloading";
        QTimer::singleShot(RELOAD_ALL_SCRIPTS_TIMEOUT, this, [&] {
            _isReloading = false;
        });
        emit scriptsReloading();
    }
}

bool ScriptEngines::stopScript(const QString& rawScriptURL, bool restart) {
    bool stoppedScript = false;
    {
        QUrl scriptURL = normalizeScriptURL(QUrl(rawScriptURL));
        if (!scriptURL.isValid()) {
            scriptURL = normalizeScriptURL(QUrl::fromLocalFile(rawScriptURL));
        }

        QReadLocker lock(&_scriptEnginesHashLock);
        if (_scriptEnginesHash.contains(scriptURL)) {
            ScriptEnginePointer scriptEngine = _scriptEnginesHash.value(scriptURL);
            if (restart) {
                bool isUserLoaded = scriptEngine->isUserLoaded();
                ScriptEngine::Type type = scriptEngine->getType();
                // an avatar's script (the only one in this list with a world) comes back as one, in the same world
                // under the same rules, and not at all once that world is gone (the avatar was changed)
                const int world = scriptEngine->getWorldId();
                auto scriptCache = DependencyManager::get<ScriptCache>();
                scriptCache->deleteScript(scriptURL);

                if (!scriptEngine->isStopping()) {
                    connect(scriptEngine.data(), &ScriptEngine::finished,
                            this, [this, isUserLoaded, type, world](QString scriptName, ScriptEnginePointer engine) {
                            if (world != 0) {
                                if (type == ScriptEngine::Type::AVATAR && !ScriptCaller::isRetired(world)) {
                                    loadAvatarScript(scriptName, world);
                                }
                                return;
                            }
                            reloadScript(scriptName, isUserLoaded)->setType(type);
                    });
                }
            }
            scriptEngine->stop();
            stoppedScript = true;
        }
    }
    return stoppedScript;
}

void ScriptEngines::reloadLocalFiles() {
    _scriptsModel.reloadLocalFiles();
}

void ScriptEngines::reloadAllScripts() {
    qCDebug(scriptengine) << "reloadAllScripts -- clearing caches";
    DependencyManager::get<ScriptCache>()->clearCache();
    qCDebug(scriptengine) << "reloadAllScripts -- stopping all scripts";
    stopAllScripts(true);
}

void ScriptEngines::setBypassScriptThrottling(bool value) { 
    qCDebug(scriptengine) << "Bypass Script Engine Throttling " << value;
    _bypassScriptThrottling = value; 
};

bool ScriptEngines::getBypassScriptThrottling(){ return _bypassScriptThrottling; };

ScriptEnginePointer ScriptEngines::loadScript(const QUrl& scriptFilename, bool isUserLoaded, bool loadScriptFromEditor,
                                              bool activateMainWindow, bool reload, bool quitWhenFinished) {
    if (thread() != QThread::currentThread()) {
        ScriptEnginePointer result { nullptr };
        BLOCKING_INVOKE_METHOD(this, "loadScript", Q_RETURN_ARG(ScriptEnginePointer, result),
            Q_ARG(QUrl, scriptFilename),
            Q_ARG(bool, isUserLoaded),
            Q_ARG(bool, loadScriptFromEditor),
            Q_ARG(bool, activateMainWindow),
            Q_ARG(bool, reload),
            Q_ARG(bool, quitWhenFinished));
        return result;
    }
    return loadScriptEngine(scriptFilename, isUserLoaded, reload, quitWhenFinished, nullptr);
}

ScriptEnginePointer ScriptEngines::loadAvatarScript(const QUrl& scriptFilename, int world) {
    if (thread() != QThread::currentThread()) {
        ScriptEnginePointer result { nullptr };
        QMetaObject::invokeMethod(this, [&] { result = loadAvatarScript(scriptFilename, world); },
                                  Qt::BlockingQueuedConnection);
        return result;
    }
    return loadScriptEngine(scriptFilename, false, false, false, [world](const ScriptEnginePointer& engine) {
        engine->setType(ScriptEngine::Type::AVATAR);
        engine->setWorldId(world);
    });
}

ScriptEnginePointer ScriptEngines::loadScriptEngine(const QUrl& scriptFilename, bool isUserLoaded, bool reload,
                                                    bool quitWhenFinished,
                                                    const std::function<void(const ScriptEnginePointer&)>& prepare) {
    QUrl scriptUrl;
    if (!KNOWN_SCHEMES.contains(scriptFilename.scheme(), Qt::CaseInsensitive)) {
        // deal with a "url" like c:/something
        scriptUrl = normalizeScriptURL(QUrl::fromLocalFile(scriptFilename.toString()));
    } else {
        scriptUrl = normalizeScriptURL(scriptFilename);
    }

    scriptUrl = QUrl(FileUtils::selectFile(scriptUrl.toString()));

    auto scriptEngine = getScriptEngine(scriptUrl);
    if (scriptEngine && !scriptEngine->isStopping()) {
        return scriptEngine;
    }

    scriptEngine = scriptEngineFactory(_context, NO_SCRIPT, "about:" + scriptFilename.fileName(), scriptUrl);
    scriptEngine->setUserLoaded(isUserLoaded);
    scriptEngine->setQuitWhenFinished(quitWhenFinished);
    if (prepare) {
        // before the fetch: a cached script's load, and so its run, can follow at once
        prepare(scriptEngine);
    }

    if (scriptFilename.isEmpty() || !scriptUrl.isValid() || scriptFilename == QUrl(ScriptEngine::CONSOLE_SCRIPT_NAME)) {
        // nothing to fetch (the console's engine runs only what is typed into it): it starts at once, outside the
        // list of running scripts
        launchScriptEngine(scriptEngine);
    } else {
        // connect to the appropriate signals of this script engine
        connect(scriptEngine.data(), &ScriptEngine::scriptLoaded, this, &ScriptEngines::onScriptEngineLoaded);
        connect(scriptEngine.data(), &ScriptEngine::errorLoadingScript, this, &ScriptEngines::onScriptEngineError);

        // Shutdown Interface when script finishes, if requested
        if (quitWhenFinished) {
            connect(scriptEngine.data(), &ScriptEngine::finished, this, &ScriptEngines::quitWhenFinished);
        }

        // get the script engine object to load the script at the designated script URL
        scriptEngine->loadURL(scriptUrl, reload);
    }

    return scriptEngine;
}

ScriptEnginePointer ScriptEngines::getScriptEngine(const QUrl& rawScriptURL) {
    ScriptEnginePointer result;
    {
        QReadLocker lock(&_scriptEnginesHashLock);
        const QUrl scriptURL = normalizeScriptURL(rawScriptURL);
        auto it = _scriptEnginesHash.find(scriptURL);
        if (it != _scriptEnginesHash.end()) {
            result = it.value();
        }
    }
    return result;
}

// FIXME - change to new version of ScriptCache loading notification
void ScriptEngines::onScriptEngineLoaded(const QString& rawScriptURL) {
    ScriptEnginePointer scriptEngine = qobject_cast<ScriptEngine*>(sender())->sharedFromThis();

    launchScriptEngine(scriptEngine);

    {
        QWriteLocker lock(&_scriptEnginesHashLock);
        QUrl url = QUrl(rawScriptURL);
        QUrl normalized = normalizeScriptURL(url);
        _scriptEnginesHash.insert(normalized, scriptEngine);
    }

    // Update settings with new script
    saveScripts();
    emit scriptCountChanged();
}

void ScriptEngines::quitWhenFinished() {
    qApp->quit();
}

int ScriptEngines::runScriptInitializers(ScriptEnginePointer scriptEngine) {
    auto nativeCount = DependencyManager::get<ScriptInitializers>()->runScriptInitializers(
        [scriptEngine](const QString& name, QObject* object) { scriptEngine->registerGlobalObject(name, object); });
    return nativeCount + ScriptInitializerMixin<ScriptEnginePointer>::runScriptInitializers(scriptEngine);
}

void ScriptEngines::launchScriptEngine(ScriptEnginePointer scriptEngine) {
    connect(scriptEngine.data(), &ScriptEngine::finished, this, &ScriptEngines::onScriptFinished, Qt::DirectConnection);
    connect(scriptEngine.data(), &ScriptEngine::loadScript, [this](const QString& scriptName, bool userLoaded) {
        loadScript(scriptName, userLoaded);
    });
    connect(scriptEngine.data(), &ScriptEngine::reloadScript, [this](const QString& scriptName, bool userLoaded) {
        loadScript(scriptName, userLoaded, false, false, true);
    });

    // register our application services and set it off on its own thread
    runScriptInitializers(scriptEngine);
    scriptEngine->runInThread();
}

void ScriptEngines::onScriptFinished(const QString& rawScriptURL, ScriptEnginePointer engine) {
    bool removed = false;
    {
        QWriteLocker lock(&_scriptEnginesHashLock);
        const QUrl scriptURL = normalizeScriptURL(QUrl(rawScriptURL));
        for (auto it = _scriptEnginesHash.find(scriptURL); it != _scriptEnginesHash.end(); ++it) {
            if (it.value() == engine) {
                _scriptEnginesHash.erase(it);
                removed = true;
                break;
            }
        }
    }

    removeScriptEngine(engine);

    if (removed && !_isReloading) {
        // Update settings with removed script
        saveScripts();
        emit scriptCountChanged();
    }
}

// FIXME - change to new version of ScriptCache loading notification
void ScriptEngines::onScriptEngineError(const QString& scriptFilename) {
    qCDebug(scriptengine) << "Application::loadScript(), script failed to load...";
    emit scriptLoadError(scriptFilename, "");
}

QString ScriptEngines::getDefaultScriptsLocation() const {
    return PathUtils::defaultScriptsLocation().toString();
}
