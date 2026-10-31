//
//  carry-trace.js
//  tests-manual/physics-baseline
//
//  The frame trace's carry columns (team/performance/INSTRUMENTS-PLAN.md, item 5; MyAvatar.cpp traceCarry): a box
//  moving at 2 m/s as a frame (frame: true), the avatar dropped onto it and carried standing still; two seconds are
//  traced once aboard, then checked: the carry columns present, the box named as the frame in most frames, the avatar's
//  local step at most 2 mm a frame (standing still, whatever the frame does), the lead column written. Prints
//  CARRY-TRACE lines ending in PASS or FAIL; the trace is dumped as carry-trace.csv in the results location.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    var config = (typeof CARRY_CONFIG !== "undefined" && CARRY_CONFIG) ? CARRY_CONFIG : {};
    var RESULTS = config.results || "";
    var LIFETIME = 120;
    var TRACE_SECONDS = 2;
    var MAX_STEP_MM = 2;
    var COLUMNS = ["carry.frame", "carry.local_x_m", "carry.local_y_m", "carry.local_z_m", "carry.step_mm", "carry.lead_mm"];
    var box = null;

    function log(s) { print("CARRY-TRACE " + s); }
    function after(seconds, f) { Script.setTimeout(f, seconds * 1000); }
    function finish(verdict) {
        log(verdict);
        if (box) { Entities.deleteEntity(box); }
        after(1, function () { Test.quit(); });
    }

    function check() {
        var written = RESULTS ? Test.dumpFrameTrace(RESULTS + "/carry-trace.csv") : -1;   // before take, which empties the ring
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
        var aboard = 0, steps = [], leads = 0;
        rows.forEach(function (row) {
            if (row[index["carry.frame"]] !== box) { return; }
            aboard++;
            var step = row[index["carry.step_mm"]];
            if (typeof step === "number") { steps.push(step); }
            if (typeof row[index["carry.lead_mm"]] === "number") { leads++; }
        });
        steps.sort(function (a, b) { return a - b; });
        var p95 = steps.length ? steps[Math.floor(steps.length * 0.95)] : -1;
        var max = steps.length ? steps[steps.length - 1] : -1;
        log("aboard " + aboard + " of " + rows.length + " frames; local step p95 " + p95.toFixed(2) + " mm, max " +
            max.toFixed(2) + " mm over " + steps.length + " steps; lead written in " + leads);
        if (aboard < rows.length / 2 || aboard < 30) {
            finish("FAIL not carried by the box in most frames (" + aboard + " of " + rows.length + ")");
        } else if (p95 > MAX_STEP_MM) {
            finish("FAIL the carry steps: p95 " + p95.toFixed(2) + " mm, max " + max.toFixed(2) + " mm (allowed " + MAX_STEP_MM + ")");
        } else if (leads < aboard / 2) {
            finish("FAIL the lead column is written in " + leads + " of " + aboard + " frames aboard");
        } else {
            finish("PASS");
        }
    }

    function waitAboard(tries) {
        if (MyAvatar.getParentID() === box) {
            log("aboard the frame " + box + " after " + tries + " s; tracing " + TRACE_SECONDS + " s standing still");
            Test.frameTrace(true);
            after(TRACE_SECONDS, function () {
                Test.frameTrace(false);
                check();
            });
        } else if (tries < 15) {
            after(1, function () { waitAboard(tries + 1); });
        } else {
            finish("FAIL never aboard the frame (parent " + MyAvatar.getParentID() + ")");
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
        // a kinematic deck moving along +x as a frame, its top at 1 m; the avatar dropped onto it from just above
        box = Entities.addEntity({
            type: "Box", shape: "Cube", name: "carry-trace-deck", position: { x: 0, y: 0.9, z: 0 },
            dimensions: { x: 8, y: 0.2, z: 8 }, color: { red: 90, green: 100, blue: 140 }, dynamic: false,
            collisionless: false, velocity: { x: 2, y: 0, z: 0 }, damping: 0, frame: true, lifetime: LIFETIME,
            grab: { grabbable: false }
        });
        MyAvatar.position = { x: 0, y: 1.2, z: 0 };
        MyAvatar.orientation = Quat.fromPitchYawRollDegrees(0, 0, 0);
        Camera.mode = "third person";
        after(1, function () { waitAboard(0); });
    }

    after(2, function () { start(0); });
}());
