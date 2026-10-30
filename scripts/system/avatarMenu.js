"use strict";

//  avatarMenu.js
//
//  An imported avatar's menu, with its toggles, buttons and submenus: the choices set the avatar's parameters
//  (MyAvatar.setAvatarParameter), which drive its effects layer: clothes shown or hidden, shape keys, bones.
//  Parameters the avatar marks as saved are remembered per avatar and put back the next time it's worn, and a
//  parameter that starts one of the avatar's emotes plays it. The menu itself is drawn by the Avatar panel
//  (qml/darkride/AvatarPanel.qml, its Menu tab), which sends each choice here on the local channel
//  io.darkride.avatarMenu as {type: "set", name, value}; a script of this client's may send the same.
//
//  Created on 2026-10-14 for Darkride's import of avatars from Unity packages.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT

/* globals Script, MyAvatar, Settings, AnimationCache, Messages */

(function () {
    var SAVED_SETTING = "io.darkride.avatarMenu.saved";  // {avatar URL: {parameter: value}}
    var CHANNEL = "io.darkride.avatarMenu";

    function savedValues() {
        var all = Settings.getValue(SAVED_SETTING, {}) || {};
        return all[MyAvatar.skeletonModelURL] || {};
    }

    function save(name, value) {
        var all = Settings.getValue(SAVED_SETTING, {}) || {};
        var mine = all[MyAvatar.skeletonModelURL] || {};
        mine[name] = value;
        all[MyAvatar.skeletonModelURL] = mine;
        Settings.setValue(SAVED_SETTING, all);
    }

    function hasMenu(menuData) {
        return menuData && menuData.menu && menuData.menu.controls && menuData.menu.controls.length > 0;
    }

    // Emotes: the avatar's emote layer picks one by a parameter's value (MyAvatar.getAvatarMenu().emotes). Its stock
    // animations aren't in the package, so the Emote app's clips stand in, matched by the state's name; the avatar's own
    // emote clips aren't played yet. Others see the emote in the avatar's joints, and hear of it on emote.js's channel.
    var EMOTE_CLIPS = [["wave", "Waving"], ["clap", "Clapping"], ["point", "Pointing"], ["cheer", "Cheering"],
        ["danc", "Dancing"], ["die", "Fall"], ["fall", "Fall"], ["cry", "Crying"], ["surprise", "Surprised"]];
    var EMOTE_FPS = 60;
    var EMOTE_CHANNEL = "tivoli.emote";
    var playing = null;  // {emote, timer}
    var emotes = [];  // the avatar's standing emotes with stock clips
    var emoteValues = {};  // parameter -> the value last seen
    function clipURL(clip) {
        return Script.resolvePath("assets/animations/" + clip + ".fbx");
    }
    EMOTE_CLIPS.forEach(function (entry) {
        AnimationCache.prefetch(clipURL(entry[1]));
    });
    function clipFor(emote) {
        var name = emote.name.toLowerCase();
        for (var i = 0; i < EMOTE_CLIPS.length; i++) {
            if (name.indexOf(EMOTE_CLIPS[i][0]) !== -1) {
                return EMOTE_CLIPS[i][1];
            }
        }
        return null;  // none of ours yet (a backflip, a sad kick)
    }
    function announce(clip, seconds) {
        Messages.sendMessage(EMOTE_CHANNEL, JSON.stringify({ emote: clip, seconds: seconds }), false);
    }
    function stopEmote() {
        if (playing) {
            if (playing.timer) {
                Script.clearTimeout(playing.timer);
            }
            playing = null;
            MyAvatar.restoreAnimation();
            announce(null, 0);
        }
    }
    function emoteParameterSet(name, value) {
        if (value === 0) {  // a toggle switched off, or a button let go: loops and held poses end, one-shots finish
            if (playing && (playing.emote.loop || playing.emote.hold)) {
                stopEmote();
            }
            return;
        }
        var emote = emotes.filter(function (e) {
            return e.parameter === name && e.value === value;
        })[0];
        var clip = emote && clipFor(emote);
        var animation = clip && AnimationCache.getAnimation(clipURL(clip));
        var frames = animation && animation.frames ? animation.frames.length : 0;
        if (!frames) {
            return;
        }
        if (playing && playing.timer) {
            Script.clearTimeout(playing.timer);
        }
        MyAvatar.overrideAnimation(clipURL(clip), EMOTE_FPS, emote.loop, 0, frames - 1);
        var seconds = emote.loop || emote.hold ? null : frames / EMOTE_FPS;  // a hold keeps the last pose until let go
        announce(clip, seconds);
        playing = { emote: emote, timer: null };
        if (seconds !== null) {
            var mine = playing;
            playing.timer = Script.setTimeout(function () {
                if (playing === mine) {
                    playing = null;
                    MyAvatar.restoreAnimation();
                }
            }, seconds * 1000);
        }
    }

    // The emote parameters are watched as well as set from the Avatar panel, so whatever sets them plays the emote; the
    // panel's own presses are taken at once, since a quick tap can come and go between two looks.
    var emoteWatch = Script.setInterval(function () {
        Object.keys(emoteValues).forEach(function (name) {
            var value = MyAvatar.getAvatarParameter(name);
            if (value !== emoteValues[name]) {
                emoteValues[name] = value;
                emoteParameterSet(name, value);
            }
        });
    }, 50);

    function onAvatarMenuChanged() {
        stopEmote();
        var menuData = MyAvatar.getAvatarMenu();
        emotes = (menuData.emotes || []).filter(function (emote) {
            return !emote.seated && !emote.own;
        });
        emoteValues = {};
        emotes.forEach(function (emote) {
            emoteValues[emote.parameter] = MyAvatar.getAvatarParameter(emote.parameter);
        });
        if (hasMenu(menuData)) {
            var saved = savedValues();
            menuData.parameters.forEach(function (parameter) {
                if (parameter.saved && saved[parameter.name] !== undefined) {
                    MyAvatar.setAvatarParameter(parameter.name, saved[parameter.name]);
                }
            });
        }
    }

    // a choice: the parameter set, an emote it starts played, and the value remembered when the avatar saves it
    function choose(name, value) {
        if (typeof name !== "string" || typeof value !== "number") {
            return;
        }
        MyAvatar.setAvatarParameter(name, value);
        if (emoteValues[name] !== undefined) {
            emoteValues[name] = MyAvatar.getAvatarParameter(name);
            emoteParameterSet(name, emoteValues[name]);
        }
        var parameter = (MyAvatar.getAvatarMenu().parameters || []).filter(function (p) {
            return p.name === name;
        })[0];
        if (parameter && parameter.saved) {
            save(name, value);
        }
    }

    function onMessageReceived(channel, message, senderID, localOnly) {
        if (channel !== CHANNEL || !localOnly) {
            return;  // only this client's own panel and scripts choose for its avatar
        }
        var event;
        try {
            event = JSON.parse(message);
        } catch (e) {
            return;
        }
        if (event && event.type === "set") {
            choose(event.name, event.value);
        }
    }

    MyAvatar.avatarMenuChanged.connect(onAvatarMenuChanged);
    Messages.subscribe(CHANNEL);
    Messages.messageReceived.connect(onMessageReceived);
    Script.scriptEnding.connect(function () {
        MyAvatar.avatarMenuChanged.disconnect(onAvatarMenuChanged);
        Messages.messageReceived.disconnect(onMessageReceived);
        Messages.unsubscribe(CHANNEL);
        Script.clearInterval(emoteWatch);
        stopEmote();
    });
    onAvatarMenuChanged();
}());
