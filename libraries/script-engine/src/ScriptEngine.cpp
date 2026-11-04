//
//  ScriptEngine.cpp
//  libraries/script-engine/src
//
//  Created by Brad Hefta-Gaub on 12/14/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ScriptEngine.h"

#include <atomic>
#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>

#include <QtCore/QFuture>
#include <QtConcurrent/QtConcurrentRun>

#include "WasmScriptEngine.h"

#include <DebugDraw.h>
#include <EntityScriptingInterface.h>
#include <MessagesClient.h>
#include <PathUtils.h>
#include <ResourceManager.h>
#include <ResourceScriptingInterface.h>
#include <ScriptNetworkPolicy.h>
#include "ScriptCaller.h"

#include "AssetScriptingInterface.h"
#include "FileScriptingInterface.h" // unzip project
#include "ModelScriptingInterface.h"
#include "ScriptCache.h"
#include "ScriptEngineLogging.h"
#include "ScriptEngines.h"
#include "StackTestScriptingInterface.h"
#include "WorldMessages.h"

#include <Profile.h>

int scriptEnginePointerMetaID = qRegisterMetaType<ScriptEnginePointer>();

ScriptEnginePointer scriptEngineFactory(ScriptEngine::Context context,
                                                 const QString& scriptContents,
                                                 const QString& fileNameString,
                                                 const QUrl& scriptURL) {
    // every script runs on the WebAssembly engine: a module as it is, JavaScript inside the guest module
    Q_UNUSED(scriptURL);
    ScriptEngine* engine = new WasmScriptEngine(context, scriptContents, fileNameString);
    ScriptEnginePointer engineSP = ScriptEnginePointer(engine, &QObject::deleteLater);
    auto scriptEngines = DependencyManager::get<ScriptEngines>();
    scriptEngines->addScriptEngine(qSharedPointerCast<ScriptEngine>(engineSP));
    engine->setScriptEngines(scriptEngines);
    return engineSP;
}

int ScriptEngine::processLevelMaxRetries { ScriptRequest::MAX_RETRIES };
const QString ScriptEngine::CONSOLE_SCRIPT_NAME { "about:console" };

ScriptEngine::ScriptEngine(Context context, const QString& scriptContents, const QString& fileNameString) :
    _context(context),
    _scriptContents(scriptContents),
    _fileNameString(fileNameString),
    _quatLibrary(new Quat(this)),
    _vec3Library(new Vec3(this)),
    _mat4Library(new Mat4(this)),
    _uuidLibrary(new ScriptUUID(this)),
    _consoleScriptingInterface(new ConsoleScriptingInterface(this)),
    _assetScriptingInterface(new AssetScriptingInterface(this))
{
    // held for the engine's life: what the agent's scripts include later never changes it
    _agentScriptIsLocal = _context == AGENT_SCRIPT &&
                          (fileNameString.isEmpty() || ScriptNetworkPolicy::isLocalOrigin(QUrl(fileNameString)));
    switch (_context) {
        case Context::CLIENT_SCRIPT:
            _type = Type::CLIENT;
            break;
        case Context::ENTITY_CLIENT_SCRIPT:
            _type = Type::ENTITY_CLIENT;
            break;
        case Context::ENTITY_SERVER_SCRIPT:
            _type = Type::ENTITY_SERVER;
            break;
        case Context::AGENT_SCRIPT:
            _type = Type::AGENT;
            break;
    }

    if (isEntityServerScript()) {
        qCDebug(scriptengine) << "isEntityServerScript() -- limiting maxRetries to 1";
        processLevelMaxRetries = 1;
    }
}

QString ScriptEngine::getTypeAsString() const {
    auto value = QVariant::fromValue(_type).toString();
    return value.isEmpty() ? "unknown" : value.toLower();
}

QString ScriptEngine::getContext() const {
    switch (_context) {
        case CLIENT_SCRIPT:
            return "client";
        case ENTITY_CLIENT_SCRIPT:
            return "entity_client";
        case ENTITY_SERVER_SCRIPT:
            return "entity_server";
        case AGENT_SCRIPT:
            return "agent";
        default:
            return "unknown";
    }
    return "unknown";
}

bool ScriptEngine::isDebugMode() const {
#if defined(DEBUG)
    return true;
#else
    return false;
#endif
}

ScriptEngine::~ScriptEngine() {}

void ScriptEngine::disconnectNonEssentialSignals() {
    disconnect();
    QThread* workerThread;
    // Ensure the thread should be running, and does exist
    if (_isRunning && _isThreaded && (workerThread = thread())) {
        connect(this, &QObject::destroyed, workerThread, &QThread::quit);
        connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);
    }
}

void ScriptEngine::runInThread() {
    Q_ASSERT_X(!_isThreaded, "ScriptEngine::runInThread()", "runInThread should not be called more than once");

    if (_isThreaded) {
        return;
    }

    _isThreaded = true;

    // The thread interface cannot live on itself, and we want to move this into the thread, so
    // the thread cannot have this as a parent.
    QThread* workerThread = new QThread();
    workerThread->setObjectName(QString("js:") + getFilename().replace("about:",""));
    workerThread->setProperty("scriptUrl", _fileNameString);
    moveToThread(workerThread);

    // NOTE: If you connect any essential signals for proper shutdown or cleanup of
    // the script engine, make sure to add code to "reconnect" them to the
    // disconnectNonEssentialSignals() method
    connect(workerThread, &QThread::started, this, &ScriptEngine::run);
    connect(this, &QObject::destroyed, workerThread, &QThread::quit);
    connect(workerThread, &QThread::finished, workerThread, &QObject::deleteLater);

    workerThread->start();
}

void ScriptEngine::executeOnScriptThread(std::function<void()> function, const Qt::ConnectionType& type ) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "executeOnScriptThread", type, Q_ARG(std::function<void()>, function));
        return;
    }

    function();
}

bool ScriptEngine::worldWasLeft() const {
    return ScriptCaller::isRetired(_worldId);
}

QString ScriptEngine::getFilename() const {
    QStringList fileNameParts = _fileNameString.split("/");
    QString lastPart;
    if (!fileNameParts.isEmpty()) {
        lastPart = fileNameParts.last();
    }
    return lastPart;
}

bool ScriptEngine::hasValidScriptSuffix(const QString& scriptFileName) {
    QFileInfo fileInfo(scriptFileName);
    QString scriptSuffixToLower = fileInfo.completeSuffix().toLower();
    return scriptSuffixToLower.contains(QString("js"), Qt::CaseInsensitive);
}

void ScriptEngine::loadURL(const QUrl& scriptURL, bool reload) {
    if (_isRunning) {
        return;
    }

    QUrl url = expandScriptUrl(scriptURL);
    _fileNameString = url.toString();
    _isReloading = reload;

    // Check that script has a supported file extension
    if (!hasValidScriptSuffix(_fileNameString)) {
        scriptErrorMessage("File extension of file: " + _fileNameString + " is not a currently supported script type");
        emit errorLoadingScript(_fileNameString);
        return;
    }

    const auto maxRetries = 0; // for consistency with previous scriptCache->getScript() behavior
    auto scriptCache = DependencyManager::get<ScriptCache>();
    scriptCache->getScriptContents(url.toString(), [this](const QString& url, const QString& scriptContents, bool isURL, bool success, const QString&status) {
        qCDebug(scriptengine) << "loadURL" << url << status << QThread::currentThread();
        if (!success) {
            scriptErrorMessage("ERROR Loading file (" + status + "):" + url);
            emit errorLoadingScript(_fileNameString);
            return;
        }

        _scriptContents = scriptContents;
        emit scriptLoaded(url);
    }, reload, maxRetries);
}

QUrl ScriptEngine::currentScriptOrigin() const {
    QUrl url(_fileNameString);
    if (url.scheme().isEmpty() && !_fileNameString.isEmpty()) {
        url = QUrl::fromLocalFile(_fileNameString);
    }
    return url;
}

bool ScriptEngine::entityScriptsMayBeLocal() const {
    if (_context != ENTITY_CLIENT_SCRIPT || _servedNonLocalWorld) {
        return false;
    }
    // decided by the world the engine serves, never by anything a script passes
    if (ScriptNetworkPolicy::isStrict() || !ScriptNetworkPolicy::isWorldLocal()) {
        _servedNonLocalWorld = true;
        return false;
    }
    return true;
}

bool ScriptEngine::worldIsLocal() const {
    if (!isClientSide()) {
        return false;   // a server is in no world of a visitor's
    }
    return _context == ENTITY_CLIENT_SCRIPT ? entityScriptsMayBeLocal() : ScriptNetworkPolicy::isWorldLocal();
}

QUrl ScriptEngine::ruledOrigin(const QUrl& origin) const {
    if (isClientSide() || !ScriptNetworkPolicy::isLocalOrigin(origin)) {
        return origin;
    }
    return QUrl(_context == AGENT_SCRIPT ? "about:agent" : "about:entity-server");
}

QUrl ScriptEngine::effectiveSandbox(const QUrl& sandbox) const {
    if (_context != ENTITY_CLIENT_SCRIPT || !ScriptNetworkPolicy::isLocalOrigin(sandbox) || entityScriptsMayBeLocal()) {
        return sandbox;
    }
    return QUrl("about:Entities");
}

QString ScriptEngine::entityScriptRefusal(const QString& entityScript) const {
    if (_context != ENTITY_CLIENT_SCRIPT || entityScriptsMayBeLocal()) {
        return QString();
    }
    // the address as the script cache reads it: code given as the script, or a javascript: URL, is no address; an
    // address the cache takes as a file here (a file URL, a path, an unknown scheme) is judged as that file
    if (ScriptCache::isInlineScript(entityScript) || QUrl(entityScript).scheme().toLower() == "javascript") {
        return QString();
    }
    const QUrl url = DependencyManager::get<ResourceManager>()->normalizeURL(QUrl(entityScript));
    if (!url.isLocalFile() || ScriptNetworkPolicy::isAppScript(url)) {
        return QString();
    }
    return "a file of this machine, in a world that isn't served from it or its network";
}

QString ScriptEngine::ruleEntityScriptFetch(const QString& entityScript) {
    if (!networkRuled() || ScriptCache::isInlineScript(entityScript) || QUrl(entityScript).scheme().toLower() == "javascript") {
        return QString();
    }
    const QUrl url = DependencyManager::get<ResourceManager>()->normalizeURL(QUrl(entityScript));
    if (ScriptNetworkPolicy::isAppScript(url)) {
        return QString();   // one of the app's own scripts (qrc, or a file in its scripts folder) is no request
    }
    const QUrl origin(isClientSide() ? "about:Entities" : "about:entity-server");
    const ScriptNetworkPolicy::Decision decision = ScriptNetworkPolicy::check(url, origin, worldIsLocal());
    if (!decision.allowed && !decision.needsLookup) {
        return decision.reason;
    }
    DependencyManager::get<ScriptCache>()->ruleFetch(url, origin, worldIsLocal());
    return QString();
}

void ScriptEngine::scriptErrorMessage(const QString& message) {
    qCCritical(scriptengine, "[%s] %s", qUtf8Printable(getFilename()), qUtf8Printable(message));
    emit errorMessage(message, getFilename());
}

void ScriptEngine::scriptWarningMessage(const QString& message) {
    qCWarning(scriptengine, "[%s] %s", qUtf8Printable(getFilename()), qUtf8Printable(message));
    emit warningMessage(message, getFilename());
}

void ScriptEngine::scriptInfoMessage(const QString& message) {
    qCInfo(scriptengine, "[%s] %s", qUtf8Printable(getFilename()), qUtf8Printable(message));
    emit infoMessage(message, getFilename());
}

void ScriptEngine::scriptPrintedMessage(const QString& message) {
    qCDebug(scriptengine, "[%s] %s", qUtf8Printable(getFilename()), qUtf8Printable(message));
    emit printedMessage(message, getFilename());
}

void ScriptEngine::clearDebugLogWindow() {
    emit clearDebugWindow();
}

// The globals every engine gives a script, through the register calls the engine implements (the WebAssembly
// engine records them in its object table). The objects made here for the script alone (File, Model) are the
// engine's children.
void ScriptEngine::registerStandardGlobals() {
    // NOTE: You do not want to end up creating new instances of singletons here. They will be on the ScriptEngine thread
    // and are likely to be unusable if we "reset" the ScriptEngine by creating a new one (on a whole new thread).

    registerGlobalObject("Script", this);
    registerGlobalObject("Audio", DependencyManager::get<AudioScriptingInterface>().data());

    registerGlobalObject("Entities", DependencyManager::get<EntityScriptingInterface>().data());
    registerGlobalObject("Quat", _quatLibrary);
    registerGlobalObject("Vec3", _vec3Library);
    registerGlobalObject("Mat4", _mat4Library);
    registerGlobalObject("Uuid", _uuidLibrary);
    // a world's entity scripts get their own Messages: the world's send path, the app's channels refused (WorldMessages)
    registerGlobalObject("Messages", isEntityClientScript() ? static_cast<QObject*>(new WorldMessages(this))
                                                            : DependencyManager::get<MessagesClient>().data());
    registerGlobalObject("File", new FileScriptingInterface(this));
    registerGlobalObject("console", _consoleScriptingInterface);

    registerGlobalObject("Assets", _assetScriptingInterface);
    registerGlobalObject("Resources", DependencyManager::get<ResourceScriptingInterface>().data());

    registerGlobalObject("DebugDraw", &DebugDraw::getInstance());

    registerGlobalObject("Model", new ModelScriptingInterface(this));
#if DEV_BUILD || PR_BUILD
    registerGlobalObject("StackTest", new StackTestScriptingInterface(this));
#endif
}

// A relative path is resolved against the script's own file (the JavaScript guest resolves against the file whose
// code runs, through Script.resolvePathFrom).
QUrl ScriptEngine::resolvePath(const QString& include) const {
    QUrl url(include);
    // first lets check to see if it's already a full URL -- or a Windows path like "c:/"
    if (include.startsWith("/") || url.scheme().length() == 1) {
        url = QUrl::fromLocalFile(include);
    }
    if (!url.isRelative()) {
        return expandScriptUrl(url);
    }

    // we apparently weren't a fully qualified url, so, let's assume we're relative to the script engine URL
    QUrl parentURL = QUrl(_fileNameString);

    // if still relative and path-like, then this is probably a local file...
    if (parentURL.isRelative() && url.path().contains("/")) {
        parentURL = QUrl::fromLocalFile(_fileNameString);
    }

    // at this point we should have a legitimate fully qualified URL for our parent
    url = expandScriptUrl(parentURL.resolved(url));
    return url;
}

QUrl ScriptEngine::resourcesPath() const {
    return QUrl(PathUtils::resourcesUrl());
}

QString ScriptEngine::dumpApiFile(const QString& requested, bool anyPath) const {
    if (anyPath) {
        return requested;
    }
    QDir folder(QDir(PathUtils::getAppDataPath()).filePath("api-dumps"));
    folder.mkpath(".");
    const QString name = QFileInfo(requested).fileName();
    return folder.filePath(name.isEmpty() ? QStringLiteral("api.json") : name);
}

void ScriptEngine::print(const QString& message) {
    emit printedMessage(message, getFilename());
}


void ScriptEngine::beginProfileRange(const QString& label) const {
    PROFILE_SYNC_BEGIN(script, label.toStdString().c_str(), label.toStdString().c_str());
}

void ScriptEngine::endProfileRange(const QString& label) const {
    PROFILE_SYNC_END(script, label.toStdString().c_str(), label.toStdString().c_str());
}

// NOTE: The load() command is similar to the include() command except that it loads the script
// as a stand-alone script. To accomplish this, the ScriptEngine class just emits a signal which
// the Application or other context will connect to in order to know to actually load the script
void ScriptEngine::load(const QString& loadFile) {
    if (QThread::currentThread() != thread()) {
        qCWarning(scriptengine) << "Script.load called from another thread than the engine's; ignored:" << loadFile;
        return;
    }
    QSharedPointer<ScriptEngines> scriptEngines(_scriptEngines);
    if (!scriptEngines || scriptEngines->isStopped()) {
        scriptWarningMessage("Script.load() while shutting down is ignored... loadFile:"
                + loadFile + "parent script:" + getFilename());
        return; // bail early
    }

    QUrl url = resolvePath(loadFile);
    if (_isReloading) {
        auto scriptCache = DependencyManager::get<ScriptCache>();
        scriptCache->deleteScript(url.toString());
        emit reloadScript(url.toString(), false);
    } else {
        emit loadScript(url.toString(), false);
    }
}

int ScriptEngine::getNumRunningEntityScripts() const {
    QReadLocker locker { &_entityScriptsLock };
    int sum = 0;
    for (const auto& st : _entityScripts) {
        if (st.status == EntityScriptStatus::RUNNING) {
            ++sum;
        }
    }
    return sum;
}

void ScriptEngine::setEntityScriptDetails(const EntityItemID& entityID, const EntityScriptDetails& details) {
    {
        QWriteLocker locker { &_entityScriptsLock };
        _entityScripts[entityID] = details;
    }
    emit entityScriptDetailsUpdated();
}

void ScriptEngine::updateEntityScriptStatus(const EntityItemID& entityID, const EntityScriptStatus &status, const QString& errorInfo) {
    {
        QWriteLocker locker { &_entityScriptsLock };
        EntityScriptDetails& details = _entityScripts[entityID];
        details.status = status;
        details.errorInfo = errorInfo;
    }
    emit entityScriptDetailsUpdated();
}

QVariant ScriptEngine::cloneEntityScriptDetails(const EntityItemID& entityID) {
    static const QVariant NULL_VARIANT { QVariant::fromValue((QObject*)nullptr) };
    QVariantMap map;
    if (entityID.isNull()) {
        // TODO: find better way to report JS Error across thread/process boundaries
        map["isError"] = true;
        map["errorInfo"] = "Error: getEntityScriptDetails -- invalid entityID";
    } else {
#ifdef DEBUG_ENTITY_STATES
        qDebug() << "cloneEntityScriptDetails" << entityID << QThread::currentThread();
#endif
        EntityScriptDetails scriptDetails;
        if (getEntityScriptDetails(entityID, scriptDetails)) {
#ifdef DEBUG_ENTITY_STATES
            qDebug() << "gotEntityScriptDetails" << scriptDetails.status << QThread::currentThread();
#endif
            map["isRunning"] = isEntityScriptRunning(entityID);
            map["status"] = EntityScriptStatus_::valueToKey(scriptDetails.status).toLower();
            map["errorInfo"] = scriptDetails.errorInfo;
            map["entityID"] = entityID.toString();
#ifdef DEBUG_ENTITY_STATES
            {
                auto debug = QVariantMap();
                debug["script"] = scriptDetails.scriptText;
                debug["lastModified"] = (qlonglong)scriptDetails.lastModified;
                debug["sandboxURL"] = scriptDetails.definingSandboxURL;
                map["debug"] = debug;
            }
#endif
        } else {
#ifdef DEBUG_ENTITY_STATES
            qDebug() << "!gotEntityScriptDetails" <<  QThread::currentThread();
#endif
            map["isError"] = true;
            map["errorInfo"] = "Entity script details unavailable";
            map["entityID"] = entityID.toString();
        }
    }
    return map;
}

QFuture<QVariant> ScriptEngine::getLocalEntityScriptDetails(const EntityItemID& entityID) {
    return QtConcurrent::run([this, entityID] { return cloneEntityScriptDetails(entityID); });
}

bool ScriptEngine::getEntityScriptDetails(const EntityItemID& entityID, EntityScriptDetails &details) const {
    QReadLocker locker { &_entityScriptsLock };
    auto it = _entityScripts.constFind(entityID);
    if (it == _entityScripts.constEnd()) {
        return false;
    }
    details = it.value();
    return true;
}

bool ScriptEngine::hasEntityScriptDetails(const EntityItemID& entityID) const {
    QReadLocker locker { &_entityScriptsLock };
    return _entityScripts.contains(entityID);
}

QList<EntityItemID> ScriptEngine::getListOfEntityScriptIDs() {
    QReadLocker locker{ &_entityScriptsLock };
    return _entityScripts.keys();
}

const int MAX_ENTITY_UNLOAD_WAIT_MS = 3000; // for a world's unload handlers, at quit or a world change
const int SERVING_POLL_MS = 5;

bool ScriptEngine::waitServingCalls(const std::function<bool()>& finished, int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!finished()) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        if (elapsed.elapsed() >= milliseconds) {
            return false;
        }
        QThread::msleep(SERVING_POLL_MS);
    }
    return true;
}

void ScriptEngine::waitForEntityScriptsToUnload() {
    QThread* engineThread = thread();
    if (!engineThread || !engineThread->isRunning()) {
        return; // nothing runs there, so no unload handler will
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    QMetaObject::invokeMethod(this, [this, done] {
        unloadAllEntityScripts(false);
        done->store(true);
    }, Qt::QueuedConnection);
    if (!waitServingCalls([done] { return done->load(); }, MAX_ENTITY_UNLOAD_WAIT_MS)) {
        qCWarning(scriptengine) << "The entity scripts of" << getFilename() << "did not unload within"
                                << MAX_ENTITY_UNLOAD_WAIT_MS << "ms; going on without waiting for them";
    }
}
