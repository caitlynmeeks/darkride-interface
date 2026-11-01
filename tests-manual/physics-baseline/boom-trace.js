//
//  boom-trace.js
//  tests-manual/physics-baseline
//
//  The frame trace's camera, boom and ride columns (team/performance/INSTRUMENTS-PLAN.md, item 7; ViewState.cpp): the
//  avatar stands on a box it is parented to (its ride), in third person with camera collision on and a wall 1.5 m
//  behind it holding the boom in; two seconds are traced, then the trace is checked: every column of item 7 present,
//  the boom held short of what it wants with the wall named as the holder, the box named as the ride. Prints
//  BOOM-TRACE lines ending in PASS or FAIL; the trace is dumped as boom-trace.csv in the results location.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    var config = (typeof BOOM_CONFIG !== "undefined" && BOOM_CONFIG) ? BOOM_CONFIG : {};
    var RESULTS = config.results || "";
    var LIFETIME = 120;
    var TRACE_SECONDS = 2;
    var COLUMNS = ["cam.x_m", "cam.y_m", "cam.z_m", "cam.qw", "cam.qx", "cam.qy", "cam.qz", "cam.mode",
                   "boom.wanted_m", "boom.allowed_m", "boom.current_m", "boom.holder",
                   "ride.id", "ride.x_m", "ride.y_m", "ride.z_m", "ride.qw", "ride.qx", "ride.qy", "ride.qz", "ride.correction_m"];
    var box = null, wall = null;

    function log(s) { print("BOOM-TRACE " + s); }
    function after(seconds, f) { Script.setTimeout(f, seconds * 1000); }
    function finish(verdict) {
        log(verdict);
        if (box) { Entities.deleteEntity(box); }
        if (wall) { Entities.deleteEntity(wall); }
        after(1, function () { Test.quit(); });
    }

    function check() {
        var written = RESULTS ? Test.dumpFrameTrace(RESULTS + "/boom-trace.csv") : -1;   // before take, which empties the ring
        var trace = Test.takeFrameTrace();
        var columns = trace.length ? trace[0] : [];
        var rows = trace.slice(1);
        var index = {};
        columns.forEach(function (name, i) { index[name] = i; });
        var missing = COLUMNS.filter(function (name) { return index[name] === undefined; });
        log(rows.length + " frames traced, " + columns.length + " columns, " + written + " rows dumped");
        if (missing.length) {
            finish("FAIL columns missing: " + missing.join(" "));
            return;
        }
        var held = 0, holderIsWall = 0, rideIsBox = 0, complete = 0, wanted = 0;
        rows.forEach(function (row) {
            var w = row[index["boom.wanted_m"]], a = row[index["boom.allowed_m"]];
            if (typeof w !== "number" || typeof a !== "number") { return; }
            complete++;
            wanted = w;
            if (a < w - 0.05) { held++; }
            if (row[index["boom.holder"]] === wall) { holderIsWall++; }
            if (row[index["ride.id"]] === box) { rideIsBox++; }
        });
        log("boom wanted " + wanted.toFixed(2) + " m; of " + complete + " frames with a boom: held in " + held +
            ", the wall named " + holderIsWall + ", the box the ride " + rideIsBox);
        if (complete < 30) {
            finish("FAIL too few frames with the boom's columns (" + complete + ")");
        } else if (held < complete / 2 || holderIsWall < complete / 2) {
            finish("FAIL the wall does not hold the boom in (held " + held + ", named " + holderIsWall + " of " + complete + ")");
        } else if (rideIsBox < complete / 2) {
            finish("FAIL the box is not the ride (" + rideIsBox + " of " + complete + ")");
        } else {
            finish("PASS");
        }
    }

    function start(tries) {
        if (!Window.isPhysicsEnabled()) {
            if (tries < 120) {
                after(1, function () { start(tries + 1); });
            } else {
                finish("FAIL no physics after 120 s");
            }
            return;
        }
        if (typeof Test.frameTrace !== "function") {
            finish("FAIL this build has no frame trace (Test.frameTrace)");
            return;
        }
        // the avatar faces -z on a box of its own; the wall stands across +z, 1.5 m behind the pivot
        box = Entities.addEntity({
            type: "Box", shape: "Cube", name: "boom-trace-ride", position: { x: 0, y: 0.95, z: 0 },
            dimensions: { x: 4, y: 0.1, z: 4 }, color: { red: 90, green: 120, blue: 90 }, dynamic: false,
            collisionless: false, lifetime: LIFETIME, grab: { grabbable: false }
        });
        wall = Entities.addEntity({
            type: "Box", shape: "Cube", name: "boom-trace-wall", position: { x: 0, y: 2.5, z: 1.5 },
            dimensions: { x: 8, y: 5, z: 0.2 }, color: { red: 160, green: 90, blue: 90 }, dynamic: false,
            collisionless: false, lifetime: LIFETIME, grab: { grabbable: false }
        });
        MyAvatar.position = { x: 0, y: 2, z: 0 };
        MyAvatar.orientation = Quat.fromPitchYawRollDegrees(0, 0, 0);
        Camera.mode = "third person";
        Menu.setIsOptionChecked("Camera Collision", true);
        after(2, function () {
            MyAvatar.setParentID(box);
            after(1, function () {
                log("tracing " + TRACE_SECONDS + " s: third person, camera collision on, parented to " + box + ", wall " + wall);
                Test.frameTrace(true);
                after(TRACE_SECONDS, function () {
                    Test.frameTrace(false);
                    check();
                });
            });
        });
    }

    after(2, function () { start(0); });
}());
