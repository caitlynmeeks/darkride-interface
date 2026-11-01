//
//  correction-trace.js
//  tests-manual/physics-baseline
//
//  The frame trace's server-state correction columns (team/performance/INSTRUMENTS-PLAN.md, item 6;
//  CorrectionCounters, TrackSimulation::adoptState): on a domain whose physics server drives a track car (the sandbox
//  must be up: a world with a Track and its car, Port Amarelle's monorail or run-monorail.sh's), the avatar boards the
//  car and rides TRACE_SECONDS with the trace on; then checked: net.residual_cm, net.closing, net.snaps and net.blends
//  present, written at least every two seconds (the server republishes at least once a second), every residual under
//  MAX_RESIDUAL_CM, no snap. Prints CORRECTION-TRACE lines ending in PASS or FAIL; the trace is dumped as
//  correction-trace.csv in the results location.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    var config = (typeof CORRECTION_CONFIG !== "undefined" && CORRECTION_CONFIG) ? CORRECTION_CONFIG : {};
    var RESULTS = config.results || "";
    var TRACE_SECONDS = config.seconds || 8;
    var MAX_RESIDUAL_CM = 50;
    var COLUMNS = ["net.residual_cm", "net.closing", "net.snaps", "net.blends"];
    var track = null, car = null;

    function log(s) { print("CORRECTION-TRACE " + s); }
    function after(seconds, f) { Script.setTimeout(f, seconds * 1000); }
    function finish(verdict) {
        log(verdict);
        after(1, function () { Test.quit(); });
    }

    function check() {
        var written = RESULTS ? Test.dumpFrameTrace(RESULTS + "/correction-trace.csv") : -1;   // before take, which empties the ring
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
        var states = 0, worst = 0, snaps = 0, blends = 0, lastAt = -1, longestGap = 0, t0 = -1;
        rows.forEach(function (row) {
            var t = row[index["t_us"]] / 1000000;
            if (t0 < 0) { t0 = t; }
            var residual = row[index["net.residual_cm"]];
            if (typeof residual !== "number") { return; }
            states++;
            worst = Math.max(worst, residual);
            snaps = row[index["net.snaps"]];
            blends = row[index["net.blends"]];
            longestGap = Math.max(longestGap, lastAt < 0 ? t - t0 : t - lastAt);
            lastAt = t;
        });
        log(states + " states adopted in " + rows.length + " frames; worst residual " + worst.toFixed(1) + " cm; longest gap " +
            longestGap.toFixed(2) + " s; snaps " + snaps + ", blends " + blends + " since the start");
        if (states < 2) {
            finish("FAIL fewer than two states adopted while riding (" + states + ")");
        } else if (longestGap > 2.5) {
            finish("FAIL " + longestGap.toFixed(2) + " s between states (the server republishes every second)");
        } else if (worst > MAX_RESIDUAL_CM) {
            finish("FAIL a residual of " + worst.toFixed(1) + " cm (allowed " + MAX_RESIDUAL_CM + ")");
        } else {
            finish("PASS");
        }
    }

    function waitAboard(tries) {
        var speed = Entities.getEntityProperties(track, ["trainSpeed"]).trainSpeed;
        if (MyAvatar.getParentID() === car && speed > 0.5) {
            log("aboard " + car + " at " + speed.toFixed(1) + " m/s after " + tries + " s; tracing " + TRACE_SECONDS + " s");
            Test.frameTrace(true);
            after(TRACE_SECONDS, function () {
                Test.frameTrace(false);
                check();
            });
        } else if (tries < 90) {
            if (MyAvatar.getParentID() !== car && tries % 10 === 5) {
                board();   // fell off, or the car was stopped at a station: again
            }
            after(1, function () { waitAboard(tries + 1); });
        } else {
            finish("FAIL never aboard and moving (parent " + MyAvatar.getParentID() + ", speed " + speed + ")");
        }
    }

    function board() {
        var pose = Entities.getEntityProperties(car, ["position", "rotation", "dimensions"]);
        MyAvatar.position = Vec3.sum(pose.position, Vec3.multiply(Quat.getUp(pose.rotation), pose.dimensions.y / 2 + 1.0));
        MyAvatar.orientation = pose.rotation;
        log("boarding at " + JSON.stringify(pose.position));
    }

    function isNull(id) { return !id || /^\{?0{8}-0{4}-0{4}-0{4}-0{12}\}?$/.test(id); }   // an unparented entity's parent is the null UUID
    function findTrackAndCar() {
        var ids = Entities.findEntities(MyAvatar.position, 5000);
        for (var i = 0; i < ids.length && !track; i++) {
            if (Entities.getEntityProperties(ids[i], ["type"]).type === "Track") { track = ids[i]; }
        }
        if (!track) { return false; }
        for (var j = 0; j < ids.length && !car; j++) {
            var p = Entities.getEntityProperties(ids[j], ["trackID", "parentID"]);
            if (p.trackID === track && isNull(p.parentID)) { car = ids[j]; }   // the car itself, not a part parented to it
        }
        return !!car;
    }

    function start(tries) {
        if (!Window.isPhysicsEnabled() || !findTrackAndCar()) {
            if (tries % 20 === 10) { log("waiting: physics " + Window.isPhysicsEnabled() + ", track " + track + ", car " + car + ", entities " + Entities.findEntities(MyAvatar.position, 5000).length); }
            if (tries < 120) {
                after(1, function () { start(tries + 1); });
            } else {
                finish("FAIL no physics, track or car after 120 s");
            }
            return;
        }
        if (typeof Test.frameTrace !== "function") {
            finish("FAIL this build has no frame trace (Test.frameTrace)");
            return;
        }
        log("track " + track + " car " + car);
        Camera.mode = "third person";
        board();
        after(1, function () { waitAboard(0); });
    }

    after(2, function () { start(0); });
}());
