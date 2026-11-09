//
//  parity.js
//  tests-manual/wasm-scripting
//
//  Round 3 of WASM-SCRIPTING.md: one PARITY line per check, diffed against the recordings (this engine's own,
//  and the QtScript engine's from before its removal). Every line is a value a script could depend on. Lines
//  that must differ by nature (timings, generated ids) are not here.
//
//  Run serverless:  tests-manual/wasm-scripting/parity.sh [timeout seconds]
//
//  Written in ES5 on purpose.
//
//  Distributed under the Apache License, Version 2.0.
//

(function () {
    var ENTITY_POSITION = { x: 0, y: 2, z: -3 };
    var lines = [];

    // fetched now, read in run(): the types that cross with a conversion of their own (WASM-SCRIPTING.md round 5)
    var animation = AnimationCache.getAnimation(Paths.defaultScripts + "/system/assets/animations/Clapping.fbx");
    var sound = SoundCache.getSound(Paths.defaultScripts + "/tivoli/assets/chat1.wav");
    var clicks = [];
    var assetOutcomes = [];
    var statusAnswers = [];
    var metadataAnswers = [];
    Entities.clickDownOnEntity.connect(function (entityID, event) {
        clicks.push({ entityID: entityID, event: event });
    });

    function check(name, value) {
        var text;
        try {
            text = typeof value === "string" ? JSON.stringify(value) : JSON.stringify(value);
            if (text === undefined) { text = String(value); }
        } catch (e) {
            text = "<" + e + ">";
        }
        lines.push(name + " = " + text);
    }
    function attempt(name, fn) {
        try {
            check(name, fn());
        } catch (e) {
            check(name, "throws " + (e && e.name ? e.name : String(e)));
        }
    }

    var modelEntity;

    function run() {
        // a model for the Graphics API, read once it has loaded (below)
        modelEntity = Entities.addEntity({
            type: "Model", name: "wasm-parity-model", modelURL: Paths.defaultScripts + "/system/assets/models/black-sphere.fbx",
            position: { x: 0, y: 2, z: -5 }, dimensions: { x: 0.5, y: 0.5, z: 0.5 }, lifetime: 60
        });
        var id = Entities.addEntity({
            type: "Box", name: "wasm-parity", position: ENTITY_POSITION, dimensions: { x: 0.5, y: 0.5, z: 0.5 },
            color: { red: 200, green: 120, blue: 40 }, lifetime: 600, userData: JSON.stringify({ a: 1 })
        });
        check("entity id is a string", typeof id === "string");
        check("entity id braces", /^\{[0-9a-f-]{36}\}$/.test(id));
        attempt("getEntityProperties(id).name", function () { return Entities.getEntityProperties(id).name; });
        // the full map and a group, keys unsorted: the order a script sees them in
        attempt("getEntityProperties(id) keys in order", function () { return Object.keys(Entities.getEntityProperties(id)); });
        attempt("getEntityProperties(id).grab keys in order", function () { return Object.keys(Entities.getEntityProperties(id).grab); });
        attempt("getEntityProperties(id, [position]) keys", function () { return Object.keys(Entities.getEntityProperties(id, ["position"])).sort(); });
        attempt("getEntityProperties(id, position) keys", function () { return Object.keys(Entities.getEntityProperties(id, "position")).sort(); });
        // a group's name alone asks for the whole group, alone or among other names (vehicles.js asks for
        // ["vehicle", "parentID", ...]; the new engine dropped the group until 2026-10-25)
        attempt("getEntityProperties(id, [grab]) keys", function () { return Object.keys(Entities.getEntityProperties(id, ["grab"])).sort(); });
        attempt("getEntityProperties(id, [grab, position]) keys", function () { return Object.keys(Entities.getEntityProperties(id, ["grab", "position"])).sort(); });
        attempt("getEntityProperties(id, grab).grab keys", function () { return Object.keys(Entities.getEntityProperties(id, "grab").grab || {}).sort(); });
        attempt("getEntityProperties position", function () { return Entities.getEntityProperties(id, ["position"]).position; });
        attempt("getEntityProperties color", function () { return Entities.getEntityProperties(id, ["color"]).color; });
        attempt("getEntityProperties dimensions type", function () { var d = Entities.getEntityProperties(id, ["dimensions"]).dimensions; return [typeof d.x, d.x]; });
        attempt("getEntityProperties rotation", function () { return Entities.getEntityProperties(id, ["rotation"]).rotation; });
        attempt("getEntityProperties userData", function () { return Entities.getEntityProperties(id, ["userData"]).userData; });
        attempt("getEntityProperties id equals", function () { return Entities.getEntityProperties(id, ["id"]).id === id; });
        attempt("getEntityProperties missing", function () { var p = Entities.getEntityProperties("{00000000-0000-0000-0000-000000000000}"); return [typeof p, Object.keys(p).length]; });
        attempt("editEntity returns", function () { return typeof Entities.editEntity(id, { name: "wasm-parity-2" }); });
        attempt("edited name", function () { return Entities.getEntityProperties(id, ["name"]).name; });
        attempt("findEntities", function () { return Entities.findEntities(ENTITY_POSITION, 1).indexOf(id) >= 0; });
        attempt("findEntitiesByName", function () { return Entities.findEntitiesByName("wasm-parity-2", ENTITY_POSITION, 2).length; });
        attempt("getChildrenIDs", function () { return Entities.getChildrenIDs(id); });
        attempt("Entities.canRez type", function () { return typeof Entities.canRez(); });
        attempt("Entities.getEntityType", function () { return Entities.getEntityType(id); });
        attempt("MyAvatar.position shape", function () { var p = MyAvatar.position; return Object.keys(p).sort(); });
        attempt("MyAvatar.orientation shape", function () { var q = MyAvatar.orientation; return Object.keys(q).sort(); });
        attempt("MyAvatar.sessionUUID type", function () { return typeof MyAvatar.sessionUUID; });
        attempt("MyAvatar.scale", function () { return MyAvatar.scale; });
        attempt("MyAvatar.getJointNames count>0", function () { return MyAvatar.getJointNames().length > 0; });
        attempt("MyAvatar.getJointIndex Head", function () { return typeof MyAvatar.getJointIndex("Head"); });
        attempt("MyAvatar.getAvatarEntitiesVariant", function () { return Array.isArray(MyAvatar.getAvatarEntitiesVariant()); });
        attempt("MyAvatar set position", function () { var p = MyAvatar.position; MyAvatar.position = { x: p.x + 1, y: p.y, z: p.z }; var q = MyAvatar.position; MyAvatar.position = p; return Math.round((q.x - p.x) * 1000); });
        attempt("Vec3.sum", function () { return Vec3.sum({ x: 1, y: 2, z: 3 }, { x: 0.5, y: 0.5, z: 0.5 }); });
        attempt("Vec3.multiply number", function () { return Vec3.multiply({ x: 1, y: 2, z: 3 }, 2); });
        attempt("Vec3.multiply swapped", function () { return Vec3.multiply(2, { x: 1, y: 2, z: 3 }); });
        attempt("Vec3.length", function () { return Vec3.length({ x: 3, y: 4, z: 12 }); });
        attempt("Vec3.ZERO", function () { return Vec3.ZERO; });
        attempt("Vec3.UNIT_Y", function () { return Vec3.UNIT_Y; });
        attempt("Vec3.toString", function () { return Vec3.toString({ x: 1, y: 2, z: 3 }); });
        attempt("Vec3.equal", function () { return Vec3.equal({ x: 1, y: 2, z: 3 }, { x: 1, y: 2, z: 3 }); });
        attempt("Vec3 from array", function () { return Vec3.sum([1, 2, 3], { x: 0, y: 0, z: 0 }); });
        attempt("Vec3 from number", function () { return Vec3.sum(1, { x: 0, y: 0, z: 0 }); });
        attempt("Vec3 from color", function () { return Vec3.sum({ red: 1, green: 2, blue: 3 }, { x: 0, y: 0, z: 0 }); });
        attempt("Quat.fromPitchYawRollDegrees", function () { var q = Quat.fromPitchYawRollDegrees(0, 90, 0); return [Math.round(q.y * 1000), Math.round(q.w * 1000)]; });
        attempt("Quat.safeEulerAngles", function () { var e = Quat.safeEulerAngles(Quat.fromPitchYawRollDegrees(10, 20, 30)); return [Math.round(e.x), Math.round(e.y), Math.round(e.z)]; });
        attempt("Quat.IDENTITY", function () { return Quat.IDENTITY; });
        attempt("Quat.getFront", function () { return Quat.getFront(Quat.IDENTITY); });
        attempt("Mat4.createFromScaleRotAndTrans keys", function () { return Object.keys(Mat4.createFromScaleRotAndTrans({ x: 1, y: 1, z: 1 }, Quat.IDENTITY, { x: 1, y: 2, z: 3 })).length; });
        attempt("Mat4.extractTranslation", function () { return Mat4.extractTranslation(Mat4.createFromScaleRotAndTrans({ x: 1, y: 1, z: 1 }, Quat.IDENTITY, { x: 1, y: 2, z: 3 })); });
        attempt("Uuid.NULL", function () { return Uuid.NULL; });
        attempt("Uuid.isNull(NULL)", function () { return Uuid.isNull(Uuid.NULL); });
        attempt("Uuid.isEqual", function () { return Uuid.isEqual(id, id); });
        attempt("Uuid.fromString", function () { return Uuid.fromString("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}"); });
        attempt("Uuid.generate shape", function () { return /^\{[0-9a-f-]{36}\}$/.test(String(Uuid.generate())); });
        attempt("Script.resolvePath", function () { return String(Script.resolvePath("x/y.js")).replace(/^.*wasm-scripting\//, ""); });
        attempt("Script.resolvePath absolute", function () { return String(Script.resolvePath("http://example.com/a.js")); });
        attempt("Script.context", function () { return Script.context; });
        attempt("Script.type", function () { return Script.type; });
        attempt("Script.isClientScript", function () { return Script.isClientScript(); });
        attempt("Script.fileName tail", function () { return String(Script.fileName).replace(/^.*\//, ""); });
        attempt("Script.require system module", function () { return typeof Script.require("appUi"); });
        attempt("Script.require.resolve bad", function () { try { Script.require.resolve("nothing-here"); return "ok"; } catch (e) { return e.name; } });
        attempt("Script.generateUUID shape", function () { return /^\{[0-9a-f-]{36}\}$/.test(String(Script.generateUUID())); });
        attempt("Script.setTimeout returns", function () { var t = Script.setTimeout(function () {}, 1000); var k = typeof t; Script.clearTimeout(t); return k === "number" || k === "object"; });
        attempt("Script.include missing", function () { Script.include("does-not-exist.js"); return "no throw"; });
        attempt("Settings round trip", function () { Settings.setValue("wasm-parity/int", 3); Settings.setValue("wasm-parity/float", 2.5); Settings.setValue("wasm-parity/obj", { a: [1, "b", true] }); return [Settings.getValue("wasm-parity/int"), Settings.getValue("wasm-parity/float"), Settings.getValue("wasm-parity/obj"), Settings.getValue("wasm-parity/none", "dflt")]; });
        attempt("Settings.getValue missing", function () { return Settings.getValue("wasm-parity/missing"); });
        attempt("location.hostname", function () { return typeof location.hostname; });
        attempt("location.href type", function () { return typeof location.href; });
        attempt("Window.innerWidth type", function () { return typeof Window.innerWidth; });
        attempt("Window.location is location", function () { return Window.location === location || Window.location == location; });
        attempt("Camera.mode", function () { return Camera.mode; });
        attempt("Camera.position shape", function () { return Object.keys(Camera.position).sort(); });
        attempt("HMD.active", function () { return HMD.active; });
        attempt("Controller.Standard.LX type", function () { return typeof Controller.Standard.LX; });
        attempt("Controller.Hardware keys>0", function () { return Object.keys(Controller.Hardware).length > 0; });
        attempt("Controller.getValue", function () { return typeof Controller.getValue(Controller.Standard.LX); });
        attempt("Controller.Actions.Yaw type", function () { return typeof Controller.Actions.Yaw; });
        attempt("Controller.newMapping type", function () { var m = Controller.newMapping("wasm-parity-" + Math.random()); return typeof m; });
        attempt("Menu.isOptionChecked", function () { return typeof Menu.isOptionChecked("Verbose Logging"); });
        attempt("Menu.menuExists", function () { return Menu.menuExists("Developer"); });
        attempt("Tablet.getTablet", function () { var t = Tablet.getTablet("com.highfidelity.interface.tablet.system"); return [typeof t, t === null]; });
        attempt("Tablet button", function () { var t = Tablet.getTablet("com.highfidelity.interface.tablet.system"); var b = t.addButton({ text: "PARITY" }); var r = [typeof b, b ? typeof b.clicked : "none"]; if (b) { t.removeButton(b); } return r; });
        attempt("Overlays.addOverlay", function () { var o = Overlays.addOverlay("cube", { position: ENTITY_POSITION, size: 0.1, color: { red: 0, green: 255, blue: 0 } }); var props = Overlays.getProperty(o, "size"); Overlays.deleteOverlay(o); return [typeof o, props]; });
        attempt("Overlays.getProperties", function () { var o = Overlays.addOverlay("text3d", { position: ENTITY_POSITION, text: "hi" }); var p = Overlays.getProperties(o, ["text", "position"]); Overlays.deleteOverlay(o); return [p.text, Object.keys(p).sort()]; });
        attempt("AvatarList.getAvatarIdentifiers", function () { return Array.isArray(AvatarList.getAvatarIdentifiers()); });
        attempt("AvatarList.getAvatar(me) position", function () { var a = AvatarList.getAvatar(MyAvatar.sessionUUID); return a ? Object.keys(a.position).sort() : "null"; });
        attempt("AvatarManager.getAvatar(me) type", function () { return typeof AvatarManager.getAvatar(MyAvatar.sessionUUID); });
        attempt("Audio.muted type", function () { return typeof Audio.muted; });
        attempt("SoundCache.getSound type", function () { var s = SoundCache.getSound(Script.resolvePath("nothing.wav")); return [typeof s, s ? typeof s.downloaded : "none"]; });
        attempt("Messages.subscribe", function () { Messages.subscribe("wasm-parity"); Messages.sendMessage("wasm-parity", "x"); Messages.unsubscribe("wasm-parity"); return "ok"; });
        attempt("Account.loggedIn type", function () { return typeof Account.loggedIn; });
        attempt("Account.username type", function () { return typeof Account.username; });
        attempt("Paths.resources type", function () { return typeof Paths.resources; });
        attempt("Render.getConfig type", function () { return typeof Render.getConfig("RenderMainView"); });
        attempt("Picks.createPick", function () { var p = Picks.createPick(PickType.Ray, { joint: "Mouse", filter: Picks.PICK_ENTITIES, enabled: true }); var r = Picks.getPrevPickResult(p); Picks.removePick(p); return [typeof p, typeof r, typeof r.type]; });
        attempt("Pointers.createPointer", function () { var p = Pointers.createPointer(PickType.Ray, { joint: "Mouse", filter: Picks.PICK_ENTITIES, enabled: false, renderStates: [] }); Pointers.removePointer(p); return typeof p; });
        attempt("Keyboard.raised type", function () { return typeof Keyboard.raised; });
        attempt("Desktop.width type", function () { return typeof Desktop.width; });
        attempt("Reticle.position shape", function () { return Object.keys(Reticle.position).sort(); });
        attempt("Clipboard.getContentsDimensions", function () { return Object.keys(Clipboard.getContentsDimensions()).sort(); });
        attempt("Graphics.getModel null", function () { return Graphics.getModel(id) === null || typeof Graphics.getModel(id); });
        attempt("Users.getCanKick", function () { return typeof Users.getCanKick(); });
        attempt("Stats.fps type", function () { return typeof Stats.fps; });
        attempt("Snapshot type", function () { return typeof Snapshot; });
        attempt("typeof XMLHttpRequest", function () { return typeof XMLHttpRequest; });
        attempt("typeof WebSocket", function () { return typeof WebSocket; });
        attempt("typeof request", function () { return typeof request; });
        attempt("typeof print", function () { return typeof print; });
        attempt("console.log type", function () { return typeof console.log; });
        attempt("typeof Resource", function () { return typeof Resource; });
        attempt("typeof AudioEffectOptions", function () { return typeof AudioEffectOptions; });
        attempt("Resource.State values", function () { var s = Resource.State; return [s.QUEUED, s.LOADING, s.LOADED, s.FINISHED, s.FAILED]; });
        attempt("new AudioEffectOptions", function () { var o = new AudioEffectOptions({ bandwidth: 5000, reverbTime: 3, roomSize: "big", unknown: 1 }); return [typeof o, o.bandwidth, o.reverbTime, o.roomSize, o.wetDryMix, o.unknown]; });
        attempt("new AudioEffectOptions defaults", function () { var o = new AudioEffectOptions(); return [o.bandwidth, o.preDelay, o.bassMult, o.highGain]; });
        attempt("Audio.setReverbOptions", function () { Audio.setReverbOptions(new AudioEffectOptions({ reverbTime: 1 })); return "ok"; });
        attempt("HMD.getHUDLookAtPosition2D", function () { return HMD.getHUDLookAtPosition2D(); });
        attempt("HMD.getHUDLookAtPosition3D", function () { return HMD.getHUDLookAtPosition3D(); });
        attempt("typeof OverlayWebWindow", function () { return typeof OverlayWebWindow; });
        attempt("typeof QmlFragment", function () { return typeof QmlFragment; });
        attempt("DriveKeys.TRANSLATE_X", function () { return DriveKeys.TRANSLATE_X; });
        attempt("PickType keys", function () { return Object.keys(PickType).sort(); });
        attempt("JSON.stringify(Vec3.ZERO)", function () { return JSON.stringify(Vec3.ZERO); });
        attempt("JSON.stringify(Script) kind", function () { var s = JSON.stringify(Script); return s === "{}" ? "{}" : (s.length > 2 ? "non-empty" : s); });
        attempt("Object.keys(Vec3) count>0", function () { return Object.keys(Vec3).length > 0; });
        attempt("for-in over MyAvatar count>0", function () { var n = 0, k; for (k in MyAvatar) { n++; } return n > 0; });
        attempt("MyAvatar has function sessionUUID?", function () { return typeof MyAvatar.sessionUUID; });
        attempt("Entities.getEntityProperties no args", function () { return typeof Entities.getEntityProperties(); });
        attempt("Entities.addEntity bad", function () { return Entities.addEntity({}); });
        attempt("Entities.getEntityProperties(undefined)", function () { return Object.keys(Entities.getEntityProperties(undefined)).length; });
        attempt("Entities.editEntity bad id", function () { return Entities.editEntity("not-a-uuid", { name: "x" }); });
        attempt("Entities.getEntityProperties(id).script", function () { return Entities.getEntityProperties(id, ["script"]).script; });
        attempt("Entities.getEntityProperties(id).created type", function () { return typeof Entities.getEntityProperties(id, ["created"]).created; });
        attempt("Entities.getEntityProperties(id).lastEdited type", function () { return typeof Entities.getEntityProperties(id, ["lastEdited"]).lastEdited; });
        attempt("Entities.getEntityProperties(id).visible", function () { return Entities.getEntityProperties(id, ["visible"]).visible; });
        attempt("Entities.getEntityProperties(id).parentID", function () { return Entities.getEntityProperties(id, ["parentID"]).parentID; });
        attempt("Entities.getEntityProperties(id).boundingBox keys", function () { return Object.keys(Entities.getEntityProperties(id, ["boundingBox"]).boundingBox).sort(); });
        attempt("Entities.getEntityProperties(id).velocity", function () { return Entities.getEntityProperties(id, ["velocity"]).velocity; });
        attempt("Entities.getEntityProperties(id).localPosition", function () { return Entities.getEntityProperties(id, ["localPosition"]).localPosition; });
        attempt("Entities.getEntityProperties(id).grab keys", function () { var g = Entities.getEntityProperties(id, ["grab"]).grab; return g ? Object.keys(g).sort() : g; });
        attempt("Entities.getEntityProperties(id).color type", function () { return typeof Entities.getEntityProperties(id, ["color"]).color.red; });
        attempt("Entities.getEntityProperties(id).renderInfo", function () { return typeof Entities.getEntityProperties(id, ["renderInfo"]).renderInfo; });
        attempt("Entities.getEntityProperties(id).clientOnly", function () { return Entities.getEntityProperties(id, ["clientOnly"]).clientOnly; });
        attempt("Entities.addEntity avatar entity", function () { var e = Entities.addEntity({ type: "Sphere", position: ENTITY_POSITION, dimensions: { x: 0.1, y: 0.1, z: 0.1 }, lifetime: 60 }, "avatar"); var p = Entities.getEntityProperties(e, ["entityHostType", "clientOnly"]); Entities.deleteEntity(e); return [p.entityHostType, p.clientOnly]; });
        attempt("Entities.getPropertyInfo", function () { return Entities.getPropertyInfo("alpha"); });
        attempt("Entities.findRayIntersection", function () { var r = Entities.findRayIntersection({ origin: { x: 0, y: 2, z: 0 }, direction: { x: 0, y: 0, z: -1 } }, true, [id]); return [Object.keys(r), r.intersects, r.entityID === id, r.face, Math.round(r.distance * 100), r.intersection, r.surfaceNormal]; });
        // the lists matter: an include list naming no entity hits nothing, a discard list naming the entity skips it
        attempt("Entities.findRayIntersection include other", function () { var r = Entities.findRayIntersection({ origin: { x: 0, y: 2, z: 0 }, direction: { x: 0, y: 0, z: -1 } }, false, [Uuid.generate()]); return [r.intersects, r.entityID === id]; });
        attempt("Entities.findRayIntersection discard", function () { var r = Entities.findRayIntersection({ origin: { x: 0, y: 2, z: 0 }, direction: { x: 0, y: 0, z: -1 } }, true, [], [id], false, false); return [r.entityID === id]; });
        attempt("Entities.getServerScriptStatus call", function () { return Entities.getServerScriptStatus(id, function (received, running, status, errorInfo) { statusAnswers.push([received, running, status, typeof errorInfo]); }); });
        attempt("Entities.queryPropertyMetadata call", function () { return Entities.queryPropertyMetadata(id, "script", function (err, result) { metadataAnswers.push([err ? err.name : null, err instanceof Error, typeof (err && err.message), result ? Object.keys(result).sort() : null]); }); });
        attempt("Entities.queryPropertyMetadata scope form", function () { var scope = { seen: function (err, result) { metadataAnswers.push(["scoped", this === scope]); } }; return Entities.queryPropertyMetadata(id, "script", scope, "seen"); });
        attempt("Entities.queryPropertyMetadata unknown", function () { return Entities.queryPropertyMetadata(id, "color", function () {}); });
        attempt("Entities.getMeshes", function () { var answer = null; Entities.getMeshes(id, function (meshes, success) { answer = [success, meshes === undefined ? "undefined" : typeof meshes]; }); return answer; });
        attempt("AvatarList.findRayIntersection", function () { var r = AvatarList.findRayIntersection({ origin: { x: 0, y: 100, z: 0 }, direction: { x: 0, y: 1, z: 0 } }); return [Object.keys(r), r.intersects, r.avatarID, r.jointIndex]; });
        attempt("Controller.getPoseValue", function () { return Controller.getPoseValue(Controller.Standard.LeftHand); });
        attempt("MyAvatar.getLeftHandPose", function () { return MyAvatar.getLeftHandPose(); });
        attempt("Controller.getAllActions", function () { var a = Controller.getAllActions(); return [Array.isArray(a), a.length > 0, a.length ? a[0] : null]; });
        attempt("Controller.getAvailableInputs", function () { var a = Controller.getAvailableInputs(0); return [Array.isArray(a), a.length > 0 ? Object.keys(a[0]) : a.length]; });
        attempt("MyAvatar.getAnimationDetailsByRole keys", function () { return Object.keys(MyAvatar.getAnimationDetailsByRole("idle")); });
        attempt("MyAvatar.getAvatarEntityData", function () { var e = Entities.addEntity({ type: "Box", name: "wasm-parity-avatar-entity", position: ENTITY_POSITION, dimensions: { x: 0.1, y: 0.1, z: 0.1 }, lifetime: 60 }, "avatar"); var data = MyAvatar.getAvatarEntityData(); var mine = data[e]; Entities.deleteEntity(e); return [typeof data, mine ? mine.name : typeof mine]; });
        attempt("MyAvatar.getAttachmentData", function () { return MyAvatar.getAttachmentData(); });
        attempt("Menu.addMenuItem properties", function () { Menu.addMenuItem({ menuName: "Developer", menuItemName: "wasm-parity item", isCheckable: true, isChecked: true }); var r = [Menu.menuItemExists("Developer", "wasm-parity item"), Menu.isOptionChecked("wasm-parity item")]; Menu.removeMenuItem("Developer", "wasm-parity item"); return r; });
        attempt("Controller.captureKeyEvents", function () { Controller.captureKeyEvents({ text: "F9" }); Controller.releaseKeyEvents({ text: "F9" }); return "ok"; });
        attempt("AnimationCache.getAnimation", function () { return [typeof animation, Array.isArray(animation.frames), animation.frames.length > 0, animation.jointNames.length > 0, animation.frames.length ? Object.keys(animation.frames[0]) : null]; });
        attempt("SoundCache.getSound loaded", function () { return [sound.downloaded, sound.duration > 0]; });
        attempt("Audio.playSound options", function () { var injector = Audio.playSound(sound, { volume: 0.01, localOnly: true, position: { x: 1, y: 2, z: 3 }, loop: false }); if (!injector) { return "null"; } var options = injector.options; injector.stop(); return options; });
        attempt("Entities.sendClickDownOnEntity", function () { Entities.sendClickDownOnEntity(id, { type: "Press", id: 7, pos2D: { x: 0.25, y: 0.5 }, pos3D: { x: 1, y: 2, z: 3 }, normal: { x: 0, y: 0, z: 1 }, direction: { x: 0, y: 0, z: -1 }, button: "Primary", isPrimaryHeld: true, keyboardModifiers: 0 }); return "sent"; });
        attempt("Desktop.createWindow", function () { var w = Desktop.createWindow(Paths.defaultScripts + "/system/create/qml/NewModelWindow.qml", { title: "wasm-parity", visible: false, size: { x: 100, y: 100 } }); var r = [typeof w, typeof w.sendToQml, typeof w.fromQml]; w.close(); return r; });
        attempt("MyAvatar.getTargetAvatar", function () { return MyAvatar.getTargetAvatar(); });
        attempt("AvatarList.findRayIntersection lists", function () { var r = AvatarList.findRayIntersection({ origin: { x: 0, y: 100, z: 0 }, direction: { x: 0, y: 1, z: 0 } }, [], [MyAvatar.sessionUUID]); return [r.intersects, r.avatarID]; });
        attempt("Picks.setIgnoreItems", function () { var p = Picks.createPick(PickType.Ray, { joint: "Mouse", filter: Picks.PICK_ENTITIES, enabled: true }); Picks.setIgnoreItems(p, [id]); Picks.setIncludeItems(p, []); Picks.removePick(p); return "ok"; });
        attempt("Pointers.setIgnoreItems", function () { var p = Pointers.createPointer(PickType.Ray, { joint: "Mouse", filter: Picks.PICK_ENTITIES, enabled: false, renderStates: [] }); Pointers.setIgnoreItems(p, [id]); Pointers.setIncludeItems(p, [id]); Pointers.removePointer(p); return "ok"; });
        attempt("Model meshes", function () {
            var mesh = Model.newMesh([{ x: 0, y: 0, z: 0 }, { x: 1, y: 0, z: 0 }, { x: 0, y: 1, z: 0 }], [], [{ vertices: [0, 1, 2] }]);
            var both = Model.appendMeshes([mesh, mesh]);
            return [typeof mesh, Model.getVertexCount(mesh), Model.getVertex(mesh, 1), Model.getVertex(mesh, 7), Model.getVertexCount(both), Model.meshToOBJ([mesh]).length > 0];
        });
        attempt("Window.hasFocus type", function () { return typeof Window.hasFocus(); });
        attempt("TREE_SCALE", function () { return [TREE_SCALE + 0, TREE_SCALE / 2]; });
        attempt("Overlays.findRayIntersection lists", function () { var r = Overlays.findRayIntersection({ origin: { x: 0, y: 100, z: 0 }, direction: { x: 0, y: 1, z: 0 } }, true, [], [id]); return [Object.keys(r), r.intersects, r.overlayID]; });
        attempt("ModelCache.prefetch", function () { var r = ModelCache.prefetch(Paths.defaultScripts + "/system/assets/models/black-sphere.fbx"); return [typeof r, typeof r.state, typeof r.loaded]; });
        attempt("AvatarList.setAvatarSortCoefficient", function () { AvatarList.setAvatarSortCoefficient("age", "not a number"); return "ok"; });
        attempt("Script.registerValue", function () { Script.registerValue("wasmParity.inner.value", 5); Script.registerValue("Vec3", 1); return [Array.isArray(wasmParity), wasmParity.inner.value, typeof Vec3]; });
        attempt("Assets.getMapping bad path", function () { Assets.getMapping("not a path", function () {}); return "no throw"; });
        attempt("Assets.compressData", function () {
            Assets.compressData({ data: "hello hello hello hello hello" }, function (error, result) {
                assetOutcomes.push(["compressed", error, typeof result.data, result.byteLength > 0]);
                Assets.decompressData({ data: result.data, responseType: "text" }, { name: "scope" }, function (error2, result2) {
                    assetOutcomes.push(["decompressed", error2, this.name, result2.response]);
                });
            });
            return "asked";
        });
        attempt("Entities.deleteEntity", function () { Entities.deleteEntity(id); return typeof Entities.getEntityProperties(id).name; });
        attempt("Entities.addEntity with script", function () { var e = Entities.addEntity({ type: "Box", position: ENTITY_POSITION, dimensions: { x: 0.1, y: 0.1, z: 0.1 }, lifetime: 60, script: "(function(){ this.preload = function(){}; })" }); var s = Entities.getEntityProperties(e, ["script"]).script; Entities.deleteEntity(e); return s.length > 10; });
        attempt("Math.random type", function () { return typeof Math.random(); });
        attempt("Date.now type", function () { return typeof Date.now(); });
        attempt("new Date toISOString length", function () { return new Date(0).toISOString(); });
        attempt("Array.prototype.includes", function () { return typeof Array.prototype.includes; });
        attempt("typeof Symbol", function () { return typeof Symbol; });
        attempt("typeof Promise", function () { return typeof Promise; });
        attempt("arguments.callee", function () { return (function () { return typeof arguments.callee; }()); });
        attempt("String.prototype.padStart", function () { return typeof "".padStart; });
        attempt("parseInt('08')", function () { return parseInt("08"); });
        attempt("JSON.stringify(undefined-in-obj)", function () { return JSON.stringify({ a: undefined, b: 1 }); });
        attempt("Number toFixed", function () { return (1.005).toFixed(2); });
        attempt("Error has stack", function () { return typeof new Error("x").stack; });
        attempt("Script.evaluate", function () { return Script.evaluate("1 + 2", "eval.js"); });
        attempt("Script.print", function () { Script.print("parity print"); return "ok"; });
        attempt("Test present", function () { return typeof Test; });

        // what arrives by signal, a moment later
        Script.setTimeout(function () {
            attempt("clickDownOnEntity event", function () { return clicks.length ? [clicks[0].entityID === id, clicks[0].event] : "none"; });
            attempt("Assets outcomes", function () { return assetOutcomes; });
            attempt("Entities.getServerScriptStatus answer", function () { return statusAnswers.length ? statusAnswers[0] : "none"; });
            attempt("Entities.queryPropertyMetadata answers", function () { return metadataAnswers; });
            attempt("Graphics.getModel", function () {
                var model = Graphics.getModel(modelEntity);
                if (!model) { return "no model"; }
                var mesh = model.meshes[0];
                var part = mesh ? mesh.parts[0] : null;
                return [typeof model, model.numMeshes, model.meshes.length, typeof model.objectID, model.materialNames,
                        Object.keys(model.materialLayers).sort(), mesh ? mesh.numParts : null, part ? part.topology : null,
                        JSON.stringify(model.materialLayers).length > 2];
            });
            Entities.deleteEntity(modelEntity);
            for (var i = 0; i < lines.length; i++) {
                print("PARITY " + lines[i]);
            }
            print("PARITY done " + lines.length + " checks");
            Script.setTimeout(function () { Test.quit(); }, 1000);
        }, 2500);
    }

    Script.setTimeout(run, 8000);
}());
