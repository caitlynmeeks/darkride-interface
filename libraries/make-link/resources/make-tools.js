//
//  make-tools.js
//  libraries/make-link/resources
//
//  Make's building tools, the part that touches the world: look, add, edit, remove, set a script, undo, and test by
//  clicking, calling, walking and watching. The app runs this in an engine of its own under a world's rules (the
//  script API policy), so whatever steers Claude can do at most what a world's script can, with the visitor's own
//  rights in the world they have open. The app checks every call before it comes here (the tool list, its schemas,
//  the person's say), and the picture, the log, Blender and uploads are the app's own.
//
//  The app hands this engine one object of its own, MakeTools: its command signal brings a checked call (an id, the
//  tool, the inputs as JSON, the person's request it belongs to), and done(id, ok, json) answers it once.
//  Every change goes on an undo list, marked with its request, so Make's Undo can take back a whole request.
//
//  From skunkworks' build-with-claude prototype (build-tools.js, 10-30).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    var history = [];        // { op, id, before|props, request }, newest last
    var made = {};           // the ids added through Make in this session (scene's "mine")
    var MAX_HISTORY = 500;

    function r2(n) { return Math.round(n * 100) / 100; }
    function vec(v) { return v ? { x: r2(v.x), y: r2(v.y), z: r2(v.z) } : null; }
    function degrees(q) {
        var e = Quat.safeEulerAngles(q);
        return { x: Math.round(e.x), y: Math.round(e.y), z: Math.round(e.z) };
    }
    function isNull(id) { return !id || id === Uuid.NULL || String(id) === "{00000000-0000-0000-0000-000000000000}"; }
    function exists(id) {
        var p = Entities.getEntityProperties(id, ["type", "entityHostType"]);
        return p && p.type && p.type !== "Unknown" ? p : null;
    }
    function angleBetween(a, b) {
        var dot = Math.abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        return Math.round(2 * Math.acos(Math.min(1, dot)) * 180 / Math.PI);
    }
    function remember(entry) {
        history.push(entry);
        if (history.length > MAX_HISTORY) { history.shift(); }
    }
    // rotationDegrees in place of rotation, objects for userData and materialData
    function convert(props) {
        var out = {}, k;
        for (k in props) {
            if (!props.hasOwnProperty(k)) { continue; }
            if (k === "rotationDegrees") { out.rotation = Quat.fromVec3Degrees(props[k]); }
            else if (k === "localRotationDegrees") { out.localRotation = Quat.fromVec3Degrees(props[k]); }
            else if ((k === "userData" || k === "materialData") && typeof props[k] === "object") { out[k] = JSON.stringify(props[k]); }
            else { out[k] = props[k]; }
        }
        return out;
    }
    function failed(code, message) { return { error: { code: code, message: message } }; }

    // the longest text value Make sets: a longer one is refused with a plain line, never reported as set
    var MAX_VALUE_BYTES = 1200; // the same as MakeLink.cpp MAX_INLINE_VALUE_BYTES
    function utf8Bytes(s) {
        var n = 0;
        for (var i = 0; i < s.length; i++) {
            var c = s.charCodeAt(i);
            if (c < 0x80) { n += 1; }
            else if (c < 0x800) { n += 2; }
            else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) { n += 4; i++; }
            else { n += 3; }
        }
        return n;
    }
    // a value longer than Make sets (put there some other way): Make won't change or remove a thing holding one, since
    // its Undo couldn't put it back (every change undoable)
    function cantPutBack(props) {
        for (var k in props) {
            if (props.hasOwnProperty(k) && typeof props[k] === "string" && props[k].length > MAX_VALUE_BYTES / 4 &&
                utf8Bytes(props[k]) > MAX_VALUE_BYTES) {
                return k + " holds " + utf8Bytes(props[k]) + " bytes, more than Undo could put back";
            }
        }
        return null;
    }
    function tooLong(props) {
        for (var k in props) {
            if (props.hasOwnProperty(k) && typeof props[k] === "string" && props[k].length > MAX_VALUE_BYTES / 4) {
                var bytes = utf8Bytes(props[k]);
                if (bytes > MAX_VALUE_BYTES) {
                    return failed("bad_input", k + " is " + bytes + " bytes; a value can be at most " + MAX_VALUE_BYTES +
                        " bytes, or the world won't keep it. Put long data a script needs in the script itself" +
                        " (set_script keeps scripts in the world's files).");
                }
            }
        }
        return null;
    }

    var BASE = ["name", "description", "type", "position", "rotation", "dimensions", "parentID", "localPosition",
        "visible", "collisionless", "dynamic", "gravity", "velocity", "angularVelocity", "locked", "userData", "lifetime"];
    var BY_TYPE = {
        Box: ["color", "alpha"], Sphere: ["color", "alpha"], Shape: ["shape", "color", "alpha"],
        Model: ["modelURL", "textures", "animation", "shapeType"],
        Text: ["text", "lineHeight", "textColor", "textAlpha", "backgroundColor", "backgroundAlpha", "billboardMode", "unlit"],
        Light: ["color", "intensity", "falloffRadius", "isSpotlight", "exponent", "cutoff"],
        Zone: ["shapeType", "keyLightMode", "keyLight", "ambientLightMode", "skyboxMode", "skybox"],
        Image: ["imageURL", "emissive", "color", "alpha"], Web: ["sourceUrl", "dpi"],
        ParticleEffect: ["textures", "emitRate", "lifespan", "maxParticles", "particleRadius", "color", "emitSpeed"],
        Material: ["materialURL", "materialData", "parentMaterialName", "priority"]
    };

    function brief(id, here) {
        var p = Entities.getEntityProperties(id, ["name", "type", "position", "rotation", "dimensions", "parentID",
            "script", "serverScripts", "locked", "entityHostType"]);
        if (!p || !p.type || p.entityHostType !== "domain") { return null; }
        var thing = { id: id, name: p.name || "", type: p.type, position: vec(p.position), dimensions: vec(p.dimensions),
            rotationDegrees: degrees(p.rotation), parentID: isNull(p.parentID) ? null : p.parentID,
            scripts: { client: !!p.script, server: !!p.serverScripts }, mine: !!made[id] };
        if (p.locked) { thing.locked = true; }
        if (here) { thing.distance = r2(Vec3.distance(here, p.position)); }
        return thing;
    }

    // who may change (and so read) this thing's server script: editor-ui's one rule (EntityScriptRights, the entity
    // server's), through the app's object, so Make and the Code panel agree (security should-fix 3)
    function mayEditScripts(p) { return MakeTools.mayChangeScriptsOf(!!p.locked); }

    // a script's text: inline code as it is, a script in the world's own files fetched; one from the web is named only
    function scriptText(value, done) {
        if (!value) { return done(null); }
        if (/^atp:/.test(value)) {
            Assets.getAsset({ url: value, responseType: "text" }, function (error, result) {
                done(error ? "(a script in the world's files that couldn't be read: " + error + ")"
                    : String((result && (result.response || result.data)) || ""));
            });
            return;
        }
        if (/^(https?|file):/i.test(value)) { return done("(a script served from " + value + ", not fetched)"); }
        done(value);
    }

    var TOOLS = {
        scene: function (a, request, done) {
            var radius = a.radius === undefined ? 20 : a.radius, name = (a.name || "").toLowerCase();
            var here = MyAvatar.position, things = [];
            Entities.findEntities(here, radius).forEach(function (id) {
                var t = brief(id, here);
                if (t && (!name || t.name.toLowerCase().indexOf(name) >= 0)) { things.push(t); }
            });
            things.sort(function (x, y) { return x.distance - y.distance; });
            done({ here: { position: vec(here), yawDegrees: degrees(MyAvatar.orientation).y,
                canBuild: Entities.canRez(), canUnlock: Entities.canAdjustLocks() },
                things: things.slice(0, 80), more: Math.max(0, things.length - 80) });
        },
        get: function (a, request, done) {
            var t = exists(a.id);
            if (!t) { return done(failed("not_found", "no thing " + a.id)); }
            var keys = BASE.concat(BY_TYPE[t.type] || [], a.props || []).filter(function (k) {
                return k !== "script" && k !== "serverScripts";
            });
            var p = Entities.getEntityProperties(a.id, keys.concat(["script", "serverScripts", "locked"])), properties = {};
            keys.forEach(function (k) {
                var v = p[k];
                if (v === undefined || v === "" || (k === "parentID" && isNull(v))) { return; }
                if (k === "rotation") { properties.rotationDegrees = degrees(v); }
                else { properties[k] = v; }
            });
            scriptText(p.script, function (client) {
                if (p.serverScripts && !mayEditScripts(p)) {
                    // it has one: say so, never its text or its address
                    return done({ id: a.id, properties: properties, mine: !!made[a.id], scripts: { client: client,
                        server: "(a server script, shown only to people who can edit this thing)" } });
                }
                scriptText(p.serverScripts, function (server) {
                    done({ id: a.id, properties: properties, scripts: { client: client, server: server }, mine: !!made[a.id] });
                });
            });
        },
        add: function (a, request, done) {
            var props = convert(a.properties || {});
            var long = tooLong(props);
            if (long) { return done(long); }
            if (!props.position && isNull(props.parentID)) {
                props.position = Vec3.sum(MyAvatar.position, Vec3.multiply(2, Quat.getFront(MyAvatar.orientation)));
            }
            var id = Entities.addEntity(props);
            if (isNull(id)) { return done(failed("refused", "couldn't add it: building isn't allowed here, or the type is wrong")); }
            made[id] = true;
            remember({ op: "add", id: id, request: request });
            done({ id: id });
        },
        edit: function (a, request, done) {
            var props = convert(a.properties || {}), keys = Object.keys(props);
            var long = tooLong(props);
            if (long) { return done(long); }
            var before = Entities.getEntityProperties(a.id, keys.concat(["locked", "type"]));
            if (!before || !before.type || before.type === "Unknown") { return done(failed("not_found", "no thing " + a.id)); }
            if (before.locked) { return done(failed("refused", "it's locked: unlocking is for the world's own editors")); }
            var old = {};
            keys.forEach(function (k) { old[k] = before[k]; });
            var stuck = cantPutBack(old);
            if (stuck) { return done(failed("refused", "Make won't change this: " + stuck + ".")); }
            Entities.editEntity(a.id, props);
            remember({ op: "edit", id: a.id, before: old, request: request });
            Script.setTimeout(function () {
                var after = Entities.getEntityProperties(a.id, keys), out = {};
                keys.forEach(function (k) { out[k] = k === "rotation" ? degrees(after[k]) : after[k]; });
                done({ id: a.id, properties: out });
            }, 200);
        },
        remove: function (a, request, done) {
            var before = Entities.getEntityProperties(a.id);
            if (!before || !before.type || before.type === "Unknown") { return done(failed("not_found", "no thing " + a.id)); }
            if (before.locked) { return done(failed("refused", "it's locked: unlocking is for the world's own editors")); }
            var stuck = cantPutBack(before);
            if (stuck) { return done(failed("refused", "Make won't remove this: " + stuck + ".")); }
            Entities.deleteEntity(a.id);
            remember({ op: "remove", id: a.id, props: before, request: request });
            done({ id: a.id, removed: true });
        },
        // the app saved the script in the world's files and kept a readable copy; this puts it into the thing
        set_script: function (a, request, done) {
            var t = exists(a.id);
            if (!t) { return done(failed("not_found", "no thing " + a.id)); }
            var key = a.side === "server" ? "serverScripts" : "script", props = {};
            props[key] = a.url || "";
            if (key === "script") { props.scriptTimestamp = Date.now(); }
            var before = Entities.getEntityProperties(a.id, [key, "locked"]), old = {};
            if (before.locked) { return done(failed("refused", "it's locked: unlocking is for the world's own editors")); }
            old[key] = before[key];
            Entities.editEntity(a.id, props);
            remember({ op: "edit", id: a.id, before: old, request: request, tool: "set_script" });
            done({ id: a.id, side: a.side, url: a.url || "" });
        },
        undo: function (a, request, done) {
            var steps = a.steps || 1, undone = [];
            while (steps-- > 0 && history.length) {
                var entry = history.pop(), result = undoOne(entry);
                undone.push(result);
                if (result.notUndone) { history.push(entry); break; }
            }
            Script.setTimeout(function () { done({ undone: undone }); }, 300);
        },
        // Make's Undo: every change of one request, newest first
        undo_request: function (a, request, done) {
            var undone = [];
            for (var i = history.length - 1; i >= 0; i--) {
                if (history[i].request === a.request) {
                    var result = undoOne(history[i]);
                    undone.push(result);
                    if (result.notUndone) { break; }
                    history.splice(i, 1);
                }
            }
            Script.setTimeout(function () { done({ undone: undone }); }, 300);
        },
        click: function (a, request, done) {
            var p = Entities.getEntityProperties(a.id, ["position", "type"]);
            if (!p || !p.position || p.type === "Unknown") { return done(failed("not_found", "no thing " + a.id)); }
            var ev = { id: 1, pos2D: { x: 0, y: 0 }, pos3D: p.position, normal: { x: 0, y: 0, z: 1 },
                direction: Vec3.normalize(Vec3.subtract(p.position, MyAvatar.position)), button: "Primary", isPrimaryHeld: true };
            Entities.sendMousePressOnEntity(a.id, ev);
            Entities.sendClickDownOnEntity(a.id, ev);
            Script.setTimeout(function () {
                ev.isPrimaryHeld = false;
                Entities.sendClickReleaseOnEntity(a.id, ev);
                Entities.sendMouseReleaseOnEntity(a.id, ev);
                done({ id: a.id, clicked: true });
            }, 150);
        },
        call: function (a, request, done) {
            if (!exists(a.id)) { return done(failed("not_found", "no thing " + a.id)); }
            var params = (a.params || []).map(String);
            if (a.side === "server") { Entities.callEntityServerMethod(a.id, a.method, params); }
            else { Entities.callEntityMethod(a.id, a.method, params); }
            done({ sent: true });
        },
        walk: function (a, request, done) {
            var target = null, to;
            if (a.id) {
                var p = Entities.getEntityProperties(a.id, ["position", "dimensions", "type"]);
                if (!p || !p.position || p.type === "Unknown") { return done(failed("not_found", "no thing " + a.id)); }
                target = p.position;
                if (a.onto) {
                    to = p.type === "Zone" ? { x: target.x, y: target.y - p.dimensions.y / 2 + 1.0, z: target.z }
                        : { x: target.x, y: target.y + p.dimensions.y / 2 + 1.0, z: target.z };
                } else {
                    var away = Vec3.subtract(MyAvatar.position, target);
                    away.y = 0;
                    if (Vec3.length(away) < 0.01) { away = { x: 0, y: 0, z: 1 }; }
                    away = Vec3.normalize(away);
                    to = Vec3.sum(target, Vec3.multiply(Math.max(p.dimensions.x, p.dimensions.z) / 2 + 1.5, away));
                    to.y = MyAvatar.position.y;
                }
            } else if (a.position) {
                to = a.position;
            } else {
                return done(failed("bad_input", "walk needs an id or a position"));
            }
            MyAvatar.position = to;
            if (target) {
                var d = Vec3.subtract(target, to);
                MyAvatar.orientation = Quat.fromPitchYawRollDegrees(0, Math.atan2(-d.x, -d.z) * 180 / Math.PI, 0);
            }
            Script.setTimeout(function () {
                done({ position: vec(MyAvatar.position), arrived: Vec3.distance(MyAvatar.position, to) < 1.0 });
            }, 600);
        },
        watch: function (a, request, done) {
            var ids = a.ids || [], seconds = a.seconds === undefined ? 3 : a.seconds;
            var KEYS = ["position", "rotation", "dimensions", "color", "visible", "text"];
            var start = {}, peak = {};
            for (var i = 0; i < ids.length; i++) {
                start[ids[i]] = Entities.getEntityProperties(ids[i], KEYS.concat(["type"]));
                if (!start[ids[i]] || !start[ids[i]].position || start[ids[i]].type === "Unknown") {
                    return done(failed("not_found", "no thing " + ids[i]));
                }
                peak[ids[i]] = { moved: 0, turned: 0 };
            }
            var timer = Script.setInterval(function () {
                ids.forEach(function (id) {
                    var p = Entities.getEntityProperties(id, ["position", "rotation"]);
                    if (!p || !p.position) { return; }
                    peak[id].moved = Math.max(peak[id].moved, Vec3.distance(p.position, start[id].position));
                    peak[id].turned = Math.max(peak[id].turned, angleBetween(p.rotation, start[id].rotation));
                });
            }, 100);
            Script.setTimeout(function () {
                Script.clearInterval(timer);
                var changes = ids.map(function (id) {
                    var s = start[id], e = Entities.getEntityProperties(id, KEYS);
                    if (!e || !e.position) { return { id: id, gone: true }; }
                    return { id: id, moved: peak[id].moved > 0.01, movedMostMetres: r2(peak[id].moved),
                        turned: peak[id].turned > 0, turnedMostDegrees: peak[id].turned,
                        resized: JSON.stringify(s.dimensions) !== JSON.stringify(e.dimensions),
                        recoloured: JSON.stringify(s.color) !== JSON.stringify(e.color),
                        text: s.text !== e.text ? e.text : null, visible: e.visible };
                });
                done({ changes: changes, seconds: seconds });
            }, seconds * 1000);
        }
    };

    function undoOne(entry) {
        var stuck = entry.op === "edit" ? cantPutBack(entry.before) : entry.op === "remove" ? cantPutBack(entry.props) : null;
        if (stuck) {
            return { tool: entry.tool || entry.op, id: entry.id, notUndone: "not undone, and the change is still there: " +
                stuck + ". Tell the person; nothing after it was undone either." };
        }
        if (entry.op === "add") {
            Entities.deleteEntity(entry.id);
            delete made[entry.id];
            return { tool: "add", id: entry.id };
        }
        if (entry.op === "edit") {
            if (entry.before.script !== undefined) { entry.before.scriptTimestamp = Date.now(); }
            Entities.editEntity(entry.id, entry.before);
            return { tool: entry.tool || "edit", id: entry.id };
        }
        var props = entry.props;
        ["id", "created", "lastEdited", "lastEditedBy", "age", "ageAsText", "entityHostType", "owningAvatarID",
            "queryAACube", "boundingBox", "renderInfo", "clientOnly", "localEntity", "avatarEntity"].forEach(function (k) {
            delete props[k];
        });
        var back = Entities.addEntity(props);
        return { tool: "remove", id: entry.id, broughtBackAs: back };
    }

    MakeTools.command.connect(function (id, tool, inputs, request) {
        var answered = false;
        function done(result) {
            if (answered) { return; }
            answered = true;
            var ok = !(result && result.error);
            MakeTools.done(id, ok, JSON.stringify(ok ? result : result.error));
        }
        var run = TOOLS[tool];
        if (!run) { return done(failed("refused", "no tool " + tool + " in this engine")); }
        try {
            run(JSON.parse(inputs), request, done);
        } catch (e) {
            done(failed("bad_input", String(e)));
        }
    });
    MakeTools.ready();
}());
