//
//  kit-client.js
//  tests-manual/make-link
//
//  The walled client kit's test script, for a client whose Make connects by itself and says no to every ask. Once
//  Make's tools have made "Make kit box", it prints the Running Scripts list, one line each ("MAKE KIT RUNNING <url>",
//  then "MAKE KIT RUNNING END <count>"): Make's own engine (about:Make) must not be in it, so Reload all and Stop all
//  can't reach it either (they walk the same list). It quits when the MCP kit adds "Make kit done", or after 8 minutes.
//    tests-manual/wasm-scripting/run-client.sh <name> 600 --url hifi://localhost:<port> --testScript <this file>
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    var started = Date.now();
    var listed = false;
    print("MAKE KIT CLIENT started");
    var timer = Script.setInterval(function () {
        var here = MyAvatar.position;
        if (!listed && Entities.findEntitiesByName("Make kit box", here, 500, true).length > 0) {
            listed = true;
            var running = ScriptDiscoveryService.getRunning();
            for (var i = 0; i < running.length; i++) {
                print("MAKE KIT RUNNING " + running[i].url);
            }
            print("MAKE KIT RUNNING END " + running.length);
        }
        var done = Entities.findEntitiesByName("Make kit done", here, 500, true).length > 0;
        if (done || Date.now() - started > 8 * 60 * 1000) {
            Script.clearInterval(timer);
            print("MAKE KIT CLIENT quits after " + Math.round((Date.now() - started) / 1000) + " s" +
                  (done ? "" : " (time limit, no 'Make kit done')"));
            Test.quit();
        }
    }, 1000);
})();
