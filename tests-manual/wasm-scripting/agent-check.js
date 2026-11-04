//
//  agent-check.js
//  tests-manual/wasm-scripting
//
//  The stand-in agent of server-check.sh, a persistent script of the sandbox domain. In its assignment client it
//  calls the APIs an AI
//  resident's body uses, makes entities with server entity scripts (a ticker and a runaway that
//  the time limit must stop alone), and edits through the entity server's edit filter (server-edit-filter.js).
//  Each result is an AGENT-CHECK line in its assignment client's log; the server entity scripts print SERVER-CHECK
//  lines in the entity script server's. Its one HTTP request goes to the sandbox domain-server's own page.
//
//  Distributed under the Apache License, Version 2.0.
//

(function () {
    var PREFIX = "server-check ";
    var onNewEngine = typeof Symbol === "function";  // newer JavaScript runs only there (KNOWN-ISSUES)
    var HTTP_PAGE = "http://127.0.0.1:43100/";
    var ECHO_URL = "http://127.0.0.1:43110/echo";  // http-echo.py, which answers a POST as a bridge would
    var SOUND_URL = Script.resolvePath("../../interface/resources/sounds/Button01.wav");

    function report(name, value) {
        var text;
        try {
            text = JSON.stringify(value);
        } catch (e) {
            text = "unprintable: " + e;
        }
        print("AGENT-CHECK " + name + " = " + text);
    }
    function attempt(name, f) {
        try {
            report(name, f());
        } catch (e) {
            report(name, "THREW " + e);
        }
    }
    function round3(v) {
        return v === undefined || v === null ? v : { x: Math.round(v.x * 1000) / 1000, y: Math.round(v.y * 1000) / 1000,
            z: Math.round(v.z * 1000) / 1000 };
    }

    var tickerScript = "(function () {" +
        "  var self = this, ticks = 0;" +
        "  this.remotelyCallable = ['ping'];" +
        "  this.preload = function (id) {" +
        "    print('SERVER-CHECK ticker preload on ' + (typeof Symbol === 'function' ? 'new' : 'QtScript'));" +
        "    self.timer = Script.setInterval(function () { ticks++; Entities.editEntity(id, { name: '" + PREFIX + "ticker: ' + ticks }); }, 1000);" +
        "  };" +
        "  this.ping = function (id, params) { print('SERVER-CHECK ticker pinged by ' + JSON.stringify(params) + ' at ' + ticks); };" +
        "  this.unload = function () { Script.clearInterval(self.timer); print('SERVER-CHECK ticker unload'); };" +
        "})";
    var runawayScript = "(function () {" +
        "  this.remotelyCallable = ['ping'];" +
        "  this.preload = function (id) {" +
        "    print('SERVER-CHECK runaway armed');" +
        "    Entities.editEntity(id, { name: '" + PREFIX + "runaway: armed' });" +
        "    Script.setTimeout(function () { Entities.editEntity(id, { name: '" + PREFIX + "runaway: spinning' }); print('SERVER-CHECK runaway spinning'); while (true) {} }, 2000);" +
        "  };" +
        "  this.ping = function () { print('SERVER-CHECK runaway pinged, which a stopped script must not be'); };" +
        "  this.unload = function () { print('SERVER-CHECK runaway unload called, which a stopped script must not have'); };" +
        "})";

    report("engine", onNewEngine ? "new" : "QtScript");

    // the agent as an avatar, and its view of the entities
    attempt("Agent.isAvatar", function () { Agent.isAvatar = true; return Agent.isAvatar; });
    attempt("Agent.avatarViewRadius", function () { Agent.avatarViewRadius = 50; return Agent.avatarViewRadius; });
    attempt("Agent.isPlayingAvatarSound", function () { return Agent.isPlayingAvatarSound; });
    attempt("Agent audio properties", function () {
        Agent.isListeningToAudioStream = true;
        return { listening: Agent.isListeningToAudioStream, loudnessType: typeof Agent.lastReceivedAudioLoudness };
    });
    attempt("Avatar setters", function () {
        Avatar.displayName = "server-check agent";
        Avatar.position = { x: 0, y: 1, z: 0 };
        Avatar.orientation = Quat.fromPitchYawRollDegrees(0, 90, 0);
        Avatar.gazeStyle = "calm";
        Avatar.animationFadeSeconds = 0.25;
        return { position: round3(Avatar.position), gazeStyle: Avatar.gazeStyle, fade: Avatar.animationFadeSeconds,
            sessionUUIDType: typeof Avatar.sessionUUID, skeletonModelURLType: typeof Avatar.skeletonModelURL };
    });
    attempt("Avatar.startAnimation", function () {
        Avatar.startAnimation(Script.resolvePath("no-such-animation.fbx"), 30, 1, false, false, 0, 10);
        return "called";
    });
    attempt("EntityViewer", function () {
        EntityViewer.setPosition({ x: 0, y: 0, z: 0 });
        EntityViewer.setCenterRadius(1000);
        EntityViewer.setBoundaryLevelAdjust(0);
        EntityViewer.queryOctree();
        return "queried";
    });
    var queryTimer = Script.setInterval(function () { EntityViewer.queryOctree(); }, 1000);

    // pure functions
    attempt("Vec3 and Quat", function () {
        var q = Quat.fromPitchYawRollDegrees(10, 20, 30);
        var a = { x: 1, y: 2, z: 3 }, b = { x: 4, y: 6, z: 3 };
        return { rotated: round3(Vec3.multiplyQbyV(q, { x: 1, y: 0, z: 0 })), euler: round3(Quat.safeEulerAngles(q)),
            forward: round3(Quat.getForward(q)), sum: Vec3.sum(a, b), subtract: Vec3.subtract(b, a),
            distance: Vec3.distance(a, b), length: Vec3.length(b), normalize: round3(Vec3.normalize(b)),
            multiply: Vec3.multiply(a, 2), dot: Vec3.dot(a, b),
            product: round3(Quat.safeEulerAngles(Quat.multiply(q, Quat.fromPitchYawRollDegrees(0, 90, 0)))) };
    });
    attempt("JSON", function () { return JSON.parse(JSON.stringify({ a: [1, "two", { three: 3 }], b: null })); });
    attempt("Uuid", function () {
        var id = Uuid.generate();
        return { nullType: typeof Uuid.NULL, isNull: Uuid.isNull(Uuid.NULL), generatedType: typeof id,
            generatedLength: String(id).length };
    });
    attempt("globals", function () {
        return { WebSocketServer: typeof WebSocketServer, location: typeof location, XMLHttpRequest: typeof XMLHttpRequest,
            AvatarList: typeof AvatarList, SoundCache: typeof SoundCache, Messages: typeof Messages };
    });
    attempt("AvatarList", function () {
        var ids = AvatarList.getAvatarIdentifiers();
        return { isArray: Array.isArray(ids), count: ids.length };
    });
    var assetAnswer = null;
    attempt("Assets.getAsset", function () {
        Assets.getAsset({ url: "atp:/server-check/no-such-asset.txt", responseType: "text" }, function (error, result) {
            assetAnswer = { errorType: typeof error, hasError: !!error, resultType: typeof result };
        });
        return "asked";
    });

    // an agent's WebSocketServer: a client of the agent's own connects and says ping; the server answers
    var socketTrip = {};
    attempt("WebSocketServer", function () {
        var server = new WebSocketServer();  // port 0: a free one
        server.newConnection.connect(function (client) {
            socketTrip.connected = true;
            client.onmessage = function (event) { client.send("pong to " + event.data); };
        });
        Script.setTimeout(function () {
            var socket = new WebSocket("ws://127.0.0.1:" + server.port);
            socket.onopen = function () { socket.send("ping"); };
            socket.onmessage = function (event) {
                socketTrip.answer = event.data;
                socket.close();
                server.close();
            };
        }, 500);
        return { listening: server.listening, hasPort: server.port > 0, urlType: typeof server.url };
    });

    // events
    var updates = 0;
    Script.update.connect(function () { updates++; });
    var clearing = 0;
    Entities.clearingEntities.connect(function () { clearing++; });
    var messages = [];
    Messages.subscribe("server-check");
    Messages.messageReceived.connect(function (channel, message, sender, localOnly) {
        if (channel === "server-check") {
            messages.push(message);
        }
    });
    Messages.sendMessage("server-check", "hello from the agent");

    // HTTP: text and bytes from the sandbox domain-server's page
    var http = {};
    function get(name, responseType) {
        var request = new XMLHttpRequest();
        request.onreadystatechange = function () {
            if (request.readyState === request.DONE) {
                http[name] = { status: request.status,
                    length: responseType === "arraybuffer" ? (request.response ? request.response.byteLength : null)
                                                           : String(request.responseText).length };
            }
        };
        request.open("GET", HTTP_PAGE);
        if (responseType) {
            request.responseType = responseType;
        }
        request.send();
    }
    get("text");
    get("bytes", "arraybuffer");
    // a POST of JSON, as an AI resident's body sends its bridge
    var post = new XMLHttpRequest();
    post.onreadystatechange = function () {
        if (post.readyState === post.DONE) {
            var answer = null;
            try {
                answer = JSON.parse(post.responseText);
            } catch (e) {
                answer = "unparsable: " + post.responseText;
            }
            http.post = { status: post.status, answer: answer };
        }
    };
    post.open("POST", ECHO_URL);
    post.setRequestHeader("Content-Type", "application/json");
    post.send(JSON.stringify({ heard: "hello", at: 1 }));

    var sound = null;
    attempt("SoundCache.getSound", function () { sound = SoundCache.getSound(SOUND_URL); return typeof sound; });

    function nameOf(id) {
        var properties = Entities.getEntityProperties(id, ["name"]);
        return properties ? properties.name : null;
    }
    function ticksOf(name) {
        var m = /ticker: (\d+)$/.exec(name || "");
        return m ? parseInt(m[1], 10) : -1;
    }

    var ticker, runaway, filtered;
    Script.setTimeout(function () {
        // leftovers of an earlier run
        var near = Entities.findEntities({ x: 0, y: 0, z: 0 }, 1000);
        var removed = 0;
        for (var i = 0; i < near.length; i++) {
            if ((nameOf(near[i]) || "").indexOf(PREFIX) === 0) {
                Entities.deleteEntity(near[i]);
                removed++;
            }
        }
        report("findEntities", { isArray: Array.isArray(near), leftoversRemoved: removed });
        ticker = Entities.addEntity({ type: "Box", name: PREFIX + "ticker: pending", position: { x: 0, y: 1, z: -3 },
            dimensions: { x: 0.5, y: 0.5, z: 0.5 }, serverScripts: tickerScript, lifetime: 300 });
        if (onNewEngine) {
            runaway = Entities.addEntity({ type: "Box", name: PREFIX + "runaway: pending", position: { x: 2, y: 1, z: -3 },
                dimensions: { x: 0.5, y: 0.5, z: 0.5 }, serverScripts: runawayScript, lifetime: 300 });
        }
        filtered = Entities.addEntity({ type: "Box", name: PREFIX + "filtered", position: { x: -2, y: 1, z: -3 },
            dimensions: { x: 0.5, y: 0.5, z: 0.5 }, lifetime: 300 });
        report("addEntity", { ticker: typeof ticker, runaway: typeof runaway, filtered: typeof filtered });
    }, 4000);

    // an assignment client's edits bypass the edit filter (the domain gives it the right to change locks)
    Script.setTimeout(function () {
        Entities.editEntity(filtered, { name: "forbidden" });
        Entities.editEntity(filtered, { position: { x: -2, y: 500, z: -3 } });
    }, 8000);

    // the server entity scripts, called from here
    Script.setTimeout(function () {
        Entities.callEntityServerMethod(ticker, "ping", ["agent"]);
        if (runaway) {
            Entities.callEntityServerMethod(runaway, "ping", ["agent"]);
        }
    }, 20000);

    Script.setTimeout(function () {
        report("ticks at 14 s", ticksOf(nameOf(ticker)));
        if (runaway) {
            report("runaway name at 14 s", nameOf(runaway));
        }
    }, 14000);

    Script.setTimeout(function () {
        report("ticks at 26 s", ticksOf(nameOf(ticker)));
        if (runaway) {
            report("runaway name at 26 s", nameOf(runaway));
        }
        attempt("edit by an assignment client, unfiltered", function () {
            var p = Entities.getEntityProperties(filtered, ["name", "position"]);
            return { name: p.name, y: Math.round(p.position.y) };
        });
        attempt("getMultipleEntityProperties", function () {
            var sets = Entities.getMultipleEntityProperties([ticker, filtered], ["name"]);
            return { isArray: Array.isArray(sets), count: sets.length, second: sets[1] ? sets[1].name : null };
        });
        attempt("takeHeardAudio", function () {
            var heard = Agent.takeHeardAudio();
            return { type: typeof heard, kind: Object.prototype.toString.call(heard),
                byteLength: heard && heard.byteLength !== undefined ? heard.byteLength : null,
                length: heard && heard.length !== undefined ? heard.length : null };
        });
        attempt("sound", function () { return { downloaded: sound ? sound.downloaded : null, duration: sound ? sound.duration : null }; });
        attempt("Agent.playAvatarSound", function () { Agent.playAvatarSound(sound); return "called"; });
        attempt("AvatarList.getAvatar", function () {
            var ids = AvatarList.getAvatarIdentifiers();
            var avatar = AvatarList.getAvatar(ids.length ? ids[0] : Avatar.sessionUUID);
            return { type: typeof avatar, hasPosition: !!(avatar && avatar.position), count: ids.length };
        });
        report("Assets.getAsset's answer", assetAnswer);
        report("WebSocketServer round trip", socketTrip);
        report("updates over 26 s", updates > 600 ? "more than 600" : updates);
        report("clearingEntities", clearing);
        report("messages", messages);
        report("http", http);
    }, 26000);

    // the box filter-client.js edited through the edit filter, as the entity server keeps it
    Script.setTimeout(function () {
        var near = Entities.findEntities({ x: -4, y: 1, z: -3 }, 500);
        var seen = [];
        for (var i = 0; i < near.length; i++) {
            var p = Entities.getEntityProperties(near[i], ["name", "position"]);
            if (p.name === "server-check filter box" || p.name === "forbidden") {
                seen.push({ name: p.name, y: Math.round(p.position.y) });
            }
        }
        report("the filtered box as the server keeps it", seen);
        Script.clearInterval(queryTimer);
        report("done", true);
    }, 45000);
}());
