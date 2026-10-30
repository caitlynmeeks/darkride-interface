//
//  api-guard-d.js
//  tests-manual/wasm-scripting
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The client test script of api-guard-d.sh: adds an entity whose script is api-guard-entity.js (so that script runs on
//  the world's engine), gives it time to print its lines, then stops. Its own lines are API-GUARD-D.
//
(function() {
    function say(text) { print("API-GUARD-D: " + text); }
    var scriptUrl = Script.resolvePath("api-guard-entity.js");
    Script.setTimeout(function() {
        var id = Entities.addEntity({
            type: "Box", name: "api-guard probe", position: { x: 0, y: 1, z: -2 },
            dimensions: { x: 0.3, y: 0.3, z: 0.3 }, script: scriptUrl, lifetime: 60
        });
        say("added " + id + " with " + scriptUrl);
        Script.setTimeout(function() { say("stopping"); Script.stop(); }, 15000);
    }, 6000);
})();
