//
//  runaway-entity.js
//  tests-manual/wasm-scripting
//
//  The time limit per entity script (WASM-SCRIPTING.md, "The time limit per script"), serverless: two entities
//  with embedded JavaScript entity scripts, one that runs away in a timer a second and a half in, one that ticks
//  every half second. The runaway is stopped at the limit (the log says so) and the other keeps ticking (on the
//  QtScript engine before this one, which had no limit, the runaway hung the entities engine and the ticks
//  stopped). This script reads the names back and prints RUNAWAY-TEST lines.
//
//  Run (a 1 s limit, so the test is quick):
//    DARKRIDE_SCRIPT_CALL_LIMIT_MS=1000 tests-manual/wasm-scripting/run-client.sh runaway-entity 60 \
//        --url file://$PWD/tests-manual/wasm-scripting/world.json --testScript $PWD/tests-manual/wasm-scripting/runaway-entity.js
//
//  Distributed under the Apache License, Version 2.0.
//

(function () {
    var lines = [];
    function report(name, value) { lines.push("RUNAWAY-TEST " + name + " = " + JSON.stringify(value)); }

    var tickingScript = "(function () {" +
        "  var self = this, ticks = 0;" +
        "  this.preload = function (id) { self.timer = Script.setInterval(function () { ticks++; Entities.editEntity(id, { name: 'ticking: ' + ticks }); }, 500); };" +
        "  this.ping = function (id) { Entities.editEntity(id, { description: 'pinged at ' + ticks }); };" +
        "  this.unload = function (id) { Script.clearInterval(self.timer); };" +
        "})";
    var runawayScript = "(function () {" +
        "  this.preload = function (id) {" +
        "    Entities.editEntity(id, { name: 'runaway: armed' });" +
        "    Script.setTimeout(function () { Entities.editEntity(id, { name: 'runaway: spinning' }); while (true) {} }, 1500);" +
        "  };" +
        "  this.ping = function (id) { Entities.editEntity(id, { name: 'runaway: pinged' }); };" +
        "  this.unload = function (id) { Entities.editEntity(id, { name: 'runaway: unload called' }); };" +
        "})";

    function box(name, position, script) {
        return Entities.addEntity({
            type: "Box", name: name, position: position, dimensions: { x: 0.5, y: 0.5, z: 0.5 },
            color: { red: 200, green: 120, blue: 120 }, lifetime: 120, script: script
        });
    }
    function nameOf(id) { return Entities.getEntityProperties(id, ["name"]).name; }
    function descriptionOf(id) { return Entities.getEntityProperties(id, ["description"]).description; }
    function ticksOf(name) { var m = /^ticking: (\d+)$/.exec(name); return m ? parseInt(m[1], 10) : -1; }
    function finish() {
        for (var i = 0; i < lines.length; i++) { print(lines[i]); }
        print("RUNAWAY-TEST done");
        Test.quit();
    }

    function run() {
    var ticking = box("ticking: pending", { x: 0, y: 2, z: -3 }, tickingScript);
    var runaway = box("runaway: pending", { x: 2, y: 2, z: -3 }, runawayScript);

    // 1.5 s in the runaway spins; under a 1 s limit it is stopped by about 2.5 s, and the ticker goes on
    Script.setTimeout(function () {
        report("names at 1 s", [nameOf(ticking), nameOf(runaway)]);
    }, 1000);
    Script.setTimeout(function () {
        var tickName = nameOf(ticking);
        var ticks = ticksOf(tickName);
        report("names at 6 s", [tickName, nameOf(runaway)]);
        report("ticks at 6 s (about 12 expected; about 3 when the runaway froze the engine)", ticks);
        report("ticker went on after the runaway", ticks >= 9);
        report("runaway was stopped mid-spin", nameOf(runaway) === "runaway: spinning");
        Entities.callEntityMethod(ticking, "ping", []);
        Entities.callEntityMethod(runaway, "ping", []);
        Script.setTimeout(function () {
            report("names after a ping to each", [nameOf(ticking), nameOf(runaway)]);
            report("ticker answered the ping", descriptionOf(ticking));
            report("stopped runaway ignored the ping", nameOf(runaway) !== "runaway: pinged");
            report("stopped runaway's unload method not called", nameOf(runaway) !== "runaway: unload called");
            finish();
        }, 1000);
    }, 6000);
    }

    // the serverless world takes a moment to be ready for entities, as entity-scripts.js found
    Script.setTimeout(run, 8000);
})();
