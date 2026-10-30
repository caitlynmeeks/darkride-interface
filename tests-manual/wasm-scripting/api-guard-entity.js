//
//  api-guard-entity.js
//  tests-manual/wasm-scripting
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The ENTITY script of api-guard-d.sh: runs on the world's engine (what a world's scripts get on a visitor's client)
//  and probes the script API policy: removed globals are stand-ins, denied members of kept objects are refused,
//  allowed ones work. Prints API-GUARD-ENTITY lines; api-guard-d.js adds the entity that carries it.
//
(function() {
    var self = this;
    function say(text) { print("API-GUARD-ENTITY: " + text); }
    function attempt(name, fn) {
        try {
            var value = fn();
            say(name + ": ok " + (value === undefined ? "" : JSON.stringify(value)));
        } catch (e) {
            say(name + ": refused (" + (e && e.message ? e.message : e) + ")");
        }
    }
    this.preload = function(entityID) {
        self.id = entityID;
        say("engine " + Script.getContext() + ", entity " + entityID);

        // removed globals: a stand-in with the name, nothing else
        attempt("typeof Snapshot", function() { return typeof Snapshot + " " + (Snapshot && Snapshot.refusedName); });
        attempt("Snapshot.takeSnapshot", function() { return Snapshot.takeSnapshot(false, false, 1); });
        attempt("ScriptDiscoveryService.loadScript", function() { return ScriptDiscoveryService.loadScript("https://example.com/x.js"); });
        attempt("Users.kick", function() { return Users.kick("00000000-0000-0000-0000-000000000001"); });
        attempt("File.runUnzip", function() { return File.runUnzip("/tmp/x.zip", "", false, true, false); });
        attempt("Clipboard.exportEntities", function() { return Clipboard.exportEntities("/tmp/x.json", [self.id]); });
        attempt("Menu.triggerOption", function() { return Menu.triggerOption("Quit"); });

        // partial objects: the kept part works, the denied part is refused
        attempt("Window.innerWidth", function() { return Window.innerWidth; });
        attempt("typeof Window.alert", function() { return typeof Window.alert; });
        attempt("Window.browse", function() { return Window.browse("pick", "/tmp"); });
        attempt("Window.openUrl", function() { return Window.openUrl("ssh://example.com/"); });
        attempt("typeof HMD.position", function() { return typeof HMD.position; });
        attempt("HMD.openTablet", function() { return HMD.openTablet(false); });
        attempt("HMD.showTablet = true", function() { HMD.showTablet = true; return HMD.showTablet; });
        attempt("Audio.muted = true", function() { Audio.muted = true; return Audio.muted; });
        attempt("Audio.devices", function() { return typeof Audio.devices; });
        attempt("Audio.getAvatarGain", function() { return Audio.getAvatarGain(); });
        attempt("MyAvatar.position", function() { return typeof MyAvatar.position; });
        attempt("MyAvatar.skeletonModelURL = ...", function() { MyAvatar.skeletonModelURL = "https://example.com/x.fst"; return MyAvatar.skeletonModelURL; });
        attempt("MyAvatar.useFullAvatarURL", function() { return MyAvatar.useFullAvatarURL("https://example.com/x.fst", "x"); });
        attempt("MyAvatar.setCollisionSoundURL", function() { return MyAvatar.setCollisionSoundURL("https://example.com/x.wav"); });
        attempt("AddressManager.hostname", function() { return typeof AddressManager.hostname; });
        attempt("AddressManager.copyAddress", function() { return AddressManager.copyAddress(); });
        attempt("Controller.getValue", function() { return typeof Controller.getValue(Controller.Standard.LX); });
        attempt("Controller.startInputRecording", function() { return Controller.startInputRecording(); });
        attempt("Chat.sendMessage", function() { return Chat.sendMessage("hello from a world script"); });
        var tablet = null;
        attempt("Tablet.getTablet", function() { tablet = Tablet.getTablet("com.highfidelity.interface.tablet.system"); return typeof tablet; });
        attempt("typeof tablet.gotoWebScreen", function() { return typeof tablet.gotoWebScreen; });
        attempt("tablet.addButton", function() { return tablet.addButton({ text: "WORLD" }); });
        attempt("tablet.emitScriptEvent", function() { return tablet.emitScriptEvent("x"); });
        attempt("tablet.buttons", function() { return typeof tablet.buttons; });

        // allowed: content's own needs
        attempt("Entities.getEntityProperties", function() { return Entities.getEntityProperties(self.id, ["name"]).name; });
        attempt("typeof Overlays", function() { return typeof Overlays + " " + typeof Messages + " " + typeof Camera + " " + typeof Picks; });
        attempt("Script.setTimeout", function() { Script.setTimeout(function() { say("timer fired"); }, 10); return "set"; });
        say("done");
    };
    this.unload = function() {};
})
