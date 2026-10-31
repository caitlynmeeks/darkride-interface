//
//  frame-trace.js
//  tests-manual/frame-trace
//
//  The frame trace's one-line test (team/performance/INSTRUMENTS-PLAN.md item 1): switch it on, run three seconds,
//  dump it, and check that the dump has the frame's own columns. Run a client with
//      --testScript tests-manual/frame-trace/frame-trace.js quitWhenFinished
//  and read the log for "FT ..." lines: "FT columns frame,t_us,frame.dt_ms,frame.idle_ms,..." (the other
//  instruments' columns follow once their owners write them), "FT rows N" (about three seconds of frames) and
//  "FT dump <path> rows N" (the CSV, in the test results folder or /tmp). FT_SECONDS in the script's scope sets the
//  length, FT_PATH the CSV, FT_PICTURE a window picture taken with the Stats panel expanded (the "Frame trace" box);
//  a world-less client is enough.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

var seconds = (typeof FT_SECONDS !== "undefined") ? FT_SECONDS : 3;
var path = (typeof FT_PATH !== "undefined") ? FT_PATH : "/tmp/frame-trace.csv";
var picture = (typeof FT_PICTURE !== "undefined") ? FT_PICTURE : "";   // a window picture with the Stats box, if wanted

Test.frameTrace(true);
if (picture) {
    // the Stats panel, expanded, so the "Frame trace" box shows; the picture a second before the dump
    Menu.setIsOptionChecked("Show Statistics", true);
    Stats.expanded = true;
    Script.setTimeout(function () {
        print("FT picture " + picture + " " + (Test.saveWindowPicture(picture) ? "saved" : "FAILED"));
    }, Math.max(1000, seconds * 1000 - 1000));
}
Script.setTimeout(function () {
    var written = Test.dumpFrameTrace(path);
    var rows = Test.takeFrameTrace();
    var columns = rows.length > 0 ? rows[0] : [];
    Test.frameTrace(false);
    print("FT columns " + columns.join(","));
    print("FT rows " + (rows.length - 1));
    print("FT dump " + path + " rows " + written);
    var ok = written > seconds * 20 && columns.indexOf("frame.dt_ms") >= 0 && columns.indexOf("frame.idle_ms") >= 0;
    print("FT " + (ok ? "PASS" : "FAIL"));
    Test.quit();
}, seconds * 1000);
