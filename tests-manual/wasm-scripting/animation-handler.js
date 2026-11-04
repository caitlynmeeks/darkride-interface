//
//  animation-handler.js
//  tests-manual/wasm-scripting
//
//  A script's animation state handlers (MyAvatar.addAnimationStateHandler / removeAnimationStateHandler), serverless,
//  on this build: the checks avatars asked for when the QtScript handler path went (2026-11-04).
//    (a) a null list: the handler gets every animation variable (more than none);
//    (b) a names list: only those names, a name that does not exist skipped;
//    (c) an empty list: an empty object;
//    (d) a returned right-hand IK target lands: the right hand ends within 5 cm of it;
//    (e) after removeAnimationStateHandler: no more calls, and the hand goes back to rest;
//    (f) the script ends with a handler still added: no crash (the client exits cleanly);
//    (g) squeezeHands.js loaded beside it: the runner counts "Ignoring unrecognized data" lines in the log (none).
//  Prints ANIM-HANDLER lines and one RESULT line; the runner is animation-handler.sh.
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//

(function () {
    var failures = 0;
    function say(text) { print("ANIM-HANDLER " + text); }
    function check(name, ok, detail) {
        if (!ok) { failures++; }
        say(name + " " + (ok ? "PASS" : "FAIL") + (detail ? " " + detail : ""));
    }
    function count(object) { var n = 0; for (var k in object) { if (object.hasOwnProperty(k)) { n++; } } return n; }
    var Y_180 = { x: 0, y: 1, z: 0, w: 0 };
    // a rig-frame point to world: rig = Y_180 * object frame (as hipsIkTest.js), object frame = avatar position + orientation
    function rigToWorld(p) {
        return Vec3.sum(MyAvatar.position, Vec3.multiplyQbyV(MyAvatar.orientation, Vec3.multiplyQbyV(Y_180, p)));
    }
    // world to the rig frame: the inverse of rigToWorld
    function worldToRig(p) {
        return Vec3.multiplyQbyV(Y_180, Vec3.multiplyQbyV(Quat.inverse(MyAvatar.orientation), Vec3.subtract(p, MyAvatar.position)));
    }
    var RAISE = { x: 0, y: 0.3, z: 0 };   // the target: the resting right hand, raised 30 cm (reachable from any rest pose)
    var TARGET_RIG;
    var restHand;

    function waitForSkeleton(then, tries) {
        var names = MyAvatar.getJointNames();
        if (names.length > 0 && MyAvatar.getJointIndex("RightHand") >= 0) {
            say("skeleton ready: " + names.length + " joints, RightHand " + MyAvatar.getJointIndex("RightHand"));
            then();
            return;
        }
        if (tries > 60) { check("skeleton", false, "no skeleton after 30 s"); finish(); return; }
        Script.setTimeout(function () { waitForSkeleton(then, tries + 1); }, 500);
    }

    function stepA() {
        var calls = 0, keys = 0;
        var id = MyAvatar.addAnimationStateHandler(function (vars) {
            calls++;
            if (calls === 1) { keys = count(vars); }
            return vars;
        }, null);
        say("(a) handler id " + id);
        Script.setTimeout(function () {
            MyAvatar.removeAnimationStateHandler(id);
            check("(a) null list: every variable", calls > 0 && keys > 0, calls + " calls, " + keys + " variables in the first");
            stepB();
        }, 1000);
    }

    function stepB() {
        var seen = null, calls = 0;
        var id = MyAvatar.addAnimationStateHandler(function (vars) {
            calls++;
            if (!seen) { seen = Object.keys(vars).sort(); }
            return vars;
        }, ["rightHandType", "headType", "noSuchVariable"]);
        Script.setTimeout(function () {
            MyAvatar.removeAnimationStateHandler(id);
            var ok = seen !== null && seen.indexOf("rightHandType") >= 0 && seen.indexOf("headType") >= 0 &&
                seen.indexOf("noSuchVariable") < 0 && seen.length === 2;
            check("(b) a names list: only those names, the unknown one skipped", ok, calls + " calls, keys " + JSON.stringify(seen));
            stepC();
        }, 1000);
    }

    function stepC() {
        var keys = -1, calls = 0;
        var id = MyAvatar.addAnimationStateHandler(function (vars) {
            calls++;
            if (keys < 0) { keys = count(vars); }
            return {};
        }, []);
        Script.setTimeout(function () {
            MyAvatar.removeAnimationStateHandler(id);
            check("(c) an empty list: an empty object", calls > 0 && keys === 0, calls + " calls, " + keys + " keys");
            stepD();
        }, 1000);
    }

    var idD, callsD = 0;
    function stepD() {
        restHand = MyAvatar.getJointPosition("RightHand");
        var restRig = worldToRig(restHand);
        TARGET_RIG = Vec3.sum(restRig, RAISE);
        var target = rigToWorld(TARGET_RIG);
        say("(d) rest hand (rig frame) " + JSON.stringify(restRig) + ", target (rig frame) " + JSON.stringify(TARGET_RIG) +
            ", frame check: rigToWorld(worldToRig(rest)) is " + Vec3.distance(rigToWorld(restRig), restHand).toFixed(4) + " m from rest");
        idD = MyAvatar.addAnimationStateHandler(function (vars) {
            callsD++;
            return { rightHandType: 0, rightHandPosition: TARGET_RIG, rightHandRotation: { x: 0, y: 0, z: 0, w: 1 } };
        }, ["rightHandType", "rightHandPosition", "rightHandRotation"]);
        Script.setTimeout(function () {
            var hand = MyAvatar.getJointPosition("RightHand");
            var distance = Vec3.distance(hand, target);
            check("(d) a right-hand IK target lands (within 5 cm)", callsD > 0 && distance < 0.05,
                callsD + " calls, " + distance.toFixed(3) + " m from the target; rest was " + Vec3.distance(restHand, target).toFixed(3) + " m away");
            stepE();
        }, 3000);
    }

    function stepE() {
        MyAvatar.removeAnimationStateHandler(idD);
        var target = rigToWorld(TARGET_RIG);
        var callsAtRemoval;
        Script.setTimeout(function () { callsAtRemoval = callsD; }, 100);
        Script.setTimeout(function () {
            var hand = MyAvatar.getJointPosition("RightHand");
            var distance = Vec3.distance(hand, target);
            check("(e) removed: no more calls", callsD === callsAtRemoval, callsD + " calls now, " + callsAtRemoval + " just after the removal");
            check("(e) removed: the hand went back to rest (more than 10 cm from the target)", distance > 0.10, distance.toFixed(3) + " m from the target");
            stepG();
        }, 3000);
    }

    function stepG() {
        var url = Paths.defaultScripts + "/system/controllers/squeezeHands.js";
        say("(g) loading " + url);
        Script.load(url);
        Script.setTimeout(stepF, 3000);
    }

    function stepF() {
        var calls = 0;
        MyAvatar.addAnimationStateHandler(function (vars) { calls++; return vars; }, null);
        say("(f) a handler left added at the end; the client must exit cleanly (the runner checks the exit status)");
        finish();
    }

    function finish() {
        say("RESULT " + (failures === 0 ? "PASS" : "FAIL (" + failures + " failed)"));
        Script.setTimeout(function () { Test.quit(); }, 500);
    }

    Script.setTimeout(function () { waitForSkeleton(stepA, 0); }, 2000);
}());
