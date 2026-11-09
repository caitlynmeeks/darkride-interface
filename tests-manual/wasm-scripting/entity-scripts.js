//
//  entity-scripts.js
//  tests-manual/wasm-scripting
//
//  Round 4 of WASM-SCRIPTING.md, serverless: an entity with a JavaScript entity script (embedded) and one
//  with a .wasm entity script (tools/wasm-sdk/examples/c/entity.wasm, built by its build.sh), on whichever
//  engine the entities engine is (the setting). Each script shows what happened in its entity's name; this
//  script reads the names back and prints ENTITY-TEST lines. Collisions: a dynamic box dropped on each.
//
//  Run:  tests-manual/wasm-scripting/run-client.sh entity-scripts 60 \
//            --url file://$PWD/tests-manual/wasm-scripting/world.json --testScript $PWD/tests-manual/wasm-scripting/entity-scripts.js
//
//  Distributed under the Apache License, Version 2.0.
//

(function () {
    var BASE = { x: 0, y: 2, z: -3 };
    var lines = [];
    function report(name, value) { lines.push(name + " = " + JSON.stringify(value)); }

    var jsScript = "(function () {" +
        "  var self = this;" +
        "  this.remotelyCallable = ['ping'];" +
        "  this.preload = function (id) { self.id = id; Entities.editEntity(id, { name: 'js: loaded' }); };" +
        "  this.ping = function (id, params) { Entities.editEntity(id, { name: 'js: pinged with ' + params.join(',') }); };" +
        "  this.collisionWithEntity = function (id, other, collision) { Entities.editEntity(id, { name: 'js: collided (' + collision.type + ')' }); };" +
        "  this.clickDownOnEntity = function (id, event) { Entities.editEntity(id, { name: 'js: clicked' }); };" +
        "  this.unload = function (id) { };" +
        "})";
    var wasmUrl = Script.resolvePath("../../tools/wasm-sdk/examples/c/entity.wasm");

    function box(name, position, extra) {
        var props = {
            type: "Box", name: name, position: position, dimensions: { x: 0.5, y: 0.5, z: 0.5 },
            color: { red: 120, green: 120, blue: 200 }, lifetime: 120, collisionless: false
        };
        for (var k in extra) { props[k] = extra[k]; }
        return Entities.addEntity(props);
    }

    function nameOf(id) { return Entities.getEntityProperties(id, ["name"]).name; }

    function run() {
        var jsBox = box("js: pending", BASE, { script: jsScript });
        var wasmBox = box("wasm: pending", { x: 2, y: 2, z: -3 }, { script: wasmUrl });
        report("wasm script url tail", wasmUrl.replace(/^.*examples\//, ""));

        Script.setTimeout(function () {
            report("js after load", nameOf(jsBox));
            report("wasm after load", nameOf(wasmBox));
            Entities.callEntityMethod(jsBox, "ping", ["hello", "js"]);
            Entities.callEntityMethod(wasmBox, "ping", ["hello", "wasm"]);
            Script.setTimeout(function () {
                report("js after ping", nameOf(jsBox));
                report("wasm after ping", nameOf(wasmBox));
                // a dynamic box dropped on each: collisionWithEntity on the scripted boxes
                box("dropped on js", { x: BASE.x, y: BASE.y + 2, z: BASE.z }, { dynamic: true, gravity: { x: 0, y: -9.8, z: 0 } });
                box("dropped on wasm", { x: 2, y: 4, z: -3 }, { dynamic: true, gravity: { x: 0, y: -9.8, z: 0 } });
                Script.setTimeout(function () {
                    report("js after collision", nameOf(jsBox));
                    report("wasm after collision", nameOf(wasmBox));
                    Entities.deleteEntity(jsBox);
                    Entities.deleteEntity(wasmBox);
                    Script.setTimeout(function () {
                        for (var i = 0; i < lines.length; i++) { print("ENTITY-TEST " + lines[i]); }
                        print("ENTITY-TEST done");
                        Test.quit();
                    }, 1000);
                }, 4000);
            }, 1500);
        }, 3000);
    }

    Script.setTimeout(run, 8000);
}());
