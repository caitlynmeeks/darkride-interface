//
//  ScriptEngine.h
//  libraries/script-engine/src
//
//  Created by Brad Hefta-Gaub on 12/14/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ScriptEngine_h
#define hifi_ScriptEngine_h

#include <functional>
#include <unordered_map>
#include <vector>

#include <memory>
#include <QtCore/QObject>
#include <QtCore/QUrl>
#include <QtCore/QSet>
#include <QtCore/QWaitCondition>
#include <QtCore/QStringList>

#include <AnimationCache.h>
#include <AvatarData.h>
#include <AvatarHashMap.h>
#include <LimitedNodeList.h>
#include <EntityItemID.h>
#include <EntitiesScriptEngineProvider.h>
#include <EntityScriptUtils.h>

#include "PointerEvent.h"
#include "AssetScriptingInterface.h"
#include "AudioScriptingInterface.h"
#include "BaseScriptEngine.h"
#include "Quat.h"
#include "Mat4.h"
#include "ScriptCache.h"
#include "ScriptUUID.h"
#include "Vec3.h"
#include "ConsoleScriptingInterface.h"
#include "SettingHandle.h"
#include "Profile.h"

static const QString NO_SCRIPT("");

static const int SCRIPT_FPS = 60;
static const int DEFAULT_MAX_ENTITY_PPS = 9000;
static const int DEFAULT_ENTITY_PPS_PER_SCRIPT = 900;

class ScriptEngines;

Q_DECLARE_METATYPE(ScriptEnginePointer)

class EntityScriptDetails {
public:
    EntityScriptStatus status { EntityScriptStatus::PENDING };

    // If status indicates an error, this contains a human-readable string giving more information about the error.
    QString errorInfo { "" };

    QString scriptText { "" };
    int64_t lastModified { 0 };
    QUrl definingSandboxURL { QUrl("about:EntityScript") };
};

/**jsdoc
 * The <code>Script</code> API provides facilities for working with scripts.
 *
 * @namespace Script
 *
 * @hifi-interface
 * @hifi-client-entity
 * @hifi-avatar
 * @hifi-server-entity
 * @hifi-assignment-client
 *
 * @property {string} context - The context that the script is running in:
 *     <ul>
 *       <li><code>"client"</code>: An Interface or avatar script.</li>
 *       <li><code>"entity_client"</code>: A client entity script.</li>
 *       <li><code>"entity_server"</code>: A server entity script.</li>
 *       <li><code>"agent"</code>: An assignment client script.</li>
 *     </ul>
 *     <em>Read-only.</em>
 * @property {string} type - The type of script that is running:
 *     <ul>
 *       <li><code>"client"</code>: An Interface script.</li>
 *       <li><code>"entity_client"</code>: A client entity script.</li>
 *       <li><code>"avatar"</code>: An avatar script.</li>
 *       <li><code>"entity_server"</code>: A server entity script.</li>
 *       <li><code>"agent"</code>: An assignment client script.</li>
 *     </ul>
 *     <em>Read-only.</em>
 * @property {string} filename - The filename of the script file.
 *     <em>Read-only.</em>
 */
/// The script engine as its hosts see it (the application, the agent, the entity script server, the entities
/// renderer): what every engine shares, whatever runs the scripts. A ScriptEngine is the `Script` global of its
/// scripts (the Q_INVOKABLEs and signals below are the Script API they see by reflection), keeps the engine's
/// context and type, its file, its thread, the records of the entity scripts it runs, the rules its scripts'
/// requests and files are judged by, and the messages it prints. The registrations the hosts make, the running of
/// scripts and the entity-script contract are the engine's own: WasmScriptEngine, the one engine, which runs
/// WebAssembly modules and JavaScript in the JavaScript guest module. QScriptEngine stays the base class until the
/// last QtScript registrations the hosts make are gone (the Qt 6 move, stage 0); nothing here runs on it.
class ScriptEngine : public BaseScriptEngine, public EntitiesScriptEngineProvider {
    Q_OBJECT
    Q_PROPERTY(QString context READ getContext)
    Q_PROPERTY(QString type READ getTypeAsString)
    Q_PROPERTY(QString fileName MEMBER _fileNameString CONSTANT)
public:

    enum Context {
        CLIENT_SCRIPT,
        ENTITY_CLIENT_SCRIPT,
        ENTITY_SERVER_SCRIPT,
        AGENT_SCRIPT
    };

    enum Type {
        CLIENT,
        ENTITY_CLIENT,
        ENTITY_SERVER,
        AGENT,
        AVATAR
    };
    Q_ENUM(Type)

    static int processLevelMaxRetries;
    ScriptEngine(Context context, const QString& scriptContents = NO_SCRIPT, const QString& fileNameString = QString("about:ScriptEngine"));
    ~ScriptEngine();

    /// run the script in a dedicated thread. This will have the side effect of evalulating
    /// the current script contents and calling run(). Callers will likely want to register the script with external
    /// services before calling this.
    void runInThread();

    /// run the script in the callers thread, exit when stop() is called.
    virtual void run() = 0;

    QString getFilename() const;
    /// The URL the running code came from: this script's own file (an entity script's own origin is the engine's to
    /// answer: verifiedScriptOrigin). The network rule for a script's own requests (ScriptNetworkPolicy) judges them
    /// by it.
    QUrl currentScriptOrigin() const;
    /// The origin for a call that names where its code came from (the WebAssembly guest does, for the entity script
    /// running): the claim counts only when the engine loaded such a script itself; otherwise the engine's own
    /// answer.
    virtual QUrl verifiedScriptOrigin(const QString& claimed) const { Q_UNUSED(claimed); return currentScriptOrigin(); }
    /// Whether this engine's entity scripts may count as local: an entities engine on a client whose world is served
    /// from this machine or its network, with the strict switch off. Held one way: once the engine has served a world
    /// that isn't local, it never counts local again (an engine serves one world; the next world gets a new engine).
    bool entityScriptsMayBeLocal() const;
    /// Whether the world this engine's scripts are judged in is local, for the network rule: a client's entities engine
    /// goes by its own world, held one way (entityScriptsMayBeLocal); any other engine by the world the client is in
    /// now.
    bool worldIsLocal() const;
    /// An entity script's address as its rights go: which files here include and require may reach, and the origin
    /// the network rule judges its requests by. On a client whose world isn't local, no entity script is local,
    /// whatever its own address (a file here, one of the app's own scripts, an address on this machine or its
    /// network), and such an address comes back as about:Entities. Any other engine's scripts, and addresses that
    /// aren't local, keep their own.
    QUrl effectiveSandbox(const QUrl& sandbox) const;
    /// Why this engine won't run an entity script from this address, or empty: on a client whose world isn't local, a
    /// file of this machine that isn't one of the app's own scripts.
    QString entityScriptRefusal(const QString& entityScript) const;
    /// The network rule for an entity script's own download, on an engine the rule holds (a world's entities engine on
    /// a client, the entity script server's), as for an include: judged as the world's (about:Entities on a client, by
    /// its world; about:entity-server on the server, as from the network), and its download sent to the address
    /// judged, each redirect judged again (ScriptCache::ruleFetch). Called just before the download is asked for.
    /// Empty when it may be fetched, else why not (a scheme or address refused before any name lookup; a name that
    /// leads to a private address fails the download itself).
    QString ruleEntityScriptFetch(const QString& entityScript);
    /// Whether this engine runs on a person's client, where its scripts' requests are held to that rule.
    bool isClientSide() const { return _context == CLIENT_SCRIPT || _context == ENTITY_CLIENT_SCRIPT; }
    /// Whether the network rule (ScriptNetworkPolicy) holds this engine's requests, sockets, includes and requires: a
    /// client's engines always (judged by each script's origin and the world); the entity script server's always;
    /// an agent's unless its assigned script is the operator's own (served by the domain server, or from this machine
    /// or its network), decided once when the engine is made.
    bool networkRuled() const {
        return isClientSide() || _context == ENTITY_SERVER_SCRIPT || (_context == AGENT_SCRIPT && !_agentScriptIsLocal);
    }
    /// The origin the rule judges by. A server's engine's scripts count as from the network, whatever their own
    /// address: a local one is no origin there. Elsewhere the origin itself.
    QUrl ruledOrigin(const QUrl& origin) const;

    QList<EntityItemID> getListOfEntityScriptIDs();

    /**jsdoc
     * Stops and unloads the current script.
     * <p><strong>Warning:</strong> If an assignment client script, the script gets restarted after stopping.</p>
     * @function Script.stop
     * @param {boolean} [marshal=false] - Marshal.
     *     <p class="important">Deprecated: This parameter is deprecated and will be removed.</p>
     * @example <caption>Stop a script after 5s.</caption>
     * Script.setInterval(function () {
     *     print("Hello");
     * }, 1000);
     *
     * Script.setTimeout(function () {
     *     Script.stop(true);
     * }, 5000);
     */
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // NOTE - this is intended to be a public interface for Agent scripts, and local scripts, but not for EntityScripts
    Q_INVOKABLE virtual void stop(bool marshal = false) = 0;

    // Stop any evaluating scripts and wait for the scripting thread to finish.
    virtual void waitTillDoneRunning() = 0;

    /// From another thread, the call a stopped engine is still in is made to end (a script stuck in a loop).
    virtual void interruptRunningCall() = 0;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // NOTE - these are NOT intended to be public interfaces available to scripts, the are only Q_INVOKABLE so we can
    //        properly ensure they are only called on the correct thread

    /// registers a global object by name (the host's, not a script's: a script could replace a global every script
    /// of a shared engine relies on; the thread hop inside needs no meta-method)
    virtual void registerGlobalObject(const QString& name, QObject* object) = 0;
    /// The application says so where it registers the Test API (a --testScript run): Script.dumpApi may then write
    /// to any path. Not a script's to call.
    void setTestApiRegistered() { _testApiOn = true; }

    /// A plain value (numbers by name, as DriveKeys and PickType are; a number; text) under a global name, unless
    /// the name is taken (the WebAssembly engine gives a module these through Script.scriptGlobalValues).
    virtual void registerGlobalValue(const QString& name, const QVariant& value) = 0;

    /**jsdoc
     * @function Script.registerGetterSetter
     * @param {string} name - Name.
     * @param {function} getter - Getter.
     * @param {function} setter - Setter.
     * @param {string} [parent=""] - Parent.
     * @deprecated This function is deprecated and will be removed.
     */
    /// registers a global getter/setter
    Q_INVOKABLE virtual void registerGetterSetter(const QString& name, QScriptEngine::FunctionSignature getter,
                                          QScriptEngine::FunctionSignature setter, const QString& parent = QString("")) = 0;

    /**jsdoc
     * @function Script.registerFunction
     * @param {string} name - Name.
     * @param {function} function - Function.
     * @param {number} [numArguments=-1] - Number of arguments.
     * @deprecated This function is deprecated and will be removed.
     */
    /// register a global function
    Q_INVOKABLE virtual void registerFunction(const QString& name, QScriptEngine::FunctionSignature fun, int numArguments = -1) = 0;

    /**jsdoc
     * @function Script.registerFunction
     * @param {string} parent - Parent.
     * @param {string} name - Name.
     * @param {function} function - Function.
     * @param {number} [numArguments=-1] - Number of arguments.
     * @deprecated This function is deprecated and will be removed.
     */
    /// register a function as a method on a previously registered global object
    Q_INVOKABLE virtual void registerFunction(const QString& parent, const QString& name, QScriptEngine::FunctionSignature fun,
                                      int numArguments = -1) = 0;

    /**jsdoc
     * @function Script.registerValue
     * @param {string} name - Name.
     * @param {object} value - Value.
     * @deprecated This function is deprecated and will be removed.
     */
    /// registers a global object by name
    Q_INVOKABLE virtual void registerValue(const QString& valueName, QScriptValue value) = 0;

    /// The scripting console's engine (Developer > Console) by name: it runs no script of its own, only the lines typed
    /// into it.
    static const QString CONSOLE_SCRIPT_NAME;
    /// A line typed into the scripting console, evaluated in this engine's global scope, on the engine's thread: what
    /// the console shows, { "text": the result as text, "error": whether it threw }. Not part of the Script API.
    virtual QVariantMap consoleEvaluate(const QString& code) = 0;

    /// if the script engine is not already running, this will download the URL and start the process of seting it up
    /// to run... NOTE - this is used by Application currently to load the url. We don't really want it to be exposed
    /// to scripts. we may not need this to be invokable
    virtual void loadURL(const QUrl& scriptURL, bool reload);
    virtual bool hasValidScriptSuffix(const QString& scriptFileName);

    /**jsdoc
     * Gets the context that the script is running in: Interface/avatar, client entity, server entity, or assignment client.
     * @function Script.getContext
     * @returns {string} The context that the script is running in:
     * <ul>
     *   <li><code>"client"</code>: An Interface or avatar script.</li>
     *   <li><code>"entity_client"</code>: A client entity script.</li>
     *   <li><code>"entity_server"</code>: A server entity script.</li>
     *   <li><code>"agent"</code>: An assignment client script.</li>
     * </ul>
     */
    Q_INVOKABLE QString getContext() const;

    /**jsdoc
     * Checks whether the script is running as an Interface or avatar script.
     * @function Script.isClientScript
     * @returns {boolean} <code>true</code> if the script is running as an Interface or avatar script, <code>false</code> if it
     *     isn't.
     */
    Q_INVOKABLE bool isClientScript() const { return _context == CLIENT_SCRIPT; }

    /**jsdoc
     * Checks whether the application was compiled as a debug build.
     * @function Script.isDebugMode
     * @returns {boolean} <code>true</code> if the application was compiled as a debug build, <code>false</code> if it was
     *     compiled as a release build.
     */
    Q_INVOKABLE bool isDebugMode() const;

    /**jsdoc
     * Checks whether the script is running as a client entity script.
     * @function Script.isEntityClientScript
     * @returns {boolean} <code>true</code> if the script is running as a client entity script, <code>false</code> if it isn't.
     */
    Q_INVOKABLE bool isEntityClientScript() const { return _context == ENTITY_CLIENT_SCRIPT; }

    /**jsdoc
     * Checks whether the script is running as a server entity script.
     * @function Script.isEntityServerScript
     * @returns {boolean} <code>true</code> if the script is running as a server entity script, <code>false</code> if it isn't.
     */
    Q_INVOKABLE bool isEntityServerScript() const { return _context == ENTITY_SERVER_SCRIPT; }

    /**jsdoc
     * Checks whether the script is running as an assignment client script.
     * @function Script.isAgentScript
     * @returns {boolean} <code>true</code> if the script is running as an assignment client script, <code>false</code> if it
     *     isn't.
     */
    Q_INVOKABLE bool isAgentScript() const { return _context == AGENT_SCRIPT; }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // NOTE - these are intended to be public interfaces available to scripts

    /**jsdoc
     * Starts running another script in Interface, if it isn't already running. The script is not automatically loaded next
     * time Interface starts.
     * <p class="availableIn"><strong>Supported Script Types:</strong> Interface Scripts &bull; Avatar Scripts</p>
     * <p>See also, {@link ScriptDiscoveryService.loadScript}.</p>
     * @function Script.load
     * @param {string} filename - The URL of the script to load. This can be relative to the current script's URL.
     * @example <caption>Load a script from another script.</caption>
     * // First file: scriptA.js
     * print("This is script A");
     *
     * // Second file: scriptB.js
     * print("This is script B");
     * Script.load("scriptA.js");
     *
     * // If you run scriptB.js you should see both scripts in the Running Scripts dialog.
     * // And you should see the following output:
     * // This is script B
     * // This is script A
     */
    Q_INVOKABLE void load(const QString& loadfile);

    /**jsdoc
     * @function Script.resetModuleCache
     * @param {boolean} [deleteScriptCache=false] - Delete script cache.
     * @deprecated This function is deprecated and will be removed.
     */
    // The JavaScript guest keeps the module cache (Script.require.cache, reset by its own Script.resetModuleCache);
    // the entities renderer still calls this on a reload of the entity scripts, where it has never reached the
    // guest: nothing to do here until that call goes.
    Q_INVOKABLE void resetModuleCache(bool deleteScriptCache = false) { Q_UNUSED(deleteScriptCache); }

    /**jsdoc
     * Prints a message to the program log and emits {@link Script.printedMessage}.
     * <p>Alternatively, you can use {@link print} or one of the {@link console} API methods.</p>
     * @function Script.print
     * @param {string} message - The message to print.
     */
    Q_INVOKABLE void print(const QString& message);

    /**jsdoc
     * Resolves a relative path to an absolute path. The relative path is relative to the script's location.
     * @function Script.resolvePath
     * @param {string} path - The relative path to resolve.
     * @returns {string} The absolute path.
     * @example <caption>Report the directory and filename of the running script.</caption>
     * print(Script.resolvePath(""));
     * @example <caption>Report the directory of the running script.</caption>
     * print(Script.resolvePath("."));
     * @example <caption>Report the path to a file located relative to the running script.</caption>
     * print(Script.resolvePath("../assets/sounds/hello.wav"));
     */
    Q_INVOKABLE QUrl resolvePath(const QString& path) const;

    /**jsdoc
     * Gets the path to the resources directory for QML files.
     * @function Script.resourcesPath
     * @returns {string} The path to the resources directory for QML files.
     */
    Q_INVOKABLE QUrl resourcesPath() const;

    /**jsdoc
     * Starts timing a section of code in order to send usage data about it to High Fidelity. Shouldn't be used outside of the
     * standard scripts.
     * @function Script.beginProfileRange
     * @param {string} label - A name that identifies the section of code.
     */
    Q_INVOKABLE void beginProfileRange(const QString& label) const;

    /**jsdoc
     * Finishes timing a section of code in order to send usage data about it to High Fidelity. Shouldn't be used outside of
     * the standard scripts.
     * @function Script.endProfileRange
     * @param {string} label - A name that identifies the section of code.
     */
    Q_INVOKABLE void endProfileRange(const QString& label) const;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Entity Script Related methods

    /**jsdoc
     * Checks whether an entity has an entity script running.
     * @function Script.isEntityScriptRunning
     * @param {Uuid} entityID - The ID of the entity.
     * @returns {boolean} <code>true</code> if the entity has an entity script running, <code>false</code> if it doesn't.
     */
    Q_INVOKABLE bool isEntityScriptRunning(const EntityItemID& entityID) {
        QReadLocker locker { &_entityScriptsLock };
        auto it = _entityScripts.constFind(entityID);
        return it != _entityScripts.constEnd() && it->status == EntityScriptStatus::RUNNING;
    }
    QVariant cloneEntityScriptDetails(const EntityItemID& entityID);
    QFuture<QVariant> getLocalEntityScriptDetails(const EntityItemID& entityID) override;

    /**jsdoc
     * @function Script.loadEntityScript
     * @param {Uuid} entityID - Entity ID.
     * @param {string} script - Script.
     * @param {boolean} forceRedownload - Force re-download.
     * @deprecated This function is deprecated and will be removed.
     */
    Q_INVOKABLE virtual void loadEntityScript(const EntityItemID& entityID, const QString& entityScript, bool forceRedownload) = 0;

    /**jsdoc
     * @function Script.unloadEntityScript
     * @param {Uuid} entityID - Entity ID.
     * @param {boolean} [shouldRemoveFromMap=false] - Should remove from map.
     * @deprecated This function is deprecated and will be removed.
     */
    Q_INVOKABLE virtual void unloadEntityScript(const EntityItemID& entityID, bool shouldRemoveFromMap = false) = 0; // will call unload method

    /**jsdoc
     * @function Script.unloadAllEntityScripts
     * @param {boolean} [blockingCall=false] - Wait for completion if call moved to another thread.
     * @deprecated This function is deprecated and will be removed.
     */
    Q_INVOKABLE virtual void unloadAllEntityScripts(bool blockingCall = false) = 0;

    /**jsdoc
     * Calls a method in an entity script.
     * @function Script.callEntityScriptMethod
     * @param {Uuid} entityID - The ID of the entity running the entity script.
     * @param {string} methodName - The name of the method to call.
     * @param {string[]} [parameters=[]] - The parameters to call the specified method with.
     * @param {Uuid} [remoteCallerID=Uuid.NULL] - An ID that identifies the caller.
     */
    Q_INVOKABLE virtual void callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName,
                                                    const QStringList& params = QStringList(),
                                                    const QUuid& remoteCallerID = QUuid()) override = 0;

    /**jsdoc
     * Calls a method in an entity script.
     * @function Script.callEntityScriptMethod
     * @param {Uuid} entityID - Entity ID.
     * @param {string} methodName - Method name.
     * @param {PointerEvent} event - Pointer event.
     * @deprecated This function is deprecated and will be removed.
     */
    Q_INVOKABLE virtual void callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName, const PointerEvent& event) = 0;

    /**jsdoc
     * Calls a method in an entity script.
     * @function Script.callEntityScriptMethod
     * @param {Uuid} entityID - Entity ID.
     * @param {string} methodName - Method name.
     * @param {Uuid} otherID - Other entity ID.
     * @param {Collision} collision - Collision.
     * @deprecated This function is deprecated and will be removed.
     */
    Q_INVOKABLE virtual void callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName, const EntityItemID& otherID, const Collision& collision) = 0;

    /**jsdoc
     * Manually runs the JavaScript garbage collector which reclaims memory by disposing of objects that are no longer
     * reachable.
     * @function Script.requestGarbageCollection
     */
    // The JavaScript guest collects its own garbage as it runs; the call is kept for the scripts that make it.
    Q_INVOKABLE void requestGarbageCollection() {}

    /**jsdoc
     * @function Script.generateUUID
     * @returns {Uuid} A new UUID.
     * @deprecated This function is deprecated and will be removed. Use {@link Uuid(0).generate|Uuid.generate} instead.
     */
    Q_INVOKABLE QUuid generateUUID() { return QUuid::createUuid(); }

    void setType(Type type) { _type = type; };
    Type getType() { return _type; };
    QString getTypeAsString() const;

    bool isFinished() const { return _isFinished; } // used by Application and ScriptWidget
    bool isRunning() const { return _isRunning; } // used by ScriptWidget

    // this is used by code in ScriptEngines.cpp during the "reload all" operation
    bool isStopping() const { return _isStopping; }
    /// The id of the world whose scripts a client's entities engine runs (ScriptCaller::world() on its thread); set by
    /// its maker before it runs. 0: none given (ScriptCaller::ANY_WORLD on its thread).
    void setWorldId(int worldId) { _worldId = worldId; }
    int getWorldId() const { return _worldId; }
    /// Whether the world this engine runs the scripts of was left (ScriptCaller::retireWorld): its scripts' calls are
    /// refused, and a wait of theirs (a synchronous request, Script.include, Script.require) ends, as when it stops.
    bool worldWasLeft() const;
    /// A world's entities engine (it has a world id) that is stopping or whose world was left: its scripts' requests
    /// end and their answers are dropped (a synchronous one stops waiting). The visitor's own engines: never.
    bool isWorldEnding() const { return _worldId != 0 && (isStopping() || worldWasLeft()); }

    void disconnectNonEssentialSignals();

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // These are currently used by Application to track if a script is user loaded or not. Consider finding a solution
    // inside of Application so that the ScriptEngine class is not polluted by this notion
    void setUserLoaded(bool isUserLoaded) { _isUserLoaded = isUserLoaded; }
    bool isUserLoaded() const { return _isUserLoaded; }

    void setQuitWhenFinished(const bool quitWhenFinished) { _quitWhenFinished = quitWhenFinished; }
    bool isQuitWhenFinished() const { return _quitWhenFinished; }

    void setEmitScriptUpdatesFunction(std::function<bool()> func) { _emitScriptUpdates = func; }

    void scriptErrorMessage(const QString& message);
    void scriptWarningMessage(const QString& message);
    void scriptInfoMessage(const QString& message);
    void scriptPrintedMessage(const QString& message);
    void clearDebugLogWindow();
    int getNumRunningEntityScripts() const;
    bool getEntityScriptDetails(const EntityItemID& entityID, EntityScriptDetails &details) const;
    bool hasEntityScriptDetails(const EntityItemID& entityID) const;

    void setScriptEngines(QSharedPointer<ScriptEngines>& scriptEngines) { _scriptEngines = scriptEngines; }

signals:

    /**jsdoc
     * @function Script.scriptLoaded
     * @param {string} filename - File name.
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    void scriptLoaded(const QString& scriptFilename);

    /**jsdoc
     * @function Script.errorLoadingScript
     * @param {string} filename - File name.
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    void errorLoadingScript(const QString& scriptFilename);

    /**jsdoc
     * Triggered frequently at a system-determined interval.
     * @function Script.update
     * @param {number} deltaTime - The time since the last update, in s.
     * @returns {Signal}
     * @example <caption>Report script update intervals.</caption>
     * Script.update.connect(function (deltaTime) {
     *     print("Update: " + deltaTime);
     * });
     */
    void update(float deltaTime);

    /**jsdoc
     * Triggered when the script is stopping.
     * @function Script.scriptEnding
     * @returns {Signal}
     * @example <caption>Report when a script is stopping.</caption>
     * print("Script started");
     *
     * Script.scriptEnding.connect(function () {
     *     print("Script ending");
     * });
     *
     * Script.setTimeout(function () {
     *     print("Stopping script");
     *     Script.stop();
     * }, 1000);
     */
    void scriptEnding();

    /**jsdoc
     * @function Script.finished
     * @param {string} filename - File name.
     * @param {object} engine - Engine.
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    void finished(const QString& fileNameString, ScriptEnginePointer);

    /**jsdoc
     * @function Script.cleanupMenuItem
     * @param {string} menuItem - Menu item.
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    void cleanupMenuItem(const QString& menuItemString);

    /**jsdoc
     * Triggered when the script prints a message to the program log via {@link  print}, {@link Script.print},
     * {@link console.log}, {@link console.debug}, {@link console.group}, {@link console.groupEnd}, {@link console.time}, or
     * {@link console.timeEnd}.
     * @function Script.printedMessage
     * @param {string} message - The message.
     * @param {string} scriptName - The name of the script that generated the message.
     * @returns {Signal}
     */
    void printedMessage(const QString& message, const QString& scriptName);

    /**jsdoc
     * Triggered when the script generates an error, {@link console.error} or {@link console.exception} is called, or
     * {@link console.assert} is called and fails.
     * @function Script.errorMessage
     * @param {string} message - The error message.
     * @param {string} scriptName - The name of the script that generated the error message.
     * @returns {Signal}
     */
    void errorMessage(const QString& message, const QString& scriptName);

    /**jsdoc
     * Triggered when the script generates a warning or {@link console.warn} is called.
     * @function Script.warningMessage
     * @param {string} message - The warning message.
     * @param {string} scriptName - The name of the script that generated the warning message.
     * @returns {Signal}
     */
    void warningMessage(const QString& message, const QString& scriptName);

    /**jsdoc
     * Triggered when the script generates an information message or {@link console.info} is called.
     * @function Script.infoMessage
     * @param {string} message - The information message.
     * @param {string} scriptName - The name of the script that generated the information message.
     * @returns {Signal}
     */
    void infoMessage(const QString& message, const QString& scriptName);

    /**jsdoc
     * Triggered when the running state of the script changes, e.g., from running to stopping.
     * @function Script.runningStateChanged
     * @returns {Signal}
     */
    void runningStateChanged();

    /**jsdoc
     * @function Script.clearDebugWindow
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    void clearDebugWindow();

    /**jsdoc
     * @function Script.loadScript
     * @param {string} scriptName - Script name.
     * @param {boolean} isUserLoaded - Is user loaded.
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    void loadScript(const QString& scriptName, bool isUserLoaded);

    /**jsdoc
     * @function Script.reloadScript
     * @param {string} scriptName - Script name.
     * @param {boolean} isUserLoaded - Is user loaded.
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    void reloadScript(const QString& scriptName, bool isUserLoaded);

    /**jsdoc
     * Triggered when the script has stopped.
     * @function Script.doneRunning
     * @returns {Signal}
     */
    void doneRunning();

    /**jsdoc
     * @function Script.entityScriptDetailsUpdated
     * @returns {Signal}
     * @deprecated This signal is deprecated and will be removed.
     */
    // Emitted when an entity script is added or removed, or when the status of an entity
    // script is updated (goes from RUNNING to ERROR_RUNNING_SCRIPT, for example)
    void entityScriptDetailsUpdated();

    /**jsdoc
     * Triggered when the script starts for the user. See also, {@link Entities.preload}.
     * <p class="availableIn"><strong>Supported Script Types:</strong> Client Entity Scripts &bull; Server Entity Scripts</p>
     * @function Script.entityScriptPreloadFinished
     * @param {Uuid} entityID - The ID of the entity that the script is running in.
     * @returns {Signal}
     * @example <caption>Get the ID of the entity that a client entity script is running in.</caption>
     * var entityScript = function () {
     *     this.entityID = Uuid.NULL;
     * };
     *
     * Script.entityScriptPreloadFinished.connect(function (entityID) {
     *     this.entityID = entityID;
     *     print("Entity ID: " + this.entityID);
     * });
     *
     * var entityID = Entities.addEntity({
     *     type: "Box",
     *     position: Vec3.sum(MyAvatar.position, Vec3.multiplyQbyV(MyAvatar.orientation, { x: 0, y: 0, z: -5 })),
     *     dimensions: { x: 0.5, y: 0.5, z: 0.5 },
     *     color: { red: 255, green: 0, blue: 0 },
     *     script: "(" + entityScript + ")",  // Could host the script on a Web server instead.
     *     lifetime: 300  // Delete after 5 minutes.
     * });
     */
    // Emitted when an entity script has finished running preload
    void entityScriptPreloadFinished(const EntityItemID& entityID);

protected:
    /// From another thread, the entity scripts unloaded on the engine's thread and waited for here, while this
    /// thread's objects keep answering the unload handlers' blocking calls (waitServingCalls). A blocking invocation
    /// let the two threads wait on each other: a seated avatar's sit script asks MyAvatar for its animation roles in
    /// unload, and Darkride never quit (qa, 2026-10-29). Gives up, with a warning, after MAX_ENTITY_UNLOAD_WAIT_MS.
    void waitForEntityScriptsToUnload();
    /// Waits on the calling thread until finished() holds, serving meanwhile the queued and blocking invocations of
    /// this thread's objects, and only those (no user input, no timers). False when milliseconds passed first.
    static bool waitServingCalls(const std::function<bool()>& finished, int milliseconds);
    /// Script.dumpApi's file: under the application's data folder (api-dumps/<the path's file name>), or the path
    /// as given when anyPath (a test run with the Test API on). Every script sees Script, a world's entity scripts
    /// among them, so a script does not choose where the file goes.
    QString dumpApiFile(const QString& requested, bool anyPath) const;
    /// Set where the Test API is registered (a --testScript run), by the host, never from a global a script can assign.
    bool _testApiOn { false };
    /// The globals every engine gives a script, through the register calls.
    void registerStandardGlobals();

    /**jsdoc
     * @function Script.executeOnScriptThread
     * @param {function} function - Function.
     * @param {ConnectionType} [type=2] - Connection type.
     * @deprecated This function is deprecated and will be removed.
     */
    Q_INVOKABLE void executeOnScriptThread(std::function<void()> function, const Qt::ConnectionType& type = Qt::QueuedConnection );

    void updateEntityScriptStatus(const EntityItemID& entityID, const EntityScriptStatus& status, const QString& errorInfo = QString());
    void setEntityScriptDetails(const EntityItemID& entityID, const EntityScriptDetails& details);

    Context _context;
    bool _agentScriptIsLocal { false };   // an agent's assigned script is the operator's own (networkRuled)
    Type _type;
    QString _scriptContents;
    std::atomic<bool> _isFinished { false };
    std::atomic<bool> _isRunning { false };
    std::atomic<bool> _isStopping { false };
    int _worldId { 0 };  // setWorldId
    bool _isInitialized { false };
    mutable QReadWriteLock _entityScriptsLock { QReadWriteLock::Recursive };
    QHash<EntityItemID, EntityScriptDetails> _entityScripts;
    mutable std::atomic<bool> _servedNonLocalWorld { false };   // entityScriptsMayBeLocal's one-way answer

    bool _isThreaded { false };
    qint64 _lastUpdate;

    QString _fileNameString;
    // the engine's own globals, its children (they print through their parent, and go with it to its thread)
    Quat* _quatLibrary { nullptr };
    Vec3* _vec3Library { nullptr };
    Mat4* _mat4Library { nullptr };
    ScriptUUID* _uuidLibrary { nullptr };
    ConsoleScriptingInterface* _consoleScriptingInterface { nullptr };
    std::atomic<bool> _isUserLoaded { false };
    bool _isReloading { false };

    std::atomic<bool> _quitWhenFinished;

    AssetScriptingInterface* _assetScriptingInterface;

    std::function<bool()> _emitScriptUpdates{ []() { return true; }  };

    QWeakPointer<ScriptEngines> _scriptEngines;
};

// scriptURL, when known, is the script's full URL: an engine chosen for the script in the Running Scripts window
// takes precedence over the setting.
ScriptEnginePointer scriptEngineFactory(ScriptEngine::Context context,
                                        const QString& scriptContents,
                                        const QString& fileNameString,
                                        const QUrl& scriptURL = QUrl());

#endif // hifi_ScriptEngine_h
