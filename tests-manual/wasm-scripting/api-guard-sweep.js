//
//  api-guard-sweep.js
//  tests-manual/wasm-scripting
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The client test script of api-guard-sweep.sh: loads the live worlds' client entity scripts (copied into
//  results/api-guard-sweep-scripts) as entity scripts, so they run on the world's engine under the script API policy,
//  and gives them time to run their preload paths. The shell collects the refusal lines. Its own lines are API-GUARD-SWEEP.
//
(function() {
    function say(text) { print("API-GUARD-SWEEP: " + text); }
    var folder = Script.resolvePath("results/api-guard-sweep-scripts/");
    var scripts = ["darkrover-pad-client.js", "music-button.js", "sitClient.js"];
    Script.setTimeout(function() {
        scripts.forEach(function(name, i) {
            var id = Entities.addEntity({
                type: "Box", name: "sweep " + name, position: { x: i * 1.5 - 1.5, y: 1, z: -2 },
                dimensions: { x: 0.3, y: 0.3, z: 0.3 }, script: folder + name, lifetime: 120,
                userData: JSON.stringify({ seat: { position: { x: 0, y: 0.5, z: 0 } } })
            });
            say("added " + name + " as " + id);
        });
        Script.setTimeout(function() { say("stopping"); Script.stop(); }, 25000);
    }, 6000);
})();
