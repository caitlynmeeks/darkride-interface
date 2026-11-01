//
//  network-rules.js
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  A --testScript for the network rule for a script's own requests (ScriptNetworkPolicy): what XMLHttpRequest and
//  WebSocket reach from a script, on both engines. Run by network-rules.sh twice: as the local script it is (every
//  address reachable, as before), and with DARKRIDE_SCRIPT_NETWORK_STRICT=1, which holds it to the rule as a script
//  from the network (this machine's addresses and file:// refused, with one line in the log each; a public host
//  reached after its name is looked up; a require of a local JSON and an include of a local script refused too), then
//  times 20 requests to a public host. Prints NETWORK-RULES lines and quits. The echo server on 127.0.0.1:43110 is
//  http-echo.py, started by network-rules.sh.
//

(function () {
    var ECHO_PORT = 43110;
    var WAIT_MS = 10000;
    var pending = 1;        // one for the registering of the probes, so a probe that ends at once does not end the run
    var finished = false;

    function done(name, outcome) {
        print("NETWORK-RULES " + name + ": " + outcome);
        if (--pending === 0) {
            timeRequests(finish);
        }
    }

    function finish() {
        if (!finished) {
            finished = true;
            print("NETWORK-RULES done; the client quits now");
            Script.stop();
        }
    }

    // performance's line (10-30): the time of 20 requests in a row to one public host, with and without the rule
    // (the strict run holds this script to it; the plain run does not), expected the same within a millisecond
    // after the first, since the name lookup is cached
    var TIMED_URL = "https://example.com/";
    var TIMED_COUNT = 20;
    function timeRequests(callback) {
        var times = [];
        var started;
        function next() {
            if (times.length === TIMED_COUNT) {
                var total = 0;
                for (var i = 0; i < times.length; i++) { total += times[i]; }
                print("NETWORK-RULES timing: " + TIMED_COUNT + " requests to " + TIMED_URL + ": first " + times[0] +
                    " ms, the rest " + ((total - times[0]) / (times.length - 1)).toFixed(1) + " ms each on average, " +
                    total + " ms in all");
                callback();
                return;
            }
            var r = new XMLHttpRequest();
            started = Date.now();
            r.onreadystatechange = function () {
                if (r.readyState === 4) {
                    times.push(Date.now() - started);
                    next();
                }
            };
            r.timeout = WAIT_MS;
            r.ontimeout = function () { times.push(WAIT_MS); next(); };
            r.open("GET", TIMED_URL, true);
            r.send();
        }
        next();
    }

    function request(name, url) {
        pending++;
        var answered = false;
        var r = new XMLHttpRequest();
        r.onreadystatechange = function () {
            if (r.readyState === 4 && !answered) {
                answered = true;
                var bytes = r.responseText ? r.responseText.length : 0;
                done(name, "status " + r.status + ", error " + r.errorCode + ", " + bytes + " bytes" +
                    (r.errorCode === 201 ? " REFUSED" : (bytes > 0 ? " READ" : " nothing")));
            }
        };
        r.timeout = WAIT_MS;
        r.ontimeout = function () { if (!answered) { answered = true; done(name, "timed out"); } };
        try {
            r.open("GET", url, true);
            r.send();
        } catch (e) {
            if (!answered) { answered = true; done(name, "threw " + e); }
        }
    }

    function socket(name, url) {
        pending++;
        var answered = false;
        function once(outcome) {
            if (!answered) {
                answered = true;
                done(name, outcome);
            }
        }
        try {
            var s = new WebSocket(url);
            s.onopen = function () { once("OPENED"); s.close(); };
            s.onerror = function () { once("error (refused, or no websocket there)"); };
            s.onclose = function (event) { once("closed " + (event ? event.code : "?")); };
            Script.setTimeout(function () { once("no answer in " + (WAIT_MS / 1000) + " s"); }, WAIT_MS);
        } catch (e) {
            once("threw " + e);
        }
    }

    function requireAndInclude() {
        // require of a JSON file hands its content to the script; include runs the file: both are requests too
        pending++;
        try {
            var world = Script.require("http://127.0.0.1:" + ECHO_PORT + "/world.json");
            done("require a local json", (world && world.Entities ? "READ (" + world.Entities.length + " entities)" : "no content"));
        } catch (e) {
            done("require a local json", "threw " + e + (String(e).indexOf("Refused") >= 0 ? " REFUSED" : ""));
        }
        // a public JSON module: its download goes to the address judged, by name (the certificate checked by the name)
        pending++;
        try {
            var pkg = Script.require("https://cdn.jsdelivr.net/npm/lodash@4.17.21/package.json");
            done("require a public json", pkg && pkg.name === "lodash" ? "READ" : "no content");
        } catch (e) {
            done("require a public json", "threw " + e);
        }
        pending++;
        var includeDone = false;
        Script.include("http://127.0.0.1:" + ECHO_PORT + "/network-rules-included.js", function () {
            if (!includeDone) {
                includeDone = true;
                done("include a local script", this.NETWORK_RULES_INCLUDED ? "RAN" : "did not run (refused or failed)");
            }
        });
        Script.setTimeout(function () {
            if (!includeDone) {
                includeDone = true;
                done("include a local script", this.NETWORK_RULES_INCLUDED ? "RAN" : "did not run (refused or failed)");
            }
        }, 4000);
    }

    function runtimeInternals() {
        // security's probes (10-30): the engine's request and socket makers must not be reachable by name, and the
        // runtime's own fetch helper must follow the same rules as include and require
        print("NETWORK-RULES Http object: " + (typeof Http === "undefined" ? "hidden" : "VISIBLE"));
        print("NETWORK-RULES Sockets object: " + (typeof Sockets === "undefined" ? "hidden" : "VISIBLE"));
        if (typeof Script.fetchScript !== "function") {
            print("NETWORK-RULES fetchScript: no such method (the QtScript engine)");
            return;
        }
        ["file:///etc/hosts", "http://127.0.0.1:" + ECHO_PORT + "/world.json"].forEach(function (url) {
            try {
                var fetched = Script.fetchScript(url, false);
                print("NETWORK-RULES fetchScript " + url + ": success " + (fetched && fetched.success) + ", status " +
                    (fetched && fetched.status) + (fetched && fetched.contents ? " READ" : " nothing"));
            } catch (e) {
                print("NETWORK-RULES fetchScript " + url + ": threw " + e);
            }
        });
    }

    function settingsWrites() {
        // security's #4 (10-30): a script from the network may not write the settings that steer the client
        var probes = [["Developer/networkRulesProbe", "steering"], ["Script Example/networkRulesProbe", "ordinary"]];
        probes.forEach(function (probe) {
            var key = probe[0], stamp = "probe " + Date.now();
            Settings.setValue(key, stamp);
            var stuck = Settings.getValue(key) === stamp;
            print("NETWORK-RULES settings write to a " + probe[1] + " key (" + key + "): " + (stuck ? "WRITTEN" : "not written (refused)"));
            if (stuck) { Settings.setValue(key, null); }
        });
    }

    print("NETWORK-RULES start");
    runtimeInternals();
    settingsWrites();
    requireAndInclude();
    request("xhr this machine by address", "http://127.0.0.1:" + ECHO_PORT + "/");
    request("xhr this machine by name", "http://localhost:" + ECHO_PORT + "/");
    request("xhr a local file", "file:///etc/hosts");
    request("xhr a public host", "https://example.com/");
    socket("websocket this machine", "ws://127.0.0.1:" + ECHO_PORT + "/");
    socket("websocket a public host", "wss://ws.postman-echo.com/raw");   // echo.websocket.events has no address any more (10-30)
    done("all probes registered", "the run ends when the last one answers");
    Script.setTimeout(finish, WAIT_MS * 4);   // whatever is still pending
})();
