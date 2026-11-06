//
//  avatar-scripts-midcall.js
//  tests-manual/wasm-scripting
//
//  The driver of avatar-scripts.sh's mid-call check (MIDCALL=1): stops the stand-in avatar's script (probe.js) while
//  it is inside a call (a message makes it spin), three ways, and says how long the script took to stop:
//    1. the avatar taken off: its world is left, so the call ends at once (the plain avatar's load and well under the
//       3 s a deadline would take: under 2.5 s);
//    2. a plain stop during an 8 s call: the call may finish to the scripts' ending deadline, 3 s, and is ended there;
//    3. a plain stop during a 1 s call: the call finishes ("spin ended" once, from this one alone).
//  The avatar is worn again before 2 and 3 (after the plain one, as wearing the same one again loads nothing). Prints
//  AVATAR-SCRIPT DRIVER lines and stops the client.
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//

(function () {
    var URL = "http://localtest.me:43110/av/probe.fst";
    var PLAIN_URL = "http://localtest.me:43110/av/plain.fst";
    var POLL_MS = 25;
    function say(text) { print("AVATAR-SCRIPT DRIVER " + text); }
    function probe() {
        return ScriptDiscoveryService.getRunning().filter(function (script) { return script.name === "probe.js"; })[0];
    }
    // calls then() once the probe runs (true) or is gone (false), or with null after 15 s
    function whenProbe(running, then) {
        var started = Date.now();
        var timer = Script.setInterval(function () {
            if (!!probe() === running || Date.now() - started > 15000) {
                Script.clearInterval(timer);
                then(!!probe() === running ? Date.now() - started : null);
            }
        }, POLL_MS);
    }
    // the probe spins for spinMs; after 500 ms into it, stop() stops it; says how long it took to go, and whether in range
    function stopMidCall(name, spinMs, stop, lowMs, highMs, then) {
        Messages.sendLocalMessage("avatar-probe-spin", String(spinMs));
        Script.setTimeout(function () {
            say(name + ": stopping it mid-call");
            stop();
            whenProbe(false, function (ms) {
                say(name + "-stopped after " + ms + " ms " + (ms !== null && ms >= lowMs && ms < highMs ? "PASS" : "FAIL") +
                    " (expect " + lowMs + " to " + highMs + ")");
                then();
            });
        }, 500);
    }
    // wears the probe; after a plain stop the probe is still worn (without its script), so the plain avatar comes first:
    // wearing the same avatar again loads nothing
    function wear(then) {
        if (MyAvatar.skeletonModelURL === URL) {
            MyAvatar.useFullAvatarURL(PLAIN_URL, "plain");
            Script.setTimeout(function () { wear(then); }, 2000);
            return;
        }
        say("wearing " + URL);
        MyAvatar.useFullAvatarURL(URL, "probe");
        whenProbe(true, function (ms) {
            say("probe running " + (ms !== null ? "PASS" : "FAIL"));
            Script.setTimeout(then, 3000);  // its own checks are done in 1.5 s
        });
    }
    function plainStop() {
        var running = probe();
        if (running) {
            ScriptDiscoveryService.stopScript(running.url, false);
        }
    }
    Script.setTimeout(function () {
        wear(function () {
            stopMidCall("takeoff", 8000, function () { MyAvatar.useFullAvatarURL(PLAIN_URL, "plain"); }, 0, 2500, function () {
                wear(function () {
                    stopMidCall("plain-stop", 8000, plainStop, 2000, 4500, function () {
                        wear(function () {
                            stopMidCall("quick-stop", 1000, plainStop, 300, 2000, function () {
                                say("done");
                                Script.stop();
                            });
                        });
                    });
                });
            });
        });
    }, 4000);
})();
