//
//  action-data-golden.js
//  tests-manual/wasm-scripting
//
//  An entity's actionData on this build against the recorded wire format (tests/entity-actions/src/
//  ActionDataFormatTests.cpp, bytes of the Qt 5.15 build), through the real client classes: every action type the
//  client makes (offset, tractor, travel-oriented, hinge, slider, ball-socket, cone-twist, and the client-only hold and
//  far-grab, which the unit test can only write by hand). Serverless, in a walled client.
//    (a) write: each type added with the golden's arguments; the entity's actionData holds bytes equal to the golden
//        but for the action's id (random; the 16 bytes at offset 2, checked against the id addAction returned);
//    (b) read: an actionData made of the nine goldens (each given its own id) set on a second entity; every action
//        comes back through Entities.getActionArguments with the golden's tag and its first field.
//  Prints ACTION-DATA lines and one RESULT line; stops itself. Run it on a Qt 5 client (expect PASS: the bytes are
//  what that build recorded) and on a Qt 6 client (the point: the same bytes).
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//

(function () {
    var failures = 0;
    function say(text) { print("ACTION-DATA " + text); }
    function check(name, ok, detail) {
        if (!ok) { failures++; }
        say(name + " " + (ok ? "PASS" : "FAIL") + (detail ? " " + detail : ""));
    }

    // the values the goldens were recorded with
    var POINT = { x: 1.5, y: -2.25, z: 3.125 };
    var AXIS = { x: 0, y: 1, z: 0 };
    var OTHER_POINT = { x: -0.5, y: 0.75, z: 2 };
    var OTHER_AXIS = { x: 1, y: 0, z: 0 };
    var ROTATION = { x: 0.5, y: -0.5, z: 0.5, w: 0.5 };
    var OTHER_ID = "{9a8b7c6d-5e4f-4d3c-8b2a-1f0e9d8c7b6a}";
    var HOLDER_ID = "{1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d}";
    var TAG = "golden";

    // GOLDEN_* of ActionDataFormatTests.cpp, with the arguments that made them and the first field read back
    var TYPES = [
        { type: "offset", first: "linearDistance", expect: 2,
          args: { pointToOffsetFrom: POINT, linearDistance: 2, linearTimeScale: 0.25, tag: TAG },
          hex: "03e83c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00013ff8000000000000c002000000000000400900000000000040000000000000003fd00000000000000100000000000000000000000c0067006f006c00640065006e" },
        { type: "tractor", first: "angularTimeScale", expect: 1.5,
          args: { targetPosition: POINT, linearTimeScale: 0.25, targetRotation: ROTATION, angularTimeScale: 1.5, otherID: OTHER_ID, otherJointIndex: 3, tag: TAG },
          hex: "08343c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00013ff8000000000000c00200000000000040090000000000003fd0000000000000013fe0000000000000bfe00000000000003fe00000000000003fe00000000000003ff80000000000000100000000000000000000000c0067006f006c00640065006e9a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6a00000003" },
        { type: "travel-oriented", first: "angularTimeScale", expect: 0.25,
          args: { forward: AXIS, angularTimeScale: 0.25, tag: TAG },
          hex: "0fa03c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000100000000000000003ff000000000000000000000000000003fd000000000000000000000000000000000000c0067006f006c00640065006e" },
        { type: "hinge", first: "high", expect: 1.5,
          args: { pivot: POINT, axis: AXIS, otherEntityID: OTHER_ID, otherPivot: OTHER_POINT, otherAxis: OTHER_AXIS, low: -0.75, high: 1.5, tag: TAG },
          hex: "13885e6f7a8b9c0d4e1f8a2b3c4d5e6f7a8b00023ff8000000000000c002000000000000400900000000000000000000000000003ff000000000000000000000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000000000003fe800000000000040000000000000003ff000000000000000000000000000000000000000000000bfe80000000000003ff800000000000000000000000000000000000c0067006f006c00640065006e" },
        { type: "slider", first: "angularHigh", expect: 2,
          args: { point: POINT, axis: AXIS, otherEntityID: OTHER_ID, otherPoint: OTHER_POINT, otherAxis: OTHER_AXIS, linearLow: -0.75, linearHigh: 1.5, angularLow: -2, angularHigh: 2, tag: TAG },
          hex: "1b583c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000100000000000000000000000c0067006f006c00640065006e3ff8000000000000c002000000000000400900000000000000000000000000003ff000000000000000000000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000000000003fe800000000000040000000000000003ff000000000000000000000000000000000000000000000bfe80000000000003ff8000000000000c0000000000000004000000000000000" },
        { type: "ball-socket", first: "otherEntityID", expect: OTHER_ID,
          args: { pivot: POINT, otherEntityID: OTHER_ID, otherPivot: OTHER_POINT, tag: TAG },
          hex: "1f403c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000100000000000000000000000c0067006f006c00640065006e3ff8000000000000c00200000000000040090000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000000000003fe80000000000004000000000000000" },
        { type: "cone-twist", first: "twistSpan", expect: 2,
          args: { pivot: POINT, axis: AXIS, otherEntityID: OTHER_ID, otherPivot: OTHER_POINT, otherAxis: OTHER_AXIS, swingSpan1: 0.25, swingSpan2: 1.5, twistSpan: 2, tag: TAG },
          hex: "23283c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000200000000000000000000000c0067006f006c00640065006e3ff8000000000000c002000000000000400900000000000000000000000000003ff000000000000000000000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000000000003fe800000000000040000000000000003ff0000000000000000000000000000000000000000000003fd00000000000003ff80000000000004000000000000000" },
        { type: "hold", first: "hand", expect: "left",
          args: { holderID: HOLDER_ID, relativePosition: POINT, relativeRotation: ROTATION, timeScale: 0.25, hand: "left", kinematic: true, kinematicSetVelocity: false, tag: TAG },
          hex: "0bb83c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00011a2b3c4d5e6f4a7b8c9d0e1f2a3b4c5d3ff8000000000000c00200000000000040090000000000003fe0000000000000bfe00000000000003fe00000000000003fe00000000000003fd000000000000000000008006c00650066007400000000000000000000000c0067006f006c00640065006e0100" },
        { type: "far-grab", first: "otherJointIndex", expect: 3,
          args: { targetPosition: POINT, linearTimeScale: 0.25, targetRotation: ROTATION, angularTimeScale: 1.5, otherID: OTHER_ID, otherJointIndex: 3, tag: TAG },
          hex: "17703c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00013ff8000000000000c00200000000000040090000000000003fd0000000000000013fe0000000000000bfe00000000000003fe00000000000003fe00000000000003ff80000000000000100000000000000000000000c0067006f006c00640065006e9a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6a00000003" }
    ];

    // bytes as arrays of numbers; hex and base64 by hand (the engine has no atob)
    var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    function fromHex(hex) { var out = []; for (var i = 0; i < hex.length; i += 2) { out.push(parseInt(hex.substr(i, 2), 16)); } return out; }
    function toHex(bytes) { var s = ""; for (var i = 0; i < bytes.length; i++) { s += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16); } return s; }
    function fromBase64(text) {
        var out = [], bits = 0, value = 0;
        for (var i = 0; i < text.length; i++) {
            var c = B64.indexOf(text.charAt(i));
            if (c < 0) { continue; }
            value = (value << 6) | c; bits += 6;
            if (bits >= 8) { bits -= 8; out.push((value >> bits) & 255); }
        }
        return out;
    }
    function toBase64(bytes) {
        var s = "", i;
        for (i = 0; i + 2 < bytes.length; i += 3) {
            var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
            s += B64.charAt(n >> 18) + B64.charAt((n >> 12) & 63) + B64.charAt((n >> 6) & 63) + B64.charAt(n & 63);
        }
        if (i < bytes.length) {
            var m = bytes[i] << 16 | ((i + 1 < bytes.length ? bytes[i + 1] : 0) << 8);
            s += B64.charAt(m >> 18) + B64.charAt((m >> 12) & 63) + (i + 1 < bytes.length ? B64.charAt((m >> 6) & 63) : "=") + "=";
        }
        return s;
    }
    function u32(n) { return [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255]; }
    function readU32(bytes, at) { return ((bytes[at] << 24) >>> 0) + (bytes[at + 1] << 16) + (bytes[at + 2] << 8) + bytes[at + 3]; }
    function uuidHex(id) { return String(id).replace(/[{}-]/g, "").toLowerCase(); }
    // an entity's actionData: a QVector<QByteArray> in the wire format (count, then length + bytes each)
    function openEnvelope(bytes) {
        var actions = [], at = 4, count = readU32(bytes, 0);
        for (var i = 0; i < count; i++) { var n = readU32(bytes, at); at += 4; actions.push(bytes.slice(at, at + n)); at += n; }
        return actions;
    }
    function envelope(actions) {
        var bytes = u32(actions.length);
        for (var i = 0; i < actions.length; i++) { bytes = bytes.concat(u32(actions[i].length), actions[i]); }
        return bytes;
    }
    function near(a, b) { return typeof a === "number" && Math.abs(a - b) < 1e-6; }
    function same(got, expect) {
        if (typeof expect === "number") { return near(got, expect); }
        if (typeof expect === "string" && expect.charAt(0) === "{") { return uuidHex(got) === uuidHex(expect); }
        return got === expect;
    }

    var writer, reader;
    function addEntity(name) {
        return Entities.addEntity({
            type: "Box", name: name, position: Vec3.sum(MyAvatar.position, { x: 0, y: 1, z: -2 }),
            dimensions: { x: 0.2, y: 0.2, z: 0.2 }, collisionless: true, dynamic: false, lifetime: 120
        });
    }

    // (a) every type written by the client's own classes
    function checkWrites() {
        var ids = {};
        for (var i = 0; i < TYPES.length; i++) {
            var t = TYPES[i];
            ids[t.type] = Entities.addAction(t.type, writer, t.args);
            check("add " + t.type, ids[t.type] && !Uuid.isNull(ids[t.type]), String(ids[t.type]));
        }
        var data = Entities.getEntityProperties(writer, ["actionData"]).actionData;
        var actions = openEnvelope(fromBase64(data || ""));
        check("actionData holds " + TYPES.length + " actions", actions.length === TYPES.length, "got " + actions.length);
        for (i = 0; i < TYPES.length; i++) {
            t = TYPES[i];
            var want = t.hex.substr(0, 4) + uuidHex(ids[t.type]) + t.hex.substr(36);
            var found = null;
            for (var j = 0; j < actions.length; j++) {
                if (toHex(actions[j]).substr(4, 32) === uuidHex(ids[t.type])) { found = toHex(actions[j]); }
            }
            check("bytes " + t.type, found === want, found === want ? found.length / 2 + " bytes" : "got " + found + " want " + want);
        }
    }

    // (b) the goldens, each under its own id, read by the client's own classes
    function checkReads() {
        var actions = [], ids = [];
        for (var i = 0; i < TYPES.length; i++) {
            var id = Uuid.generate();
            var bytes = fromHex(TYPES[i].hex.substr(0, 4) + uuidHex(id) + TYPES[i].hex.substr(36));
            actions.push(bytes); ids.push(id);
        }
        Entities.editEntity(reader, { actionData: toBase64(envelope(actions)) });
        Script.setTimeout(function () {
            var listed = Entities.getActionIDs(reader);
            check("reader lists " + TYPES.length + " actions", listed.length === TYPES.length, "got " + listed.length);
            for (var i = 0; i < TYPES.length; i++) {
                var t = TYPES[i];
                var got = Entities.getActionArguments(reader, ids[i]);
                var ok = got && got.type === t.type && got.tag === TAG && same(got[t.first], t.expect);
                check("read " + t.type, ok, got ? "type " + got.type + " tag " + got.tag + " " + t.first + " " + JSON.stringify(got[t.first]) : "no arguments");
            }
            finish();
        }, 1000);
    }

    function finish() {
        Entities.deleteEntity(writer);
        Entities.deleteEntity(reader);
        say("RESULT " + (failures === 0 ? "PASS" : "FAIL " + failures));
        Script.setTimeout(function () { Script.stop(); }, 500);
    }

    Script.setTimeout(function () {
        writer = addEntity("action-data-golden writer");
        reader = addEntity("action-data-golden reader");
        check("entities added", !Uuid.isNull(writer) && !Uuid.isNull(reader));
        Script.setTimeout(function () { checkWrites(); checkReads(); }, 1000);
    }, 3000);
    Script.setTimeout(function () { say("timeout"); finish(); }, 30000);
})();
