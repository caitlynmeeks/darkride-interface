//
//  default-scripts.js
//  tests-manual/wasm-scripting
//
//  The default scripts in a test client: loads this checkout's defaultScripts.js (a scripted client goes straight in,
//  signed out, so the default scripts do not load by themselves), lets them run for a minute, prints each running
//  script's name and engine, and quits.
//
//    tests-manual/wasm-scripting/run-client.sh default-scripts-wasm 110 \
//        --url file://$PWD/tests-manual/wasm-scripting/world.json --testScript $PWD/tests-manual/wasm-scripting/default-scripts.js
//
//  Distributed under the Apache License, Version 2.0.
//
(function () {
    var DEFAULTS = Script.resolvePath("../../scripts/system/defaultScripts.js");
    var SECONDS = 60;
    ScriptDiscoveryService.loadOneScript(DEFAULTS);
    print("DEFAULT-SCRIPTS loading " + DEFAULTS);
    Script.setTimeout(function () {
        var running = ScriptDiscoveryService.getRunning();
        print("DEFAULT-SCRIPTS " + running.length + " scripts running after " + SECONDS + " s:");
        for (var i = 0; i < running.length; i++) {
            print("DEFAULT-SCRIPTS   " + running[i].name + " on " + running[i].engine);
        }
        Test.quit();
    }, SECONDS * 1000);
}());
