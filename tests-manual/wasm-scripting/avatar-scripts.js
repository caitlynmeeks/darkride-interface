//
//  avatar-scripts.js
//  tests-manual/wasm-scripting
//
//  The driver of the avatar-scripts check (avatar-scripts.sh): the visitor's own test script, which wears the stand-in
//  avatar probe.fst from the kit's own server, waits for its script lines to load, checks the avatar is still that one
//  (the avatar's script tried to change it) and that the visitor's own scripts keep what a world's script is refused,
//  then takes the avatar off by wearing the same one without its script lines (plain.fst, which avatar-scripts.sh makes),
//  checks its script has stopped, and stops the client. The avatar's own script (avatar-scripts/av/probe.js) prints the
//  AVATAR-SCRIPT checks.
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//

(function () {
    var URL = "http://localtest.me:43110/av/probe.fst";
    var PLAIN_URL = "http://localtest.me:43110/av/plain.fst";
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
        say("taking it off");
        MyAvatar.useFullAvatarURL(PLAIN_URL, "plain");
    }, 45000);
    Script.setTimeout(function () {
        var running = ScriptDiscoveryService.getRunning().filter(function (script) { return script.name === "probe.js"; });
        say("avatar-off " + (MyAvatar.skeletonModelURL === PLAIN_URL ? "PASS" : "FAIL " + MyAvatar.skeletonModelURL));
        say("script-stopped " + (running.length === 0 ? "PASS" : "FAIL"));
        say("done");
        Script.stop();
    }, 60000);
})();
