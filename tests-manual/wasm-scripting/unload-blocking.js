//
//  unload-blocking.js
//  tests-manual/wasm-scripting
//
//  The quit while an entity script's unload handler waits for the main thread (qa, 2026-10-29): High Fidelity's
//  sit script stands the avatar up in unload, and MyAvatar.getAnimationRoles() there is a blocking call into the
//  main thread, which was itself waiting for the unloads; a seated client never quit. Serverless: one entity with an
//  embedded script whose unload makes that call; once the entity script has loaded this script stops, and the client
//  quits (quitWhenFinished). Without the fix the client hangs after "NodeList UUID changed" until run-client.sh's time
//  limit kills it; with it the log shows "UNLOAD-TEST unload saw N animation roles" and the client ends on its own.
//
//  Run:  tests-manual/wasm-scripting/run-client.sh unload-blocking 60 \
//            --url file://$PWD/tests-manual/wasm-scripting/world.json --testScript $PWD/tests-manual/wasm-scripting/unload-blocking.js
//
//  Distributed under the MIT License.
//

(function () {
    var LOADED = "unload-test: loaded";
    var script = "(function () {" +
        "  this.preload = function (id) { Entities.editEntity(id, { name: '" + LOADED + "' }); };" +
        "  this.unload = function (id) {" +
        "    var roles = MyAvatar.getAnimationRoles();" + // a blocking call into the main thread, as sitClient.js's standUp makes
        "    print('UNLOAD-TEST unload saw ' + roles.length + ' animation roles');" +
        "  };" +
        "})";
    function add() {
        return Entities.addEntity({
            type: "Box", name: "unload-test", position: { x: 0, y: 2, z: -3 }, dimensions: { x: 0.5, y: 0.5, z: 0.5 },
            color: { red: 120, green: 200, blue: 120 }, lifetime: 120, script: script
        });
    }
    var id = add();
    var tries = 0;
    var timer = Script.setInterval(function () {
        var name = Entities.getEntityProperties(id, ["name"]).name;
        tries++;
        if (name === undefined && tries % 4 === 0) {
            // the world was not ready for the first add (the QtScript engine starts its script sooner): again
            id = add();
        }
        if (name === LOADED || tries > 40) {
            Script.clearInterval(timer);
            print("UNLOAD-TEST entity script " + (name === LOADED ? "loaded" : "NOT loaded (" + name + ")") + " after " +
                  tries * 0.5 + " s; stopping, the client quits now");
            Script.stop();
        }
    }, 500);
})();
