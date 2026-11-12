//
//  WasmScriptEngine.h
//  libraries/script-engine/src
//
//  The script engine that runs WebAssembly modules: the one engine. It derives from ScriptEngine, the part every
//  engine's hosts see (the Script API by reflection, the engine's context, file, thread, entity script records,
//  messages and rules): the application, the agent and the entity script server register their objects on a
//  ScriptEnginePointer with registerGlobalObject, and this engine records them in its object table. A module
//  reaches them through the host interface (libraries/wasm-scripting) by reflection. The QScriptEngine the base
//  still carries is never run; it goes with the hosts' last QtScript registrations (the Qt 6 move, stage 0).
//
//  What runs here: a module (a .wasm file, run as it is); a JavaScript file, inside the JavaScript guest
//  (tools/wasm-js, darkride-js.wasm: QuickJS-NG plus a shim that gives the script the objects it had on
//  QtScript); and, in an entities engine (the client's and the entity script server's), the entity scripts:
//  JavaScript ones inside the guest, .wasm ones each as an instance of its module, one per URL however many
//  entities run it, every instance in this engine's thread and object table. Every script loads here: the factory
//  (scriptEngineFactory) makes no other engine.
//
//  Lifecycle, as the QtScript engine's: loadURL fetches the script; scriptLoaded makes ScriptEngines run the
//  initializers (the registrations) and runInThread; run() on the engine's thread registers the standard
//  globals, instantiates the module, calls its script_start and loops at SCRIPT_FPS, emitting update and
//  processing events (signals, timers, entity loads) until stop(); then scriptEnding, script_stop, finished.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_WasmScriptEngine_h
#define hifi_WasmScriptEngine_h

#include <atomic>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QVariantMap>

#include <ValueCodec.h>
#include <WasmRuntime.h>

#include "ScriptEngine.h"

class AudioEffectOptionsFactory;
class HostInterface;
class HttpRequestFactory;
class ObjectTable;
class QTimer;
class ResourceStates;
class SignalRelay;
class WebSocketFactory;

class WasmScriptEngine : public ScriptEngine {
    Q_OBJECT
public:
    /// A module running in this engine: the script's own (the JavaScript guest for a .js file or an entities
    /// engine), or a .wasm entity script's. Each has its instance, its host interface and its codec (so that
    /// a callback knows its instance); all share the engine's object table, signal relay and thread.
    struct ModuleInstance {
        QString name;
        std::shared_ptr<WasmModule> module;
        std::unique_ptr<WasmInstance> instance;
        std::unique_ptr<ValueCodec> codec;
        std::shared_ptr<CallbackTarget> target;
        std::unique_ptr<HostInterface> host;
        bool running { false };
        ~ModuleInstance();
    };
    using ModuleInstancePointer = std::shared_ptr<ModuleInstance>;

    /// A .wasm name runs as a module; any other name is JavaScript for the guest (a file's text, or none for an
    /// entities engine). The contents, when given, are the script (an agent hands them over); otherwise
    /// loadURL fetches them.
    WasmScriptEngine(Context context, const QString& scriptContents, const QString& fileNameString);
    ~WasmScriptEngine() override;

    /// True for a name or URL ending in .wasm (what the factory dispatches on).
    static bool isWasmScript(const QString& fileNameOrUrl);
    /// True for a name or URL ending in .js.
    static bool isJavaScriptFile(const QString& fileNameOrUrl);
    /// The runtime every module of this process is compiled by.
    static std::shared_ptr<WasmRuntime> runtime();

    /// Where the JavaScript guest module is: $DARKRIDE_JS_WASM, else wasm/darkride-js.wasm under the
    /// resources beside the executable (Contents/Resources on macOS, resources/ elsewhere).
    static QString javascriptGuestPath();
    /// Whether the JavaScript guest module is there. Every build carries it (CopyJavaScriptGuest); without it no
    /// JavaScript script can run, which is logged once.
    static bool hasJavaScriptGuest();
    /// The JavaScript guest module, compiled once per process (the edit-filter engine instantiates it too).
    static std::shared_ptr<WasmModule> javascriptGuestModule(QString& error);

    bool runsJavaScript() const { return _runsJavaScript; }

    /// The module's bytes, when they do not come through loadURL (tests).
    void setModuleBytes(const QByteArray& bytes) { _moduleBytes = bytes; }
    void setLimits(const WasmInstance::Limits& limits) { _limits = limits; }
    const WasmInstance::Limits& limits() const { return _limits; }

    // The ScriptEngine hooks the hosts call.
    void registerGlobalObject(const QString& name, QObject* object) override;
    void registerAppObject(const QString& name, QObject* object) override;
    void registerGlobalValue(const QString& name, const QVariant& value) override;
    void loadURL(const QUrl& scriptURL, bool reload) override;
    bool hasValidScriptSuffix(const QString& scriptFileName) override;
    void run() override;
    void stop(bool marshal = false) override;
    void waitTillDoneRunning() override;
    void interruptRunningCall() override;

    // The entity-script contract, for an entities engine: a JavaScript entity script runs in the guest, a
    // .wasm one in an instance of its module (one per URL), reached through the module's entity_* exports.
    void loadEntityScript(const EntityItemID& entityID, const QString& entityScript, bool forceRedownload) override;
    /// A console line, through the JavaScript guest's console_eval (revision 6 on): evaluated in the global scope of
    /// the engine's script. A module, or an older guest, says it has no console.
    QVariantMap consoleEvaluate(const QString& code) override;
    void unloadEntityScript(const EntityItemID& entityID, bool shouldRemoveFromMap = false) override;
    void unloadAllEntityScripts(bool blockingCall = false) override;
    void callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName,
                                const QStringList& params = QStringList(), const QUuid& remoteCallerID = QUuid()) override;
    void callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName, const PointerEvent& event) override;
    void callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName, const EntityItemID& otherID,
                                const Collision& collision) override;

    // Script.addEventHandler and removeEventHandler for a module: a guest function as the handler.
    Q_INVOKABLE void addEventHandler(const EntityItemID& entityID, const QString& eventName, const ScriptCallback& handler);
    Q_INVOKABLE void removeEventHandler(const EntityItemID& entityID, const QString& eventName, const ScriptCallback& handler);

    // The Script API a module uses for timers: a guest function as the callback, a timer id back.
    Q_INVOKABLE int setTimeout(const ScriptCallback& callback, int timeoutMS);
    Q_INVOKABLE int setInterval(const ScriptCallback& callback, int intervalMS);
    Q_INVOKABLE void clearTimeout(int timer);
    Q_INVOKABLE void clearInterval(int timer);

    // The Script API the JavaScript shim builds its include, require and resolvePath on (a native module may
    // use them too). Paths are resolved against the parent URL the caller tracks (the file whose code runs),
    // and an entity script's sandbox URL bounds what a local file may include.
    /// The script itself: {"fileName", "source"}.
    Q_INVOKABLE QVariantMap scriptSource() const;
    /// The plain values the hosts register (registerGlobalValue: TREE_SCALE, DriveKeys, PickType): name to value.
    Q_INVOKABLE QVariantMap scriptGlobalValues() const;
    /// Script.resolvePath with the parent given.
    Q_INVOKABLE QString resolvePathFrom(const QString& path, const QString& parentUrl) const;
    /// Script.include's rules: {"url"} or {"reason"} when the file is not to be included.
    Q_INVOKABLE QVariantMap resolveInclude(const QString& file, const QString& parentUrl, const QString& sandboxUrl = QString());
    /// Script.require.resolve's rules: {"url"} or {"error", "type"}.
    Q_INVOKABLE QVariantMap resolveModule(const QString& moduleId, const QString& relativeTo, const QString& parentUrl,
                                          const QString& sandboxUrl = QString());
    /// A script's text through the script cache, synchronously: {"url", "status", "success", "contents"}, for
    /// Script.include and Script.require; sandboxUrl is the guest's claim of the entity script running (verified).
    /// A file of this machine is served by Script.include's rule (the app's own scripts, a local script's files, a
    /// file under the entity script's own folder); the rest by the network rule for a script's own requests.
    Q_INVOKABLE QVariantMap fetchScript(const QString& url, bool forceDownload, const QString& sandboxUrl = QString());
    QUrl verifiedScriptOrigin(const QString& claimed) const override;
    /// The guest's claim of the entity script running, checked against the entity scripts this engine loaded: a
    /// claim it did not load counts as the entities engine itself (about:Entities, never local). Empty for a client
    /// script or an agent, which have no entity scripts and whose code is the engine's own.
    QUrl verifiedSandbox(const QString& claimed) const;
    /// Script.dumpApi for this engine: the globals record the guest composed, written under the usual root.
    Q_INVOKABLE bool dumpApi(const QString& path, const QVariantMap& globals);

    /// What the engine has done: host calls, bytes crossed, frames, the modules' memory.
    Q_INVOKABLE QVariantMap statistics() const;

    /// The names of the globals registered so far (the hosts' and the engine's own), in no order. For tests.
    QStringList registeredGlobalNames() const;
    /// The class of the object registered under a global's name (a stand-in's, RefusedGlobal, when the script API
    /// policy refused it), or empty for none. For tests.
    QString registeredGlobalClass(const QString& name) const;

    /// A guest function called with arguments, on this thread, for its result (what a ScriptCallback's call
    /// comes to); postCallback queues one without a result, from any thread.
    bool invokeCallback(const ModuleInstancePointer& instance, int32_t callback, const QVariantList& arguments,
                        QVariant& result, QString& error);
    void postCallback(const ModuleInstancePointer& instance, int32_t callback, const QVariantList& arguments);

protected:
    /// The globals the engine itself gives every script of its context, beside the standard ones and the hosts':
    /// Http and Sockets (what the guest's XMLHttpRequest and WebSocket are built on), WebSocketServers for an agent
    /// alone, AudioEffectOptions, Resource and TREE_SCALE. run() calls it on the engine's thread.
    void registerEngineGlobals();

private:
    bool fetchModuleBytes();
    ModuleInstancePointer makeInstance(const QString& name, const std::shared_ptr<WasmModule>& module,
                                       const WasmInstance::Limits& limits, QString& error);
    bool startMain();
    void stopInstance(const ModuleInstancePointer& instance);
    int endingTimeLeftMs() const;  // of the scripts' ending deadline, counted from the first stop
    void stopInstances();          // stop()'s part for the modules; the caller holds _instancesMutex
    void handOver(const ModuleInstancePointer& instance);  // off the starting list; the caller holds _instancesMutex
    void stopAll();
    /// For every module's instance; the caller holds _instancesMutex.
    void forEachInstance(const std::function<void(WasmInstance&)>& action);
    void loop();
    void onModuleFailure(const ModuleInstancePointer& instance, const QString& entry, const QString& error);
    /// A call into the JavaScript guest ended at the time limit (HostInterface::TimeLimitHandler): the entity script
    /// that was running is stopped and the others run on; when the script itself was running, it is stopped as a
    /// failure stops it.
    void onCallTimeLimit(const ModuleInstancePointer& instance, const QString& entry, const QString& entity, const QString& reason);
    /// Stops an entity script that misbehaved: its status an error with the reason, its object, timers, signal
    /// handlers and callbacks dropped (entity_unload, queued), its unload method not called.
    void stopEntityScript(const EntityItemID& entityID, const QString& errorInfo);
    int addTimer(const ScriptCallback& callback, int intervalMS, bool singleShot);
    void removeTimer(int id);
    void stopTimers();

    // entity scripts
    void entityScriptReady(const EntityItemID& entityID, const QString& url, const QVariant& sourceOrBytes, bool success,
                           const QString& status);
    ModuleInstancePointer entityModuleInstance(const QString& url, const QByteArray& bytes, QString& error);
    ModuleInstancePointer instanceForEntity(const EntityItemID& entityID) const;
    void callEntityMethod(const EntityItemID& entityID, const QString& methodName, const QVariantList& arguments,
                          const QUuid& remoteCallerID = QUuid());
    void connectEntityEventSignals();
    void forwardEntityEvent(const EntityItemID& entityID, const QString& eventName, const QVariantList& arguments);

    bool _runsJavaScript { false };
    bool _isEntitiesEngine { false };
    QByteArray _moduleBytes;
    WasmInstance::Limits _limits;
    std::unique_ptr<ObjectTable> _objects;
    QVariantMap _globalValues;  // registerGlobalValue's
    QSet<const QMetaObject*> _appClasses;  // registerAppObject's: past the script API policy
    QList<QPair<QString, QPointer<QObject>>> _appObjects;  // registerAppObject's, put in after every other global
    void installAppObjects();
    SignalRelay* _relay { nullptr };  // a child, so it moves to the engine's thread with it
    HttpRequestFactory* _http { nullptr };
    WebSocketFactory* _sockets { nullptr };
    AudioEffectOptionsFactory* _audioEffectOptions { nullptr };
    ResourceStates* _resourceStates { nullptr };
    std::mutex _instancesMutex;  // guards the set of instances against requestStop from another thread, and the next three
    bool _scriptEndingBegun { false };     // stopAll has begun: a stop leaves the handlers to the deadline
    bool _endingDeadlinePassed { false };  // interruptRunningCall: nothing more of the script runs, its handlers included
    bool _stopLetCallFinish { false };     // stop() found a call running and let it finish, to its deadline
    bool _startEndedByStop { false };      // a stop ended a module while it started (makeInstance)
    std::vector<ModuleInstancePointer> _starting;  // the instances makeInstance is starting, until handed over
    ModuleInstancePointer _main;
    QHash<QString, ModuleInstancePointer> _entityModules;              // .wasm entity scripts by URL
    QHash<EntityItemID, std::weak_ptr<ModuleInstance>> _entityHosts;  // which instance runs each entity's script
    // the entity scripts the time limit stopped, with the script each ran: not started again for the same script
    // unless a reload is forced (the entities engines load a script again whenever its entity changes and the script
    // is not running, which would start a runaway again and again)
    QHash<EntityItemID, QString> _stoppedByTimeLimit;
    std::atomic<int> _callDepth { 0 };
    std::atomic<quint64> _firstStopTime { 0 };  // usecTimestampNow() at the first stop; the handlers' time runs from it

    struct Timer {
        QTimer* timer;
        ScriptCallback callback;
        bool singleShot;
    };
    QHash<int, Timer> _timers;
    int _nextTimer { 1 };

    QHash<EntityItemID, QHash<QString, QList<ScriptCallback>>> _callbackHandlers;
    bool _entityEventSignalsConnected { false };

    uint64_t _frames { 0 };
    double _updateMicroseconds { 0.0 };
};

/// `new AudioEffectOptions(properties)` on the WebAssembly engine. The QtScript engine gives a native constructor,
/// which a module cannot call; the engine registers this factory under the name instead (the JavaScript guest
/// wraps it back into a constructor). The options it makes live as long as the engine.
class AudioEffectOptionsFactory : public QObject {
    Q_OBJECT
public:
    explicit AudioEffectOptionsFactory(QObject* parent = nullptr);

    /// Options with the defaults, then each known option given as a number (bandwidth, preDelay, reverbTime...).
    Q_INVOKABLE QObject* create(const QVariantMap& properties);
};

/// The `Resource` global on the WebAssembly engine: `Resource.State`, the loading states of a resource by name,
/// which the QtScript engine gives as a plain object.
class ResourceStates : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap State READ getState CONSTANT)
public:
    explicit ResourceStates(QObject* parent = nullptr) : QObject(parent) {}

    QVariantMap getState() const;
};

// --- The API reference's notes for what the JavaScript guest's shim (tools/wasm-js/runtime.js) and this engine give
// a script beyond the Script members declared in ScriptEngine.h ---

/**jsdoc
 * Prints a message to the program log and emits {@link Script.printedMessage}.
 * The message logged is the message values separated by spaces.
 * <p>Alternatively, you can use {@link Script.print} or one of the {@link console} API methods.</p>
 * @function print
 * @param {...*} [message] - The message values to print.
 */

/**jsdoc
 * Decodes base64 to text.
 * @function atob
 * @param {string} base64 - The base64 you want to decode.
 * @returns {string} text
 */

/**jsdoc
 * Encodes text to base64.
 * @function btoa
 * @param {string} text - The text you want to encode.
 * @returns {string} base64
 */

/**jsdoc
 * The <code>Resource</code> API provides values that define the possible loading states of a resource.
 *
 * @namespace Resource
 *
 * @hifi-interface
 * @hifi-client-entity
 * @hifi-avatar
 * @hifi-server-entity
 * @hifi-assignment-client
 *
 * @property {Resource.State} State - The possible loading states of a resource. <em>Read-only.</em>
 */

/**jsdoc
 * Includes JavaScript from other files in the current script. If a callback is specified, the files are loaded and
 * included asynchronously, otherwise they are included synchronously (i.e., script execution blocks while the files are
 * included).
 * @function Script.include
 * @variation 0
 * @param {string[]} filenames - The URLs of the scripts to include. Each can be relative to the current script.
 * @param {function} [callback=null] - The function to call back when the scripts have been included. It can be either the
 *     name of a function or an in-line definition.
 */

/**jsdoc
 * Includes JavaScript from another file in the current script. If a callback is specified, the file is loaded and included
 * asynchronously, otherwise it is included synchronously (i.e., script execution blocks while the file is included).
 * @function Script.include
 * @param {string} filename - The URL of the script to include. It can be relative to the current script.
 * @param {function} [callback=null] - The function to call back when the script has been included. It can be either the
 *     name of a function or an in-line definition.
 * @example <caption>Include a script file asynchronously.</caption>
 * // First file: scriptA.js
 * print("This is script A");
 *
 * // Second file: scriptB.js
 * print("This is script B");
 * Script.include("scriptA.js", function () {
 *     print("Script A has been included");
 * });
 *
 * // If you run scriptB.js you should see only scriptB.js in the running scripts list.
 * // And you should see the following output:
 * // This is script B
 * // This is script A
 * // Script A has been included
 */

/**jsdoc
 * Provides access to methods or objects provided in an external JavaScript or JSON file.
 * See {@link https://legacydocs.tivolicloud.com/script/js-tips.html} for further details.
 * @function Script.require
 * @param {string} module - The module to use. May be a JavaScript file, a JSON file, or the name of a system module such
 *     as <code>"appUi"</code> (i.e., the "appUi.js" system module JavaScript file).
 * @returns {object|array} The value assigned to <code>module.exports</code> in the JavaScript file, or the value defined
 *     in the JSON file.
 */

/**jsdoc
 * @function Script.resetModuleCache
 * @param {boolean} [deleteScriptCache=false] - Delete script cache.
 * @deprecated This function is deprecated and will be removed.
 */

/**jsdoc
 * Calls a function repeatedly, at a set interval.
 * @function Script.setInterval
 * @param {function} function - The function to call. This can be either the name of a function or an in-line definition.
 * @param {number} interval - The interval at which to call the function, in ms.
 * @returns {object} A handle to the interval timer. This can be used in {@link Script.clearInterval}.
 * @example <caption>Print a message every second.</caption>
 * Script.setInterval(function () {
 *     print("Interval timer fired");
 * }, 1000);
 */

/**jsdoc
 * Calls a function once, after a delay.
 * @function Script.setTimeout
 * @param {function} function - The function to call. This can be either the name of a function or an in-line definition.
 * @param {number} timeout - The delay after which to call the function, in ms.
 * @returns {object} A handle to the timeout timer. This can be used in {@link Script.clearTimeout}.
 * @example <caption>Print a message once, after a second.</caption>
 * Script.setTimeout(function () {
 *     print("Timeout timer fired");
 * }, 1000);
 */

/**jsdoc
 * Stops an interval timer set by {@link Script.setInterval|setInterval}.
 * @function Script.clearInterval
 * @param {object} timer - The interval timer to stop.
 * @example <caption>Stop an interval timer.</caption>
 * // Print a message every second.
 * var timer = Script.setInterval(function () {
 *     print("Interval timer fired");
 * }, 1000);
 *
 * // Stop the timer after 10 seconds.
 * Script.setTimeout(function () {
 *     print("Stop interval timer");
 *     Script.clearInterval(timer);
 * }, 10000);
 */

/**jsdoc
 * Stops a timeout timer set by {@link Script.setTimeout|setTimeout}.
 * @function Script.clearTimeout
 * @param {object} timer - The timeout timer to stop.
 * @example <caption>Stop a timeout timer.</caption>
 * // Print a message after two seconds.
 * var timer = Script.setTimeout(function () {
 *     print("Timer fired");
 * }, 2000);
 *
 * // Uncomment the following line to stop the timer from firing.
 * //Script.clearTimeout(timer);
 */

/**jsdoc
 * Adds a function to the list of functions called when a particular event occurs on a particular entity.
 * <p>See also, the {@link Entities} API.</p>
 * @function Script.addEventHandler
 * @param {Uuid} entityID - The ID of the entity.
 * @param {Script.EntityEvent} eventName - The name of the event.
 * @param {Script~entityEventCallback|Script~pointerEventCallback|Script~collisionEventCallback} handler - The function to
 *     call when the event occurs on the entity. It can be either the name of a function or an in-line definition.
 * @example <caption>Report when a mouse press occurs on a particular entity.</caption>
 * var entityID = Entities.addEntity({
 *     type: "Box",
 *     position: Vec3.sum(MyAvatar.position, Vec3.multiplyQbyV(MyAvatar.orientation, { x: 0, y: 0, z: -5 })),
 *     dimensions: { x: 0.5, y: 0.5, z: 0.5 },
 *     lifetime: 300  // Delete after 5 minutes.
 * });
 *
 * function reportMousePress(entityID, event) {
 *     print("Mouse pressed on entity: " + JSON.stringify(event));
 * }
 *
 * Script.addEventHandler(entityID, "mousePressOnEntity", reportMousePress);
 */

/**jsdoc
 * Removes a function from the list of functions called when an entity event occurs on a particular entity.
 * <p>See also, the {@link Entities} API.</p>
 * @function Script.removeEventHandler
 * @param {Uuid} entityID - The ID of the entity.
 * @param {Script.EntityEvent} eventName - The name of the entity event.
 * @param {function} handler - The name of the function to no longer call when the entity event occurs on the entity.
 */

/**jsdoc
 * <p>The name of an entity event. When the entity event occurs, any function that has been registered for that event
 * via {@link Script.addEventHandler} is called with parameters per the entity event.</p>
 * <table>
 *   <thead>
 *     <tr><th>Event Name</th><th>Callback Type</th><th>Entity Event</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>"enterEntity"</code></td><td>{@link Script~entityEventCallback|entityEventCallback}</td>
 *       <td>{@link Entities.enterEntity}</td></tr>
 *     <tr><td><code>"leaveEntity"</code></td><td>{@link Script~entityEventCallback|entityEventCallback}</td>
 *       <td>{@link Entities.leaveEntity}</td></tr>
 *     <tr><td><code>"mousePressOnEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.mousePressOnEntity}</td></tr>
 *     <tr><td><code>"mouseMoveOnEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.mouseMoveOnEntity}</td></tr>
 *     <tr><td><code>"mouseReleaseOnEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.mouseReleaseOnEntity}</td></tr>
 *     <tr><td><code>"clickDownOnEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.clickDownOnEntity}</td></tr>
 *     <tr><td><code>"holdingClickOnEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.holdingClickOnEntity}</td></tr>
 *     <tr><td><code>"clickReleaseOnEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.clickReleaseOnEntity}</td></tr>
 *     <tr><td><code>"hoverEnterEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.hoverEnterEntity}</td></tr>
 *     <tr><td><code>"hoverOverEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.hoverOverEntity}</td></tr>
 *     <tr><td><code>"hoverLeaveEntity"</code></td><td>{@link Script~pointerEventCallback|pointerEventCallback}</td>
 *       <td>{@link Entities.hoverLeaveEntity}</td></tr>
 *     <tr><td><code>"collisionWithEntity"</code><td>{@link Script~collisionEventCallback|collisionEventCallback}</td>
 *       </td><td>{@link Entities.collisionWithEntity}</td></tr>
 *   </tbody>
 * </table>
 * @typedef {string} Script.EntityEvent
 */

/**jsdoc
 * Called when an entity event occurs on an entity as registered with {@link Script.addEventHandler}.
 * @callback Script~entityEventCallback
 * @param {Uuid} entityID - The ID of the entity the event has occured on.
 */

/**jsdoc
 * Called when a pointer event occurs on an entity as registered with {@link Script.addEventHandler}.
 * @callback Script~pointerEventCallback
 * @param {Uuid} entityID - The ID of the entity the event has occurred on.
 * @param {PointerEvent} pointerEvent - Details of the event.
 */

/**jsdoc
 * Called when a collision event occurs on an entity as registered with {@link Script.addEventHandler}.
 * @callback Script~collisionEventCallback
 * @param {Uuid} entityA - The ID of one entity in the collision.
 * @param {Uuid} entityB - The ID of the other entity in the collision.
 * @param {Collision} collisionEvent - Details of the collision.
 */

/**jsdoc
 * Triggered when the script starts for a user. See also, {@link Script.entityScriptPreloadFinished}.
 * <p>Note: Can only be connected to via <code>this.preload = function (...) { ... }</code> in the entity script.</p>
 * <p class="availableIn"><strong>Supported Script Types:</strong> Client Entity Scripts &bull; Server Entity Scripts</p>
 * @function Entities.preload
 * @param {Uuid} entityID - The ID of the entity that the script is running in.
 * @returns {Signal}
 * @example <caption>Get the ID of the entity that a client entity script is running in.</caption>
 * var entityScript = (function () {
 *     this.entityID = Uuid.NULL;
 *
 *     this.preload = function (entityID) {
 *         this.entityID = entityID;
 *         print("Entity ID: " + this.entityID);
 *     };
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

/**jsdoc
 * Triggered when the script terminates for a user.
 * <p>Note: Can only be connected to via <code>this.unload = function () { ... }</code> in the entity script.</p>
 * <p class="availableIn"><strong>Supported Script Types:</strong> Client Entity Scripts &bull; Server Entity Scripts</p>
 * @function Entities.unload
 * @param {Uuid} entityID - The ID of the entity that the script is running in.
 * @returns {Signal}
 */

/**jsdoc
 * @function Script.evaluate
 * @param {string} program - Program.
 * @param {string} filename - File name.
 * @param {number} [lineNumber=-1] - Line number.
 * @returns {object} Object.
 * @deprecated This function is deprecated and will be removed.
 */

/**jsdoc
 * Writes a description of every global object this script sees to a JSON file: for an object from the engine, its
 * class and the methods, properties, signals and enums of its meta-object, with parameter and return types; for
 * anything else, its kind and members. A developer tool: the file is the machine-readable form of the API in this
 * script's context. The file goes under the application's own data folder (<code>api-dumps/</code>, the path's
 * file name only); a test run with the Test API on may name any path.
 * @function Script.dumpApi
 * @param {string} path - The file to write.
 * @returns {boolean} <code>true</code> if the file was written.
 */

#endif // hifi_WasmScriptEngine_h
