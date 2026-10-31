//
//  physics-trace.js
//  tests-manual/physics-baseline
//
//  The frame trace's physics columns (team/performance/INSTRUMENTS-PLAN.md, item 4; PhysicsEngine::stepSimulation):
//  two seconds traced standing in the world, then checked: physics.substeps, physics.alpha and physics.lead_ms in every
//  frame, the substeps whole numbers from 0 to the cap, the alpha between 0 and 1, and the substeps adding up to the
//  simulation's rate (90 a second) within a tenth. Prints PHYSICS-TRACE lines ending in PASS or FAIL; the trace is
//  dumped as physics-trace.csv in the results location.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    var config = (typeof PHYSICS_CONFIG !== "undefined" && PHYSICS_CONFIG) ? PHYSICS_CONFIG : {};
    var RESULTS = config.results || "";
    var TRACE_SECONDS = 2;
    var SUBSTEPS_PER_SECOND = 90;
    var COLUMNS = ["physics.substeps", "physics.alpha", "physics.lead_ms"];

    function log(s) { print("PHYSICS-TRACE " + s); }
    function after(seconds, f) { Script.setTimeout(f, seconds * 1000); }
    function finish(verdict) {
        log(verdict);
        after(1, function () { Test.quit(); });
    }

    function check(traced) {
        var trace = Test.takeFrameTrace();
        var columns = trace.length ? trace[0] : [];
        var rows = trace.slice(1);
        var index = {};
        columns.forEach(function (name, i) { index[name] = i; });
        var missing = COLUMNS.filter(function (name) { return index[name] === undefined; });
        var written = RESULTS ? Test.dumpFrameTrace(RESULTS + "/physics-trace.csv") : -1;
        log(rows.length + " frames traced in " + traced.toFixed(2) + " s, " + columns.length + " columns, " + written + " rows dumped");
        if (missing.length) {
            finish("FAIL columns missing: " + missing.join(" "));
            return;
        }
        var frames = 0, total = 0, bad = 0, histogram = {};
        rows.forEach(function (row) {
            var n = row[index["physics.substeps"]], alpha = row[index["physics.alpha"]], lead = row[index["physics.lead_ms"]];
            if (typeof n !== "number" || typeof alpha !== "number" || typeof lead !== "number") { bad++; return; }
            if (n !== Math.floor(n) || n < 0 || n > 8 || alpha < 0 || alpha > 1.0001) { bad++; return; }
            frames++;
            total += n;
            histogram[n] = (histogram[n] || 0) + 1;
        });
        var rate = total / traced;
        log(frames + " frames with the three columns, " + bad + " without or out of range; substeps per frame " +
            Object.keys(histogram).sort().map(function (k) { return k + ":" + histogram[k]; }).join(" ") +
            "; " + total + " substeps = " + rate.toFixed(1) + " a second");
        if (frames < 30 || bad > frames / 20) {
            finish("FAIL " + bad + " of " + (frames + bad) + " frames without the columns or out of range");
        } else if (Math.abs(rate - SUBSTEPS_PER_SECOND) > SUBSTEPS_PER_SECOND / 10) {
            finish("FAIL the substeps add up to " + rate.toFixed(1) + " a second, not " + SUBSTEPS_PER_SECOND);
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
        MyAvatar.position = { x: 0, y: 1, z: 0 };
        after(2, function () {
            var t0 = Date.now();
            log("tracing " + TRACE_SECONDS + " s");
            Test.frameTrace(true);
            after(TRACE_SECONDS, function () {
                Test.frameTrace(false);
                check((Date.now() - t0) / 1000);
            });
        });
    }

    after(2, function () { start(0); });
}());
