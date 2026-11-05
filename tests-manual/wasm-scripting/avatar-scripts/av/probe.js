//
//  probe.js
//  tests-manual/wasm-scripting/avatar-scripts/av
//
//  The script line of a stand-in avatar (probe.fst, served by avatar-scripts.sh): what an avatar's script gets on
//  this build. It runs under a world's rules, so the visitor's own things are refused and what an imported avatar's
//  gesture script uses (hand animation overrides, Controller.keyPressEvent, PlatformInfo, Script.resolvePath) works.
//  Prints AVATAR-SCRIPT lines and one RESULT line; the driver (avatar-scripts.js) wears the avatar and stops the client.
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//

(function () {
    var failures = 0;
    function say(text) { print("AVATAR-SCRIPT " + text); }
    function check(name, ok, detail) {
        if (!ok) { failures++; }
        say(name + " " + (ok ? "PASS" : "FAIL") + (detail ? " " + detail : ""));
    }
    // a refused global is a stand-in: its members log a line and give nothing back, or the call throws
    function refused(call) {
        try { var result = call(); return result === undefined || result === null; } catch (e) { return true; }
    }
    say("running from " + Script.resolvePath(""));

    check("refused ScriptDiscoveryService.getRunning", refused(function () {
        var running = ScriptDiscoveryService.getRunning(); return running && typeof running.length === "number" ? running : undefined;
    }));
    check("refused Menu.menuExists", refused(function () {
        var exists = Menu.menuExists("Edit"); return typeof exists === "boolean" ? exists : undefined;
    }));
    check("refused new OverlayWebWindow", refused(function () {
        var window = new OverlayWebWindow({ title: "probe", source: "about:blank", visible: false });
        if (window) { window.close(); }
        return window;
    }));
    // the identity-changing calls: refused here; the driver checks the avatar is still this one afterwards
    try { MyAvatar.useFullAvatarURL("https://cdn.darkride.ai/avatars/lightling-1/lightling.glb", "other"); } catch (e) {}
    say("attempted MyAvatar.useFullAvatarURL");

    // what an imported avatar's gesture script uses (avatars' gestures.js)
    check("allowed MyAvatar hand animation calls",
          typeof MyAvatar.overrideHandAnimation === "function" && typeof MyAvatar.restoreHandAnimation === "function");
    var keys = 0;
    function onKey(event) { keys++; }
    check("allowed Controller.keyPressEvent.connect", !refused(function () { Controller.keyPressEvent.connect(onKey); return true; }));
    check("allowed PlatformInfo.getOperatingSystemType", (function () {
        try { return typeof PlatformInfo.getOperatingSystemType() === "string"; } catch (e) { return false; }
    })());
    check("allowed Script.resolvePath", Script.resolvePath("probe.js").indexOf("/av/probe.js") > 0, Script.resolvePath("probe.js"));
    // an include from the avatar's own folder runs in this engine under the same rules (its download is the network
    // rule's business, as for any script of this origin: a refusal there says so in the log)
    var included = null;
    try { Script.include("helper.js"); included = typeof probeHelper === "object" ? probeHelper : null; } catch (e) { say("include threw " + e); }
    say("include of helper.js " + (included ? "ran, Menu refused inside it: " + included.menuRefused : "did not run (see the log for the reason)"));
    if (included) { check("included code under the same rules", included.menuRefused === true); }
    // its own entities (a world of its own): made, read back, deleted
    var box = Entities.addEntity({ type: "Box", name: "avatar-script probe", position: Vec3.sum(MyAvatar.position, { x: 0, y: 2, z: 0 }),
                                   dimensions: { x: 0.1, y: 0.1, z: 0.1 }, collisionless: true, lifetime: 60 });
    var boxType = box && !Uuid.isNull(box) ? Entities.getEntityProperties(box, ["type"]).type : "none";
    check("allowed its own entity", boxType === "Box", String(boxType));
    if (box) { Entities.deleteEntity(box); }

    // Messages is the world's: a plain channel works, an app channel (io.darkride.*) is refused quietly
    var plain = 0, app = 0;
    function onMessage(channel, message, sender, localOnly) {
        if (channel === "avatar-probe") { plain++; } else if (channel === "io.darkride.avatar-probe") { app++; }
    }
    try {
        Messages.messageReceived.connect(onMessage);
        Messages.subscribe("avatar-probe");
        Messages.subscribe("io.darkride.avatar-probe");
        Messages.sendLocalMessage("avatar-probe", "hello");
        Messages.sendLocalMessage("io.darkride.avatar-probe", "hello");
    } catch (e) {
        say("Messages threw " + e);
    }
    Script.setTimeout(function () {
        check("allowed Messages plain channel", plain === 1, "received " + plain);
        check("refused Messages app channel", app === 0, "received " + app);
        say("RESULT " + (failures === 0 ? "PASS" : "FAIL " + failures));
    }, 1500);

    Script.scriptEnding.connect(function () {
        try { Controller.keyPressEvent.disconnect(onKey); } catch (e) {}
        try { Messages.messageReceived.disconnect(onMessage); } catch (e) {}
        say("ending (" + keys + " keys seen)");
    });
})();
