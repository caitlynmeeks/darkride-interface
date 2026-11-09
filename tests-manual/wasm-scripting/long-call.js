//
//  long-call.js
//  tests-manual/wasm-scripting
//
//  A client script with one call that runs long (2.5 s of its own time, in a timer) and then returns. On the
//  WebAssembly engine a client script past the call limit is warned about in the log once and goes on (an entity
//  script would be stopped); this prints LONG-CALL lines to show it went on.
//
//  Run (a 1 s limit, so the test is quick):
//    DARKRIDE_SCRIPT_CALL_LIMIT_MS=1000 tests-manual/wasm-scripting/run-client.sh long-call 60 \
//        --url file://$PWD/tests-manual/wasm-scripting/world.json --testScript $PWD/tests-manual/wasm-scripting/long-call.js
//
//  Distributed under the Apache License, Version 2.0.
//

(function () {
    Script.setTimeout(function () {
        var started = Date.now(), spins = 0;
        while (Date.now() - started < 2500) {
            spins++;
        }
        print("LONG-CALL the long call returned after " + (Date.now() - started) + " ms");
        Script.setTimeout(function () {
            print("LONG-CALL the script still runs");
            Test.quit();
        }, 1000);
    }, 3000);
}());
