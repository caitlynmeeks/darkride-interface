//
//  api-guard.js
//  tests-manual/wasm-scripting
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The test script of api-guard.sh: three probes of the script API's guards. A JSON reply is data (an XMLHttpRequest
//  with responseType "json" parses it and never runs it); an archive unzips only when every entry lands inside the
//  target folder; Window.openUrl opens web and world addresses only. Prints API-GUARD lines and stops.
//
(function() {
    var BASE = "http://127.0.0.1:43110/results/api-guard-data/";
    var DATA_DIR = Script.resolvePath("results/api-guard-data").replace("file://", "");
    var pending = 1;
    function say(text) { print("API-GUARD: " + text); }
    function done() {
        if (--pending === 0) {
            Script.setTimeout(function() { Script.stop(); }, 500);
        }
    }
    function jsonRequest(name, file) {
        pending++;
        var request = new XMLHttpRequest();
        request.responseType = "json";
        request.onreadystatechange = function() {
            if (request.readyState !== 4) {
                return;
            }
            var value = request.response;
            say(name + ": status=" + request.status + " response=" + JSON.stringify(value) + " type=" + typeof value);
            done();
        };
        request.open("GET", BASE + file, true);
        request.send();
    }
    jsonRequest("json data", "data.json");    // {"a":1}
    jsonRequest("json code", "code.json");    // a JavaScript expression: null, and no EVALUATED line

    if (typeof File !== "undefined" && File.runUnzip) {
        pending++;
        var results = 0;
        File.unzipResult.connect(function(path, fileList) {
            say("unzip " + path.split("/").pop() + ": " + fileList.length + " files " + JSON.stringify(fileList));
            if (++results === 2) {
                done();
            }
        });
        File.runUnzip(DATA_DIR + "/good.zip", "", false, true, false);      // one file, extracted
        File.runUnzip(DATA_DIR + "/escape.zip", "", false, true, false);    // an entry named ../../: refused whole
    } else {
        say("no File object on this engine");
    }

    Window.openUrl("file:///etc/hosts");    // refused (see the app's lines)
    Window.openUrl("ssh://example.com/");   // refused
    say("openUrl: two refusals asked");

    Script.setTimeout(function() { say("timeout"); Script.stop(); }, 20000);
    say("all probes registered");
    done();
})();
