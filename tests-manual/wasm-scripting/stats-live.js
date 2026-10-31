//
//  stats-live.js
//  tests-manual/wasm-scripting
//
//  Created by scripting on 2026-10-31.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The one-line test of Stats values live without the panel (team/performance/INSTRUMENTS-PLAN.md item 8). With the
//  Stats panel hidden: two snapshots a second apart before any reader must match (frozen; a forced update early in the
//  client's life, the Tivoli bar's overview, fills them, so they need not be zero); a second after Test.statsLive(true)
//  the rates are live and the frame trace's lines (the trace is on from the start) have moved on; an expanded-only text has not
//  moved; two seconds after the reader leaves the values match the last live read (frozen again). Every line is
//  STATS-LIVE; the verdict line is PASS or FAIL. Run through stats-live.sh (one client, about 40 s, no input).
//
(function () {
    function say(text) { print("STATS-LIVE: " + text); }
    // the values a reader keeps live (the render details, triangles and the like, come from the renderer every frame
    // whether or not anyone reads, so they stay out of the frozen comparisons)
    var WATCHED = ["gameLoopRate", "renderrate", "presentrate", "serverCount", "avatarCount", "position", "frameTraceLines"];
    function watched(snapshot) {
        var picked = {};
        WATCHED.forEach(function (key) { picked[key] = snapshot[key]; });
        return JSON.stringify(picked);
    }
    var before1, before2, live1, live2, after;

    Test.frameTrace(true);   // the frame trace's lines are the live signal: they move on only when a Stats pass runs
    Script.setTimeout(function () {
        before1 = Test.statsSnapshot();
        say("panel hidden, no reader, 5 s: gameLoopRate " + before1.gameLoopRate + " renderrate " + before1.renderrate +
            " keys " + Object.keys(before1).length + " frameTraceLines " + before1.frameTraceLines.length);
    }, 5000);
    Script.setTimeout(function () {
        before2 = Test.statsSnapshot();
        say("no reader, 6 s: frozen " + (watched(before2) === watched(before1) ? "yes" : "NO") + " lodStatus '" + before2.lodStatus + "'");
        Test.statsLive(true);
        say("reader on");
    }, 6000);
    Script.setTimeout(function () {
        live1 = Test.statsSnapshot();
        say("reader on, 1 s: gameLoopRate " + live1.gameLoopRate + " renderrate " + live1.renderrate +
            " Stats.renderrate " + Stats.renderrate + " frameTraceLines " + live1.frameTraceLines.length +
            " timingStats '" + live1.timingStats + "' lodStatus moved " + (live1.lodStatus !== before2.lodStatus ? "YES" : "no"));
    }, 7000);
    Script.setTimeout(function () {
        live2 = Test.statsSnapshot();
        Test.statsLive(false);
        say("reader on, 3 s: gameLoopRate " + live2.gameLoopRate + " renderrate " + live2.renderrate +
            " numNeededEntityUpdates " + live2.numNeededEntityUpdates + " position " + JSON.stringify(live2.position));
        say("reader off");
    }, 9000);
    Script.setTimeout(function () {
        after = Test.statsSnapshot();
        Test.frameTrace(false);
        say("reader off, 2 s: gameLoopRate " + after.gameLoopRate + " frozen again " + (watched(after) === watched(live2) ? "yes" : "NO"));
        var ok = watched(before2) === watched(before1)              // frozen before the reader
            && live1.gameLoopRate > 0 && live1.renderrate > 0         // live within a second of the reader
            && live1.frameTraceLines.length > 0                            // a pass ran for the reader: the trace's
            && JSON.stringify(live1.frameTraceLines) !== JSON.stringify(before2.frameTraceLines)   // lines moved on
            && live1.timingStats === "" && live1.lodStatus === before2.lodStatus          // expanded-only parts untouched
            && watched(after) === watched(live2)                      // frozen again without the reader
            && Object.keys(live1).length > 50;                        // the whole property set came
        say(ok ? "PASS" : "FAIL");
        say("stopping");
        Script.stop();
    }, 11000);
})();
