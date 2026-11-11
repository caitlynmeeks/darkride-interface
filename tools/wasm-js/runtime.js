//
//  runtime.js
//  tools/wasm-js
//
//  The shim inside darkride-js.wasm: what makes a JavaScript script see the engine's objects as it did on
//  QtScript. Evaluated once per script by darkride-js.c, which hands it the native host object (the ten
//  imports of the "darkride" module plus a few helpers) and takes back the hooks it calls.
//
//  What it builds, from the host's descriptions: a JavaScript object per registered global, lazily on first
//  use, whose prototype (one per C++ class) has a method per invokable, an accessor per property, and a
//  signal object per signal with connect and disconnect; Script's own API (timers, include, require,
//  resolvePath, print, the module cache); console, print, request; XMLHttpRequest and WebSocket over the
//  engine's Http and Sockets objects; the plain-object globals the hosts put on the global object (DriveKeys,
//  PickType). Functions passed to the host cross as callback ids and come back through onCallback.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

(function (host) {
    "use strict";

    var LOG_PRINT = 0, LOG_INFO = 1, LOG_WARNING = 2, LOG_ERROR = 3, LOG_DEBUG = 4;
    var global = globalThis;

    // --- callbacks: functions the host holds by id ---------------------------------------------------------
    var callbacks = new Map();      // id -> function
    var callbackIds = new WeakMap(); // function -> id (the same function keeps its id)
    var callbackOwners = new Map();  // id -> { entity, url } for a callback an entity script registered
    var nextCallbackId = 1;

    function registerCallback(fn) {
        var id = callbackIds.get(fn);
        if (id === undefined) {
            id = nextCallbackId++;
            callbacks.set(id, fn);
            callbackIds.set(fn, id);
            if (currentEntity) {
                callbackOwners.set(id, { entity: currentEntity, url: currentSandbox });
            }
        }
        return id;
    }

    function unregisterCallback(id) {
        var fn = callbacks.get(id);
        if (fn !== undefined) {
            callbacks.delete(id);
            callbackIds.delete(fn);
            callbackOwners.delete(id);
        }
    }

    function onCallback(id, args) {
        var fn = callbacks.get(id);
        if (fn !== undefined) {
            var owner = callbackOwners.get(id);
            if (owner !== undefined) {
                return withEntity(owner.entity, owner.url, function () { return fn.apply(undefined, args); });
            }
            return fn.apply(undefined, args);
        }
        return undefined;
    }

    // --- errors in handlers are reported, not fatal ------------------------------------------------------------
    function describeError(e) {
        if (e instanceof Error) {
            return (e.stack && e.stack.indexOf(e.message) >= 0) ? e.stack : (e.name + ": " + e.message + "\n" + (e.stack || ""));
        }
        return String(e);
    }

    function reportError(what, e) {
        host.log(LOG_ERROR, what + ": " + describeError(e));
    }

    // --- signals -----------------------------------------------------------------------------------------------
    var connections = new Map(); // connection id -> signal record

    function makeSignal(object, name) {
        var record = { object: object, name: name, handlers: [], connection: 0 };
        // a function, as a QtScript signal was (typeof === "function"); calling it does nothing
        var signal = function () {};
        signal.connect = function (a, b) {
                var thisObject, fn;
                if (typeof a === "function") { thisObject = undefined; fn = a; }
                else if (typeof b === "function") { thisObject = a; fn = b; }
                else if (typeof b === "string" && a && typeof a[b] === "function") { thisObject = a; fn = a[b]; }
                else { throw new TypeError(name + ".connect: a function is needed"); }
                record.handlers.push({ thisObject: thisObject, fn: fn, entity: currentEntity, url: currentSandbox });
                if (!record.connection) {
                    record.connection = host.connect(object, name);
                    if (record.connection) {
                        connections.set(record.connection, record);
                    } else {
                        host.log(LOG_WARNING, "cannot connect " + name);
                    }
                }
            };
        signal.disconnect = function (a, b) {
                var fn = typeof a === "function" ? a : (typeof b === "function" ? b : (typeof b === "string" && a ? a[b] : undefined));
                var handlers = record.handlers;
                for (var i = 0; i < handlers.length; i++) {
                    if (handlers[i].fn === fn) {
                        handlers.splice(i, 1);
                        break;
                    }
                }
                if (handlers.length === 0 && record.connection) {
                    connections.delete(record.connection);
                    host.disconnect(record.connection);
                    record.connection = 0;
                }
            };
        signal.toString = function () { return "[signal " + name + "]"; };
        return signal;
    }

    function onSignal(connection, args) {
        var record = connections.get(connection);
        if (!record) {
            return;
        }
        var handlers = record.handlers.slice();
        for (var i = 0; i < handlers.length; i++) {
            var handler = handlers[i];
            try {
                if (handler.entity) {
                    withEntity(handler.entity, handler.url, function () { handler.fn.apply(handler.thisObject, args); });
                } else {
                    handler.fn.apply(handler.thisObject, args);
                }
            } catch (e) {
                reportError("in a handler of " + record.name, e);
            }
        }
    }

    // An entity script's signal handlers go when it does
    function disconnectHandlersOf(entity) {
        connections.forEach(function (record) {
            var kept = record.handlers.filter(function (handler) { return handler.entity !== entity; });
            if (kept.length === record.handlers.length) {
                return;
            }
            record.handlers = kept;
            if (kept.length === 0 && record.connection) {
                connections.delete(record.connection);
                host.disconnect(record.connection);
                record.connection = 0;
            }
        });
    }

    function unregisterCallbacksOf(entity) {
        var ids = [];
        callbackOwners.forEach(function (owner, id) { if (owner.entity === entity) { ids.push(id); } });
        ids.forEach(unregisterCallback);
    }

    // --- host objects: one JavaScript object per handle, one prototype per class --------------------------------
    // A registered global's object is kept for the script's life. Any other object (one a method returned or a
    // signal carried) is held weakly: once the script no longer reaches it and it is collected, its handle is
    // released, so the host forgets it (and deletes it, when the object was made for the script alone). An
    // object the host hands over again before the release runs gets a new wrapper, which takes the handle over.
    var globalObjects = new Map();      // handle -> object
    var objectsByHandle = new Map();    // handle -> WeakRef to the object
    var releases = new FinalizationRegistry(function (handle) {
        var ref = objectsByHandle.get(handle);
        if (ref !== undefined) {
            if (ref.deref() !== undefined) {
                return; // a newer wrapper holds the handle
            }
            objectsByHandle.delete(handle);
        }
        host.release(handle);
    });
    var prototypesByClass = new Map();
    var warnedReadOnly = new Set();

    function makeMethod(name) {
        return host.method(name);
    }

    // One prototype per class carries the methods and toString; each wrapper gets its properties and signals
    // as own accessors, and its methods as own values too, so that Object.keys, for in and JSON.stringify see
    // what they saw on a QtScript wrapper (every member was an own property there).
    function prototypeFor(className, description) {
        var proto = prototypesByClass.get(className);
        if (proto) {
            return proto;
        }
        proto = {};
        // as QtScript printed a QObject: its class and its objectName, "Vec3(name = \"\")"
        Object.defineProperty(proto, "toString", { value: function () {
            var name = "";
            try { name = host.get(this, "objectName"); } catch (e) { name = ""; }
            return className + "(name = \"" + (name === undefined || name === null ? "" : String(name)) + "\")";
        }, writable: true, configurable: true });
        var methods = description.methods || [];
        var seen = new Set();
        for (var i = 0; i < methods.length; i++) {
            var member = methods[i];
            if (seen.has(member.name)) { continue; } // overloads: one function, the host picks the overload
            seen.add(member.name);
            Object.defineProperty(proto, member.name, { value: makeMethod(member.name), writable: true, configurable: true });
        }
        prototypesByClass.set(className, proto);
        return proto;
    }

    function defineMembers(object, className, description) {
        var i, member;
        var seen = new Set();
        var methods = description.methods || [];
        for (i = 0; i < methods.length; i++) {
            member = methods[i];
            if (seen.has(member.name)) { continue; }
            seen.add(member.name);
            Object.defineProperty(object, member.name, { value: Object.getPrototypeOf(object)[member.name], writable: true, enumerable: true, configurable: true });
        }
        var signals = description.signals || [];
        for (i = 0; i < signals.length; i++) {
            member = signals[i];
            if (seen.has(member.name)) { continue; }
            seen.add(member.name);
            (function (name) {
                Object.defineProperty(object, name, {
                    get: function () {
                        var signal = makeSignal(this, name);
                        Object.defineProperty(this, name, { value: signal, writable: true, enumerable: true, configurable: true });
                        return signal;
                    },
                    enumerable: true,
                    configurable: true
                });
            }(member.name));
        }
        var properties = description.properties || [];
        for (i = 0; i < properties.length; i++) {
            member = properties[i];
            (function (name, writable) {
                Object.defineProperty(object, name, {
                    get: host.getter(name),
                    set: writable ? host.setter(name) : function () {
                        var key = className + "." + name;
                        if (!warnedReadOnly.has(key)) {
                            warnedReadOnly.add(key);
                            host.log(LOG_WARNING, key + " is read-only; the assignment is ignored");
                        }
                    },
                    enumerable: true,
                    configurable: true
                });
            }(member.name, member.writable));
        }
    }

    // Methods whose QtScript form takes a QScriptValue have a twin under another name that takes a plain value;
    // the script keeps calling the name it knows.
    var methodTwins = {
        "controller::MappingBuilderProxy": { from: "fromValue", makeAxis: "makeAxisFromValues" },
        "controller::RouteBuilderProxy": { to: "toValue", when: "whenValue" },
        "MyAvatar": { addAnimationStateHandler: "addAnimationStateCallback", removeAnimationStateHandler: "removeAnimationStateCallback" },
        "HMDScriptingInterface": { getHUDLookAtPosition2D: "getHUDLookAtPosition2DValue", getHUDLookAtPosition3D: "getHUDLookAtPosition3DValue" }
    };

    // A callback as QtScript's makeScopedHandlerObject takes one: a function; a scope and a function; a scope and the
    // name of one of its methods; or (with no second argument) a {scope, callback} record. Called with the scope as this.
    function scopedCallback(scopeOrCallback, methodOrName, hasSecond) {
        var scope, callback = scopeOrCallback;
        if (scopeOrCallback !== null && (typeof scopeOrCallback === "object" || typeof scopeOrCallback === "function")) {
            if (typeof methodOrName === "string") {
                scope = scopeOrCallback;
                callback = scope[methodOrName];
            } else if (typeof methodOrName === "function") {
                scope = scopeOrCallback;
                callback = methodOrName;
            } else if (!hasSecond && typeof scopeOrCallback.callback === "function") {
                scope = scopeOrCallback.scope;
                callback = scopeOrCallback.callback;
            }
        }
        if (typeof callback !== "function") {
            return callback; // the host reports what is missing
        }
        return function () { return callback.apply(scope, arguments); };
    }

    // A callback the host calls once (an asset request's outcome): its id is forgotten when it has been called.
    function oneShot(fn) {
        if (typeof fn !== "function") {
            return fn;
        }
        var wrapped = function () {
            var id = callbackIds.get(wrapped);
            if (id !== undefined) {
                unregisterCallback(id);
            }
            return fn.apply(this, arguments);
        };
        return wrapped;
    }

    function defineMethod(object, name, fn) {
        Object.defineProperty(object, name, { value: fn, writable: true, enumerable: true, configurable: true });
    }

    // A list of IDs as QtScript reads one (qVectorQUuidFromScriptValue, qVectorEntityItemIDFromScriptValue): each
    // element's text; no list at all for a value that is not an array
    function idList(items) {
        return Array.isArray(items) ? items.map(function (item) { return String(item); }) : [];
    }

    // setIgnoreItems and setIncludeItems take the lists as QScriptValues; the host's ...Vector forms take lists
    function itemListForms(object) {
        ["setIgnoreItems", "setIncludeItems"].forEach(function (name) {
            defineMethod(object, name, function (uid, items) {
                return host.call(object, name + "Vector", [uid, idList(items)]);
            });
        });
    }

    // What the QtScript engine added to an object as native functions, which the description cannot list
    var extraMembers = {
        "AssetScriptingInterface": function (assets) {
            // the host's forms take (values..., callback); a script may give a scope with the callback, as on QtScript
            ["getAsset", "putAsset", "deleteAsset", "resolveAsset", "decompressData", "compressData", "queryCacheMeta",
             "loadFromCache"].forEach(function (name) {
                defineMethod(assets, name, function (options, scopeOrCallback, methodOrName) {
                    return host.call(assets, name, [options, oneShot(scopedCallback(scopeOrCallback, methodOrName, arguments.length > 2))]);
                });
            });
            defineMethod(assets, "getCacheStatus", function (scopeOrCallback, methodOrName) {
                return host.call(assets, "getCacheStatus", [oneShot(scopedCallback(scopeOrCallback, methodOrName, arguments.length > 1))]);
            });
            defineMethod(assets, "saveToCache", function (first, second, third, fourth, fifth) {
                if (arguments.length >= 4) { // (url, data, metadata, scope[, callback])
                    return host.call(assets, "saveToCache", [first, second, third, oneShot(scopedCallback(fourth, fifth, arguments.length > 4))]);
                }
                return host.call(assets, "saveToCache", [first, oneShot(scopedCallback(second, third, arguments.length > 2))]);
            });
            // the forms with a callback alone call it with Assets as this (jsBindCallback(thisObject(), callback))
            [["uploadData", 1], ["downloadData", 1], ["setMapping", 2], ["getMapping", 1], ["setBakingEnabled", 2]].forEach(function (entry) {
                var name = entry[0], callbackIndex = entry[1];
                defineMethod(assets, name, function () {
                    var args = Array.prototype.slice.call(arguments);
                    if (typeof args[callbackIndex] === "function") {
                        args[callbackIndex] = oneShot(scopedCallback(assets, args[callbackIndex], true));
                    }
                    return host.call(assets, name, args);
                });
            });
        },
        "AvatarManager": function (avatars) {
            // findRayIntersection(ray, include, discard, pickAgainstMesh) takes the lists as QScriptValues; the host's
            // findRayIntersectionVector takes them as lists
            defineMethod(avatars, "findRayIntersection", function (ray, include, discard, pickAgainstMesh) {
                return host.call(avatars, "findRayIntersectionVector", [ray, idList(include), idList(discard),
                    pickAgainstMesh === undefined ? true : !!pickAgainstMesh]);
            });
            // setAvatarSortCoefficient(name, value) takes a number as a QScriptValue, and ignores anything else
            defineMethod(avatars, "setAvatarSortCoefficient", function (name, value) {
                if (typeof value === "number") {
                    return host.call(avatars, "setAvatarSortCoefficientValue", [name, value]);
                }
            });
        },
        "Overlays": function (overlays) {
            defineMethod(overlays, "findRayIntersection", function (ray, precisionPicking, include, discard, visibleOnly, collidableOnly) {
                return host.call(overlays, "findRayIntersectionVector", [ray, !!precisionPicking, idList(include), idList(discard),
                    !!visibleOnly, !!collidableOnly]);
            });
        },
        "PickScriptingInterface": itemListForms,
        "RayPickScriptingInterface": itemListForms,
        "LaserPointerScriptingInterface": itemListForms,
        "PointerScriptingInterface": itemListForms,
        "EntityScriptingInterface": function (entities) {
            // findRayIntersection with lists: the QtScript form takes them as QScriptValues, so a module's call would
            // reach the shorter form and drop them; the host's findRayIntersectionVector takes them as lists
            if (typeof entities.findRayIntersectionVector === "function") {
                defineMethod(entities, "findRayIntersection", function (ray, precisionPicking, include, discard, visibleOnly, collidableOnly) {
                    return host.call(entities, "findRayIntersectionVector",
                        [ray, !!precisionPicking, idList(include), idList(discard), !!visibleOnly, !!collidableOnly]);
                });
            }
            // queryPropertyMetadata(id, property, callback) or (id, property, scope, method): the host's form takes a
            // callback called with (error, result); the error becomes an Error carrying its fields, as QtScript's
            // makeError made it, and the checks QtScript made before asking are made here
            defineMethod(entities, "queryPropertyMetadata", function (entityID, property, scopeOrCallback, methodOrName) {
                var name = String(property);
                var callback = scopedCallback(scopeOrCallback, methodOrName, arguments.length > 3);
                if (typeof callback !== "function") {
                    throw new TypeError("callback is not a function");
                }
                if (name !== "script" && name !== "serverScripts") {
                    throw new Error("metadata for property " + name + " is not yet queryable");
                }
                return host.call(entities, "queryPropertyMetadata", [entityID, name, function (error, result) {
                    var err;
                    if (error) {
                        err = new Error(error.message);
                        for (var key in error) {
                            if (Object.prototype.hasOwnProperty.call(error, key)) { err[key] = error[key]; }
                        }
                    }
                    return callback(err, result === null ? undefined : result);
                }]);
            });
            // getMultipleEntityProperties(ids, properties): one getEntityProperties per id
            Object.defineProperty(entities, "getMultipleEntityProperties", {
                value: function (ids, properties) {
                    var list = Array.isArray(ids) ? ids : [ids];
                    var out = [];
                    for (var i = 0; i < list.length; i++) {
                        out.push(properties === undefined ? entities.getEntityProperties(list[i]) : entities.getEntityProperties(list[i], properties));
                    }
                    return out;
                },
                writable: true, enumerable: true, configurable: true
            });
        }
    };

    function wrapObject(handle, className) {
        var isGlobal = globalHandleSet.has(handle);
        var object = isGlobal ? globalObjects.get(handle) : undefined;
        if (!isGlobal) {
            var ref = objectsByHandle.get(handle);
            object = ref !== undefined ? ref.deref() : undefined;
        }
        if (object) {
            return object;
        }
        var description = host.describe(host.newHostObject(handle, null)); // a stand-in, never registered for release
        if (!description || !description["class"]) {
            return null; // the object is gone
        }
        className = description["class"];
        object = host.newHostObject(handle, prototypeFor(className, description));
        defineMembers(object, className, description);
        if (className === "RefusedGlobal") {
            // a global the script API policy keeps from a world's scripts: the host registered a stand-in with
            // nothing but its name, so there is nothing to call either way; this only names the refusal
            object = refusedStandIn(object.refusedName, object);
        }
        var twins = methodTwins[className];
        if (twins) {
            Object.keys(twins).forEach(function (name) {
                Object.defineProperty(object, name, { value: makeMethod(twins[name]), writable: true, enumerable: true, configurable: true });
            });
        }
        if (extraMembers[className]) {
            extraMembers[className](object);
        }
        if (isGlobal) {
            globalObjects.set(handle, object);
        } else {
            objectsByHandle.set(handle, new WeakRef(object));
            releases.register(object, handle);
        }
        return object;
    }

    function refusedStandIn(name, inner) {
        var passThrough = { refusedName: true, toString: true, valueOf: true, toJSON: true, constructor: true };
        return new Proxy(inner, {
            get: function (target, property) {
                if (typeof property === "symbol" || passThrough[property]) {
                    return target[property];
                }
                return function () {
                    var line = name + "." + String(property) + " is for the visitor's own scripts; a world's script may not use it";
                    host.log(LOG_WARNING, line);
                    throw new TypeError(line);
                };
            },
            set: function (target, property) {
                host.log(LOG_WARNING, name + "." + String(property) + " is for the visitor's own scripts; a world's script may not set it");
                return true;
            }
        });
    }

    // --- the globals: defined lazily, resolved on first use ------------------------------------------------------
    var globalHandles = host.globals();
    var globalNames = Object.keys(globalHandles);
    var globalHandleSet = new Set(globalNames.map(function (name) { return globalHandles[name]; }));

    function defineGlobal(name, handle) {
        var dot = name.indexOf(".");
        if (dot > 0) {
            // "Window.location": the object under the parent, when the parent exists
            var parentName = name.slice(0, dot), childName = name.slice(dot + 1);
            if (globalHandles[parentName] !== undefined) {
                pendingChildren.push({ parent: parentName, child: childName, handle: handle });
                return;
            }
        }
        Object.defineProperty(global, name, {
            get: function () {
                var object = wrapObject(handle, undefined);
                Object.defineProperty(global, name, { value: object, writable: true, enumerable: true, configurable: true });
                return object;
            },
            set: function (value) {
                Object.defineProperty(global, name, { value: value, writable: true, enumerable: true, configurable: true });
            },
            enumerable: true,
            configurable: true
        });
    }

    var pendingChildren = [];
    for (var g = 0; g < globalNames.length; g++) {
        defineGlobal(globalNames[g], globalHandles[globalNames[g]]);
    }
    pendingChildren.forEach(function (entry) {
        var parent = global[entry.parent];
        if (parent) {
            Object.defineProperty(parent, entry.child, {
                get: function () { return wrapObject(entry.handle, undefined); },
                enumerable: true,
                configurable: true
            });
        }
    });

    var Script = global.Script; // the engine; the rest of the shim adds to it
    if (!Script) {
        throw new Error("the host registered no Script object");
    }
    var scriptHandle = globalHandles.Script;

    // --- printing ----------------------------------------------------------------------------------------------
    function joinArguments(args) {
        var parts = [];
        for (var i = 0; i < args.length; i++) {
            var a = args[i];
            parts.push(a instanceof Error ? describeError(a) : String(a));
        }
        return parts.join(" ");
    }

    global.print = function () { host.log(LOG_PRINT, joinArguments(arguments)); };

    var consoleGroups = 0;
    var consoleTimers = new Map();
    function consoleLine(level) {
        return function () {
            var indent = consoleGroups ? new Array(consoleGroups + 1).join("  ") : "";
            host.log(level, indent + joinArguments(arguments));
        };
    }
    global.console = {
        log: consoleLine(LOG_PRINT),
        info: consoleLine(LOG_INFO),
        debug: consoleLine(LOG_DEBUG),
        warn: consoleLine(LOG_WARNING),
        error: consoleLine(LOG_ERROR),
        exception: consoleLine(LOG_ERROR),
        assert: function (condition) {
            if (!condition) {
                var rest = Array.prototype.slice.call(arguments, 1);
                host.log(LOG_ERROR, "Assertion failed" + (rest.length ? ": " + joinArguments(rest) : ""));
            }
        },
        group: function () { consoleLine(LOG_PRINT).apply(undefined, arguments); consoleGroups++; },
        groupCollapsed: function () { consoleLine(LOG_PRINT).apply(undefined, arguments); consoleGroups++; },
        groupEnd: function () { if (consoleGroups > 0) { consoleGroups--; } },
        time: function (label) { consoleTimers.set(String(label), Date.now()); },
        timeEnd: function (label) {
            var started = consoleTimers.get(String(label));
            if (started !== undefined) {
                consoleTimers.delete(String(label));
                host.log(LOG_PRINT, label + ": " + (Date.now() - started) + "ms");
            }
        },
        trace: function () { host.log(LOG_PRINT, joinArguments(arguments) + "\n" + (new Error().stack || "")); },
        // the engine's own console object empties the Script Log (and refuses a script that may not)
        clear: function () {
            var hostConsole = globalHandles.console !== undefined ? wrapObject(globalHandles.console, undefined) : null;
            if (hostConsole) {
                host.call(hostConsole, "clear", []);
            }
        }
    };

    // --- Script: the engine's API that was QtScript-specific --------------------------------------------------------
    var timers = new Map(); // timer id -> callback id

    var timerEntities = new Map(); // timer id -> the entity script that set it ("" for the script itself)

    function makeTimer(method, fn, ms) {
        if (typeof fn !== "function") {
            throw new TypeError("Script." + method + ": the first argument must be a function");
        }
        var callbackId = registerCallback(fn);
        var timer = host.call(Script, method, [{ "@callback": callbackId }, ms | 0]);
        if (timer) {
            timers.set(timer, callbackId);
            timerEntities.set(timer, currentEntity);
        }
        return timer;
    }

    function clearTimer(timer) {
        if (!timer) { return; }
        var callbackId = timers.get(timer);
        timers.delete(timer);
        timerEntities.delete(timer);
        host.call(Script, "clearTimeout", [timer]);
        if (callbackId !== undefined) {
            unregisterCallback(callbackId);
        }
    }

    function clearTimersOf(entity) {
        var toClear = [];
        timerEntities.forEach(function (owner, timer) { if (owner === entity) { toClear.push(timer); } });
        toClear.forEach(clearTimer);
    }

    function define(object, name, value) {
        Object.defineProperty(object, name, { value: value, writable: true, enumerable: false, configurable: true });
    }

    define(Script, "setTimeout", function (fn, ms) {
        var wrapped = function () {
            var callbackId = callbackIds.get(wrapped);
            // single shot: the host forgets the timer; forget the callback too
            timers.forEach(function (cb, id) { if (cb === callbackId) { timers.delete(id); } });
            if (callbackId !== undefined) { unregisterCallback(callbackId); }
            fn();
        };
        return makeTimer("setTimeout", wrapped, ms);
    });
    define(Script, "setInterval", function (fn, ms) { return makeTimer("setInterval", fn, ms); });
    define(Script, "clearTimeout", clearTimer);
    define(Script, "clearInterval", clearTimer);
    define(Script, "print", function () { host.log(LOG_PRINT, joinArguments(arguments)); });

    // The file whose code is running, for relative paths. QtScript took it from the call stack (the file of
    // the function calling resolvePath, include or require); so does this, from an Error's stack, and when
    // no frame names a file (code evaluated without one), from the include or module being evaluated, then
    // the script itself.
    var fileName;
    var parentStack = [];
    function callerFile() {
        var stack = new Error().stack;
        if (!stack) {
            return undefined;
        }
        var lines = stack.split("\n");
        for (var i = 0; i < lines.length; i++) {
            var m = /\(([^()]*):(\d+):(\d+)\)\s*$/.exec(lines[i]);
            if (m && m[1] !== "runtime.js" && m[1] !== "(evaluate)" && m[1] !== "(script)") {
                return m[1];
            }
        }
        return undefined;
    }
    function currentParent() {
        return callerFile() || (parentStack.length ? parentStack[parentStack.length - 1] : fileName);
    }

    define(Script, "resolvePath", function (path) {
        return host.call(Script, "resolvePathFrom", [String(path), currentParent()]);
    });
    define(Script, "evaluate", function (program, name) {
        return host.evaluate(String(program), name === undefined ? "(evaluate)" : String(name));
    });
    define(Script, "makeError", function (value, type) {
        var error = value instanceof Error ? value : new Error(value === undefined ? "" : String(value));
        if (type) { error.name = String(type); }
        return error;
    });
    define(Script, "formatException", function (e) { return describeError(e); });
    // ScriptEngine::registerValue: the value under its dotted name on the global object, unless the name is taken;
    // a missing part on the way is made an array, as there
    define(Script, "registerValue", function (valueName, value) {
        var parts = String(valueName).split(".");
        var object = global;
        for (var i = 0; i < parts.length; i++) {
            var part = parts[i];
            if (!(part in object)) {
                object[part] = i < parts.length - 1 ? [] : value;
            }
            object = object[part];
            if (object === null || (typeof object !== "object" && typeof object !== "function")) {
                break;
            }
        }
    });
    define(Script, "lintScript", function () { return []; });

    // Script.include: synchronous (a callback, when given, is called after the files ran)
    var includedUrls = new Set();

    // QtScript never honoured "use strict", and the system scripts rely on that (a strict file assigning
    // undeclared globals); the directive is blanked before evaluation, so the scripts run as they did.
    function sloppy(source) {
        return source.replace(/(^|[;{}\n\r])(\s*)(["'])use strict\3(\s*;?)/g, function (m, before, space, quote, after) {
            return before + space + quote + "use sloppy" + quote + after;
        });
    }

    // QtScript resolved a relative path against the file of the code making the call, and an included file
    // ran in the scope of the code that included it (its variables visible to the included file, the included
    // file's `var`s to it). Both are reproduced at load time: every Script.resolvePath, require and include
    // call gets its file's URL baked in, and an include becomes a direct eval at its call site of the text
    // this loader prepares (prepareInclude).
    function instrument(source, url) {
        var u = JSON.stringify(url);
        return rewriteIncludes(sloppy(source)
            .replace(/\bScript\s*\.\s*require\s*\.\s*resolve\s*\(/g, "__darkride.requireResolveFrom(" + u + ")(")
            .replace(/\bScript\s*\.\s*require\s*\(/g, "__darkride.requireFrom(" + u + ")(")
            .replace(/\bScript\s*\.\s*resolvePath\s*\(/g, "__darkride.resolvePathFrom(" + u + ")("), u);
    }

    // Script.include(args) becomes eval(__darkride.includeFrom(url)(args)): the call's closing parenthesis is
    // found by scanning (strings, template literals and comments skipped), and one more is added after it.
    var INCLUDE_CALL = /\bScript\s*\.\s*include\s*\(/g;
    function rewriteIncludes(text, u) {
        var out = "", last = 0, m;
        INCLUDE_CALL.lastIndex = 0;
        while ((m = INCLUDE_CALL.exec(text)) !== null) {
            var open = m.index + m[0].length;  // just after "("
            var i = open, depth = 1, n = text.length;
            while (i < n && depth > 0) {
                var c = text[i];
                if (c === "'" || c === '"' || c === "`") {
                    var q = c;
                    i++;
                    while (i < n && text[i] !== q) { if (text[i] === "\\") { i++; } i++; }
                    i++;
                } else if (c === "/" && text[i + 1] === "/") {
                    while (i < n && text[i] !== "\n") { i++; }
                } else if (c === "/" && text[i + 1] === "*") {
                    i = text.indexOf("*/", i + 2);
                    i = i < 0 ? n : i + 2;
                } else {
                    if (c === "(" || c === "[" || c === "{") { depth++; }
                    else if (c === ")" || c === "]" || c === "}") { depth--; }
                    i++;
                }
            }
            if (depth !== 0) {
                break;  // unbalanced: leave the rest as it is
            }
            // i is just after the closing ")"
            out += text.slice(last, m.index) + "eval(__darkride.includeFrom(" + u + ")(" + text.slice(open, i) + ")";
            last = i;
            INCLUDE_CALL.lastIndex = i;
        }
        return out + text.slice(last);
    }

    function evaluateFile(source, url) {
        parentStack.push(url);
        try {
            return host.evaluate(instrument(source, url), url);
        } finally {
            parentStack.pop();
        }
    }

    // The entity script whose code is running, for the sandbox rules of include and require, and for the
    // timers it sets (cleared when it unloads); empty outside entity scripts.
    var currentEntity = "";
    var currentSandbox = "";

    function includeOne(file) {
        var resolved = host.call(Script, "resolveInclude", [String(file), currentParent(), currentSandbox]);
        if (!resolved || !resolved.url) {
            if (resolved && resolved.reason) {
                host.log(LOG_WARNING, "Script.include() -- skipping " + file + " -- " + resolved.reason);
            }
            return;
        }
        var url = resolved.url;
        if (includedUrls.has(url)) {
            host.log(LOG_PRINT, "Script.include() skipping evaluation of previously included url:" + url);
            return;
        }
        var fetched = host.call(Script, "fetchScript", [url, false, currentSandbox]);
        if (!fetched || !fetched.success) {
            host.log(LOG_ERROR, "Error loading file (" + (fetched ? fetched.status : "no reply") + "): " + url);
            return;
        }
        includedUrls.add(url);
        try {
            evaluateFile(fetched.contents, url);
        } catch (e) {
            reportError("in " + url, e);
        }
    }

    // Script.include called through a reference (not rewritten): the files run in the global scope
    define(Script, "include", function (files, callback) {
        var list = Array.isArray(files) ? files : [files];
        for (var i = 0; i < list.length; i++) {
            includeOne(list[i]);
        }
        if (typeof callback === "function") {
            callback();
        }
    });

    // The text an include call site evals: each file fetched, prepared as a file of its own, bracketed so
    // that an error in one is reported and the next still runs, and the callback called at the end.
    var includeCallbacks = [];
    function prepareInclude(fromUrl, files, callback) {
        var list = Array.isArray(files) ? files : [files];
        var text = "";
        for (var i = 0; i < list.length; i++) {
            var resolved = host.call(Script, "resolveInclude", [String(list[i]), fromUrl, currentSandbox]);
            if (!resolved || !resolved.url) {
                if (resolved && resolved.reason) {
                    host.log(LOG_WARNING, "Script.include() -- skipping " + list[i] + " -- " + resolved.reason);
                }
                continue;
            }
            var url = resolved.url;
            if (includedUrls.has(url)) {
                host.log(LOG_PRINT, "Script.include() skipping evaluation of previously included url:" + url);
                continue;
            }
            var fetched = host.call(Script, "fetchScript", [url, false, currentSandbox]);
            if (!fetched || !fetched.success) {
                host.log(LOG_ERROR, "Error loading file (" + (fetched ? fetched.status : "no reply") + "): " + url);
                continue;
            }
            includedUrls.add(url);
            var u = JSON.stringify(url);
            text += "__darkride.beginInclude(" + u + ");\ntry {\n" + instrument(fetched.contents, url) +
                "\n} catch (__darkrideError) { __darkride.includeError(" + u + ", __darkrideError); } finally { __darkride.endInclude(); }\n";
        }
        if (typeof callback === "function") {
            includeCallbacks.push(callback);
            text += "__darkride.includeCallback();\n";
        }
        return text;
    }

    Object.defineProperty(global, "__darkride", {
        value: {
            includeFrom: function (fromUrl) {
                return function (files, callback) { return prepareInclude(fromUrl, files, callback); };
            },
            beginInclude: function (url) { parentStack.push(url); },
            endInclude: function () { parentStack.pop(); },
            includeError: function (url, e) { reportError("in " + url, e); },
            includeCallback: function () {
                var callback = includeCallbacks.pop();
                if (callback) { callback(); }
            },
            resolvePathFrom: function (fromUrl) {
                return function (path) { return host.call(Script, "resolvePathFrom", [path === undefined ? "" : String(path), fromUrl]); };
            },
            requireFrom: function (fromUrl) {
                // a bare name is a system module, a path is relative to the calling file
                return function (id) { return require(requireResolve(id, "", fromUrl)); };
            },
            requireResolveFrom: function (fromUrl) {
                return function (id, relativeTo) { return requireResolve(id, relativeTo, fromUrl); };
            }
        },
        writable: false, enumerable: false, configurable: false
    });

    // Script.require: the CommonJS loader, as the QtScript engine had it (Script.require.cache, module.require,
    // JSON modules, system modules by bare name, cyclic dependencies through early registration)
    var moduleCache = Object.create(null);
    var moduleStack = [];

    function currentModule() {
        return moduleStack.length ? moduleStack[moduleStack.length - 1] : undefined;
    }

    function registerModuleWithParent(module, parent) {
        if (!parent || !Array.isArray(parent.children)) {
            return;
        }
        for (var i = 0; i < parent.children.length; i++) {
            if (parent.children[i].id === module.id) {
                parent.children[i] = module;
                return;
            }
        }
        parent.children.push(module);
    }

    function requireResolve(moduleId, relativeTo, fromUrl) {
        var result = host.call(Script, "resolveModule", [String(moduleId), relativeTo === undefined ? "" : String(relativeTo), fromUrl === undefined ? currentParent() : fromUrl, currentSandbox]);
        if (!result || result.error) {
            var error = new Error(result ? result.error : "Cannot find module '" + moduleId + "'");
            if (result && result.type) { error.name = result.type; }
            throw error;
        }
        return result.url;
    }

    function newModule(modulePath, parent) {
        var module = {
            id: modulePath,
            filename: modulePath,
            exports: {},
            loaded: false,
            parent: parent,
            children: []
        };
        module.require = function (id) { return require(requireResolve(id, module.filename)); };
        module.require.resolve = function (id) { return requireResolve(id, module.filename); };
        module.require.cache = moduleCache;
        return module;
    }

    function instantiateModule(module, source) {
        var modulePath = module.filename;
        moduleStack.push(module);
        parentStack.push(modulePath);
        try {
            if (/\.json$/i.test(modulePath.replace(/[?#].*$/, ""))) {
                module.exports = JSON.parse(source);
            } else {
                var dirname = modulePath.replace(/\/[^\/]*$/, "");
                var wrapper = host.evaluate("(function (module, exports, require, __filename, __dirname) {" + instrument(source, modulePath) + "\n})", modulePath);
                wrapper.call(module.exports, module, module.exports, module.require, modulePath, dirname);
            }
        } finally {
            parentStack.pop();
            moduleStack.pop();
        }
    }

    function require(moduleId) {
        var modulePath = requireResolve(moduleId);
        var module = moduleCache[modulePath];
        var parent = currentModule();
        if (module && module.exports && typeof module.exports === "object") {
            registerModuleWithParent(module, parent);
            return module.exports;
        }
        module = newModule(modulePath, parent);
        registerModuleWithParent(module, parent);
        moduleCache[modulePath] = module;
        var fetched = host.call(Script, "fetchScript", [modulePath, false, currentSandbox]);
        if (!fetched || !fetched.success) {
            delete moduleCache[modulePath];
            throw new Error("error retrieving script (" + (fetched ? fetched.status : "no reply") + "): " + modulePath);
        }
        try {
            instantiateModule(module, fetched.contents);
        } catch (e) {
            delete moduleCache[modulePath];
            throw e;
        }
        module.loaded = true;
        return module.exports;
    }
    require.resolve = function (id, relativeTo) { return requireResolve(id, relativeTo); };
    require.cache = moduleCache;
    define(Script, "require", require);
    define(Script, "resetModuleCache", function () {
        Object.keys(moduleCache).forEach(function (key) { delete moduleCache[key]; });
    });

    // --- the plain-object globals the hosts set on the global object (DriveKeys, PickType, ...) --------------------
    var values = host.call(Script, "scriptGlobalValues", []);
    if (values && typeof values === "object") {
        Object.keys(values).forEach(function (name) {
            if (!(name in global)) {
                global[name] = values[name];
            }
        });
    }

    // --- XMLHttpRequest over the engine's Http object ---------------------------------------------------------------
    var UNSENT = 0, OPENED = 1, HEADERS_RECEIVED = 2, LOADING = 3, DONE = 4;

    // The engine's makers of requests and sockets are the shim's alone: a script reaches them through XMLHttpRequest
    // and WebSocket, which name the script they are made for; off the global, nothing can name another.
    var httpFactory = global.Http;
    var socketsFactory = global.Sockets;
    delete global.Http;
    delete global.Sockets;

    function XMLHttpRequest() {
        if (!httpFactory) {
            throw new Error("XMLHttpRequest: the engine has no Http object");
        }
        // made for the entity script running, else the script itself: the host's network rule judges it by that
        var request = httpFactory.createRequest(currentSandbox || fileName || "");
        var self = this;
        this.onreadystatechange = null;
        this.ontimeout = null;
        this.onload = null;
        this.onerror = null;
        this.onabort = null;
        this.onloadend = null;
        Object.defineProperty(this, "_request", { value: request });
        // The request's signals are connected only while it can still report something, from open() until it is
        // done or aborted. A connection keeps its handlers, and through them this object and the host's request,
        // reachable; held for good, every request a script ever made would stay, in the guest's memory and the
        // host's (a body that posts what it hears ran out of both in two hours).
        var connected = false;
        function onReadyStateChanged() {
            if (typeof self.onreadystatechange === "function") {
                try { self.onreadystatechange.call(self); } catch (e) { reportError("in onreadystatechange", e); }
            }
            if (request.readyState === DONE) {
                var handler = request.errorCode === 0 ? self.onload : self.onerror;
                if (typeof handler === "function") {
                    try { handler.call(self); } catch (e) { reportError("in an XMLHttpRequest handler", e); }
                }
                if (typeof self.onloadend === "function") {
                    try { self.onloadend.call(self); } catch (e) { reportError("in onloadend", e); }
                }
                disconnectRequest();
            }
        }
        function onTimedOut() {
            if (typeof self.ontimeout === "function") {
                try { self.ontimeout.call(self); } catch (e) { reportError("in ontimeout", e); }
            }
        }
        function connectRequest() {
            if (!connected) {
                connected = true;
                request.readyStateChanged.connect(onReadyStateChanged);
                request.timedOut.connect(onTimedOut);
            }
        }
        function disconnectRequest() {
            if (connected) {
                connected = false;
                request.readyStateChanged.disconnect(onReadyStateChanged);
                request.timedOut.disconnect(onTimedOut);
            }
        }
        Object.defineProperty(this, "_connect", { value: connectRequest });
        Object.defineProperty(this, "_disconnect", { value: disconnectRequest });
    }
    XMLHttpRequest.UNSENT = UNSENT;
    XMLHttpRequest.OPENED = OPENED;
    XMLHttpRequest.HEADERS_RECEIVED = HEADERS_RECEIVED;
    XMLHttpRequest.LOADING = LOADING;
    XMLHttpRequest.DONE = DONE;
    var xhrProto = XMLHttpRequest.prototype;
    ["UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE"].forEach(function (name) {
        Object.defineProperty(xhrProto, name, { value: XMLHttpRequest[name], enumerable: true });
    });
    ["response", "responseText", "status", "statusText", "readyState", "errorCode"].forEach(function (name) {
        Object.defineProperty(xhrProto, name, { get: function () { return this._request[name]; }, enumerable: true });
    });
    ["responseType", "timeout"].forEach(function (name) {
        Object.defineProperty(xhrProto, name, {
            get: function () { return this._request[name]; },
            set: function (value) { this._request[name] = value; },
            enumerable: true
        });
    });
    xhrProto.open = function (method, url, async, username, password) {
        this._connect();  // again for a request opened after it was done
        this._request.open(String(method), String(url), async === undefined ? true : !!async, username === undefined ? "" : String(username), password === undefined ? "" : String(password));
    };
    xhrProto.send = function (data) {
        if (data === undefined || data === null) {
            this._request.send();
        } else if (typeof data === "string" || data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
            this._request.send(data);
        } else {
            this._request.send(String(data));
        }
    };
    // an aborted request reports nothing more (the host drops its reply), so its signals go now
    xhrProto.abort = function () { this._request.abort(); this._disconnect(); };
    xhrProto.setRequestHeader = function (name, value) { this._request.setRequestHeader(String(name), String(value)); };
    xhrProto.getAllResponseHeaders = function () { return this._request.getAllResponseHeaders(); };
    xhrProto.getResponseHeader = function (name) { return this._request.getResponseHeader(String(name)); };
    global.XMLHttpRequest = XMLHttpRequest;

    // --- WebSocket over the engine's Sockets object ---------------------------------------------------------------------
    // the script's side of a host socket: its on* handlers, fired from the socket's signals
    // (connected until the socket has closed, as a request's are until it is done: then the socket can be collected)
    function attachSocket(target, socket) {
        target.onopen = null;
        target.onmessage = null;
        target.onerror = null;
        target.onclose = null;
        Object.defineProperty(target, "_socket", { value: socket });
        function fire(name, event) {
            var handler = target[name];
            if (typeof handler === "function") {
                try { event === undefined ? handler.call(target) : handler.call(target, event); } catch (e) { reportError("in " + name, e); }
            }
        }
        function onOpened() { fire("onopen"); }
        function onMessage(message) { fire("onmessage", { data: message }); }
        function onErrored() { fire("onerror"); }
        function onClosed(code, reason, wasClean) {
            fire("onclose", { code: code, reason: reason, wasClean: wasClean });
            socket.opened.disconnect(onOpened);
            socket.messageReceived.disconnect(onMessage);
            socket.errored.disconnect(onErrored);
            socket.closedWith.disconnect(onClosed);
        }
        socket.opened.connect(onOpened);
        socket.messageReceived.connect(onMessage);
        socket.errored.connect(onErrored);
        socket.closedWith.connect(onClosed);
    }
    function WebSocket(url) {
        if (!socketsFactory) {
            throw new Error("WebSocket: the engine has no Sockets object");
        }
        attachSocket(this, socketsFactory.createWebSocket(url === undefined ? "" : String(url), currentSandbox || fileName || ""));
    }
    WebSocket.CONNECTING = 0;
    WebSocket.OPEN = 1;
    WebSocket.CLOSING = 2;
    WebSocket.CLOSED = 3;
    var wsProto = WebSocket.prototype;
    ["CONNECTING", "OPEN", "CLOSING", "CLOSED"].forEach(function (name) {
        Object.defineProperty(wsProto, name, { value: WebSocket[name], enumerable: true });
    });
    ["readyState", "url", "bufferedAmount", "extensions", "protocol"].forEach(function (name) {
        Object.defineProperty(wsProto, name, { get: function () { return this._socket[name]; }, enumerable: true });
    });
    Object.defineProperty(wsProto, "binaryType", {
        get: function () { return this._socket.binaryType; },
        set: function (value) { this._socket.binaryType = String(value); },
        enumerable: true
    });
    wsProto.send = function (message) { this._socket.send(String(message)); };
    wsProto.close = function (code, reason) {
        if (code === undefined) { this._socket.close(); }
        else if (reason === undefined) { this._socket.close(code | 0); }
        else { this._socket.close(code | 0, String(reason)); }
    };
    global.WebSocket = WebSocket;

    // --- WebSocketServer, over the engine's WebSocketServers object, which only an agent's engine has ---------------
    if (globalHandles.WebSocketServers !== undefined) {
        var WebSocketServer = function (options) {
            var serverName = "Scripted WebSocket Listener", port = 0;  // port 0 picks a free one
            if (options && typeof options === "object") {
                if (typeof options.serverName === "string") { serverName = options.serverName; }
                if (typeof options.port === "number") { port = options.port; }
            }
            var server = global.WebSocketServers.createWebSocketServer(serverName, port);
            var self = this, handlers = [];
            Object.defineProperty(this, "_server", { value: server });
            // newConnection as QtScript has it, a signal whose handlers get each client, here as a WebSocket
            this.newConnection = {
                connect: function (handler) { if (typeof handler === "function") { handlers.push(handler); } },
                disconnect: function (handler) { var i = handlers.indexOf(handler); if (i >= 0) { handlers.splice(i, 1); } }
            };
            var onNewConnection = function (socket) {
                var client = Object.create(WebSocket.prototype);
                attachSocket(client, socket);
                handlers.slice().forEach(function (handler) {
                    try { handler.call(self, client); } catch (e) { reportError("in newConnection", e); }
                });
            };
            server.newConnection.connect(onNewConnection);
            // a closed server takes no more connections; its signal goes with close(), so it can be collected
            Object.defineProperty(this, "_disconnect", { value: function () { server.newConnection.disconnect(onNewConnection); } });
        };
        ["url", "port", "listening"].forEach(function (name) {
            Object.defineProperty(WebSocketServer.prototype, name, { get: function () { return this._server[name]; }, enumerable: true });
        });
        WebSocketServer.prototype.close = function () { this._server.close(); this._disconnect(); };
        global.WebSocketServer = WebSocketServer;
    }

    // --- request(): the helper the QtScript engine evaluated into every script --------------------------------------
    global.request = function (options, callback) {
        var httpRequest = new XMLHttpRequest(), key;
        httpRequest.onreadystatechange = function () {
            var READY_STATE_DONE = 4;
            var HTTP_OK = 200;
            if (httpRequest.readyState >= READY_STATE_DONE) {
                var error = (httpRequest.status !== HTTP_OK) && httpRequest.status.toString() + ': ' + httpRequest.statusText;
                var body = !error && httpRequest.responseText;
                var contentType = !error && httpRequest.getResponseHeader('content-type');
                var response = !error && { statusCode: httpRequest.status };
                if (!error) {
                    response.headers = httpRequest.getAllResponseHeaders()
                        .split("\n")
                        .filter(function (pairStr) { return pairStr.indexOf(":") > -1; })
                        .reduce(function (headers, pairStr) {
                            var pair = pairStr.split(": ");
                            if (pair.length > 1) {
                                var k = pair[0];
                                pair.shift();
                                headers[k.toLowerCase()] = pair.join(": ");
                            }
                            return headers;
                        }, {});
                    if (contentType && contentType.indexOf('application/json') === 0) {
                        try { body = JSON.parse(body); } catch (e) { error = e; }
                    }
                }
                callback(error, response, body);
                httpRequest.onreadystatechange = null;
            }
        };
        if (typeof options === "string") {
            options = { uri: options };
        }
        if (options.url) {
            options.uri = options.url;
        }
        if (!options.method) {
            options.method = 'GET';
        }
        if (options.body && (options.method === 'GET')) {
            options.method = 'POST';
        }
        if (options.json) {
            options.headers = options.headers || {};
            options.headers["Content-Type"] = "application/json";
            if (options.body) {
                options.body = JSON.stringify(options.body);
            }
        }
        httpRequest.open(options.method, options.uri, true);
        for (key in options.headers || {}) {
            httpRequest.setRequestHeader(key, options.headers[key]);
        }
        httpRequest.send(options.body || null);
    };

    // --- the QML windows: constructors over the factories the engine registers under the same names ------------------------
    ["OverlayWindow", "OverlayWebWindow"].forEach(function (name) {
        var handle = globalHandles[name];
        if (handle === undefined) {
            return;
        }
        var factory = null;
        function windowProperties(args) {
            if (args.length > 1) {
                var properties = {};
                if (args[0] !== undefined) { properties.title = String(args[0]); }
                if (args[1] !== undefined) { properties.source = String(args[1]); }
                if (typeof args[2] === "number") { properties.width = args[2] | 0; }
                if (typeof args[3] === "number") { properties.height = args[3] | 0; }
                return properties;
            }
            return (args[0] !== null && typeof args[0] === "object") ? args[0] : {};
        }
        var constructor = function () {
            if (!factory) {
                factory = wrapObject(handle, undefined);
            }
            var window = factory ? factory.create(windowProperties(arguments)) : null;
            if (!window) {
                throw new Error(name + ": the window could not be created");
            }
            return window;
        };
        Object.defineProperty(global, name, { value: constructor, writable: true, enumerable: false, configurable: true });
    });

    // --- new AudioEffectOptions(options): a constructor over the factory the engine registers under the name ----------
    (function () {
        var handle = globalHandles.AudioEffectOptions;
        if (handle === undefined) {
            return;
        }
        var factory = null;
        var constructor = function (options) {
            if (!factory) {
                factory = wrapObject(handle, undefined);
            }
            return factory.create((options !== null && typeof options === "object") ? options : {});
        };
        Object.defineProperty(global, "AudioEffectOptions", { value: constructor, writable: true, enumerable: false, configurable: true });
    }());

    // --- odds and ends of the old global object ------------------------------------------------------------------------
    global.gc = function () { host.gc(); };
    global.version = function () { return "QuickJS-NG on WebAssembly"; };

    // Script.dumpApi: what this script sees, in the format of the QtScript engine's dump, written by the host
    define(Script, "dumpApi", function (path) {
        var globals = {};
        var names = Object.getOwnPropertyNames(global);
        names.forEach(function (name) {
            var value;
            try { value = global[name]; } catch (e) { return; }
            var entry;
            var handle = (value !== null && typeof value === "object") ? host.handleOf(value) : 0;
            if (handle) {
                entry = { kind: "qobject", meta: host.describe(value) };
            } else if (typeof value === "function") {
                entry = { kind: "function" };
            } else if (Array.isArray(value)) {
                entry = { kind: "array" };
            } else if (value !== null && typeof value === "object") {
                var members = {};
                Object.getOwnPropertyNames(value).forEach(function (member) {
                    var m;
                    try { m = value[member]; } catch (e) { return; }
                    members[member] = typeof m === "function" ? "function" : (m !== null && typeof m === "object") ? "object" : typeof m;
                });
                entry = { kind: "object", members: members };
            } else {
                entry = { kind: value === undefined ? "undefined" : typeof value };
            }
            globals[name] = entry;
        });
        return host.call(Script, "dumpApi", [String(path), globals]);
    });

    // --- entity scripts: the contract the engine drives through the module's entity_* exports ---------------------
    // A script is "(function () { this.preload = function (id) {...}; ... })": evaluated as a file of its own, it
    // must give a function, which is constructed once per entity; the methods are called with the entity's id
    // first, as the QtScript engine called them. One object per entity; the code is shared by its URL.
    var entityScripts = new Map(); // entity id -> { object, url }

    function withEntity(entity, url, fn) {
        var previousEntity = currentEntity, previousSandbox = currentSandbox;
        currentEntity = entity;
        currentSandbox = url;
        try {
            return fn();
        } finally {
            currentEntity = previousEntity;
            currentSandbox = previousSandbox;
        }
    }

    function errorRecord(e) {
        return { "@error": { type: (e && e.name) ? String(e.name) : "Error", message: describeError(e) } };
    }

    function entityLoad(payload) {
        var entity = String(payload.entity), url = String(payload.url), source = payload.source;
        if (typeof source !== "string") {
            return errorRecord(new Error("no source for " + url));
        }
        try {
            return withEntity(entity, url, function () {
                var constructor = host.evaluate(instrument(source, url), url);
                if (typeof constructor !== "function") {
                    var type = constructor === null ? "null" : typeof constructor;
                    throw new Error("failed to load entity script -- expected a function, got " + type);
                }
                var object = new constructor();
                entityScripts.set(entity, { object: object, url: url });
                return null;
            });
        } catch (e) {
            return errorRecord(e);
        }
    }

    function entityUnload(payload) {
        var entity = String(payload.entity);
        entityScripts.delete(entity);
        clearTimersOf(entity);
        disconnectHandlersOf(entity);
        unregisterCallbacksOf(entity);
    }

    // After the host interrupted a call (the time limit): no catch or finally ran while the call unwound, so the
    // state the call left behind is reset here, and the entity script that was running is named for the host
    // ("" when it was the script itself).
    function afterInterruption() {
        var entity = currentEntity;
        currentEntity = "";
        currentSandbox = "";
        Script.remoteCallerID = undefined;
        parentStack.length = 0;
        return entity;
    }

    function entityHas(payload) {
        var record = entityScripts.get(String(payload.entity));
        return record && typeof record.object[String(payload.method)] === "function" ? 1 : 0;
    }

    function entityCall(payload) {
        var entity = String(payload.entity), method = String(payload.method);
        var record = entityScripts.get(entity);
        if (!record) {
            return undefined;
        }
        var object = record.object;
        if (payload.remoteCallerID) {
            // a call from another client or the server: only what the script lists as remotelyCallable
            var callable = Array.isArray(object.remotelyCallable) && object.remotelyCallable.indexOf(method) >= 0;
            if (!callable) {
                host.log(LOG_PRINT, "Method [ " + method + " ] not remotely callable.");
                return undefined;
            }
        }
        if (typeof object[method] !== "function") {
            return undefined;
        }
        var args = Array.isArray(payload.args) ? payload.args.slice() : [];
        args.unshift(entity);
        var previousCaller = Script.remoteCallerID;
        Script.remoteCallerID = payload.remoteCallerID ? String(payload.remoteCallerID) : undefined;
        try {
            return withEntity(entity, record.url, function () { return object[method].apply(object, args); });
        } catch (e) {
            reportError("in " + method + " of entity script " + record.url + " (" + entity + ")", e);
            return undefined;
        } finally {
            Script.remoteCallerID = previousCaller;
        }
    }
    Object.defineProperty(Script, "remoteCallerID", { value: undefined, writable: true, enumerable: false, configurable: true });

    // --- start and stop -------------------------------------------------------------------------------------------------
    function start() {
        var program = host.call(Script, "scriptSource", []);
        fileName = program.fileName;
        if (!program.source) {
            return; // an entities engine: the scripts arrive through entityLoad
        }
        try {
            host.evaluate(instrument(program.source, fileName), fileName);
        } catch (e) {
            reportError("in " + fileName, e);
        }
    }

    function stop() {
        // the host emitted scriptEnding before this; nothing else to run
        callbacks.clear();
        connections.clear();
    }

    return {
        wrapObject: wrapObject,
        registerCallback: registerCallback,
        onSignal: onSignal,
        onCallback: onCallback,
        start: start,
        stop: stop,
        entityLoad: entityLoad,
        entityUnload: entityUnload,
        entityHas: entityHas,
        entityCall: entityCall,
        afterInterruption: afterInterruption
    };
})
