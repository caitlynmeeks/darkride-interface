//
//  avatar-scripts.js
//  tests-manual/wasm-scripting
//
//  The driver of the avatar-scripts check (avatar-scripts.sh): the visitor's own test script, which wears the stand-in
//  avatar probe.fst from the kit's own server, waits for its script lines to load, checks the avatar is still that one
//  (the avatar's script tried to change it) and that the visitor's own scripts keep what a world's script is refused,
//  then stops the client. The avatar's own script (avatar-scripts/av/probe.js) prints the AVATAR-SCRIPT checks.
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//

(function () {
    var URL = "http://localtest.me:43110/av/probe.fst";
    function say(text) { print("AVATAR-SCRIPT DRIVER " + text); }
    Script.setTimeout(function () {
        var running = ScriptDiscoveryService.getRunning();
        say("own-scripts-keep-ScriptDiscoveryService " + (running && typeof running.length === "number" ? "PASS" : "FAIL"));
        say("Menu-for-own-scripts " + (typeof Menu.menuExists("Edit") === "boolean" ? "PASS" : "FAIL"));
        say("wearing " + URL);
        MyAvatar.useFullAvatarURL(URL, "probe");
    }, 4000);
    Script.setTimeout(function () {
        var url = MyAvatar.skeletonModelURL;
        say("avatar-unchanged " + (url === URL ? "PASS" : "FAIL " + url));
        say("done");
        Script.stop();
    }, 45000);
})();
