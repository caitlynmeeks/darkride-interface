"use strict";

//  controllerScripts.js
//
//  Created by David Rowe on 15 Mar 2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

/* global Script, Menu, Controller, Picks */

// Values that never change, read once for every controller script in this engine. On the WebAssembly engine each
// property read is a call into the app, and Controller.Standard is a map of ~190 entries that the dispatcher and its
// modules read ~16 times a tick, and squeezeHands.js 6 times a frame. (Controller.Hardware is left alone: it is
// rebuilt when devices come and go.)
(function () {
    function keep(object, names) {
        names.forEach(function (name) {
            if (!(name in object)) {
                return;
            }
            try {
                Object.defineProperty(object, name,
                    { value: object[name], writable: false, enumerable: true, configurable: true });
            } catch (e) {
                // not a replaceable property on this engine (QtScript): it is read each time, as before
            }
        });
    }
    keep(Controller, ["Standard"]);
    keep(Picks, ["PICK_ENTITIES", "PICK_OVERLAYS", "PICK_AVATARS", "PICK_HUD", "PICK_INCLUDE_NONCOLLIDABLE",
        "PICK_INCLUDE_INVISIBLE", "INTERSECTED_NONE", "INTERSECTED_ENTITY", "INTERSECTED_LOCAL_ENTITY",
        "INTERSECTED_OVERLAY", "INTERSECTED_AVATAR", "INTERSECTED_HUD"]);
}());

var CONTOLLER_SCRIPTS = [
    "squeezeHands.js",
    "controllerDisplayManager.js",
    "grab.js",
    //"toggleAdvancedMovementForHandControllers.js",
    "handTouch.js",
    "controllerDispatcher.js",
    "controllerModules/nearParentGrabOverlay.js",
    "controllerModules/stylusInput.js",
    "controllerModules/equipEntity.js",
    "controllerModules/nearTrigger.js",
    "controllerModules/webSurfaceLaserInput.js",
    "controllerModules/inEditMode.js",
    "controllerModules/inVREditMode.js",
    "controllerModules/disableOtherModule.js",
    "controllerModules/farTrigger.js",
    "controllerModules/teleport.js",
    "controllerModules/hudOverlayPointer.js",
    "controllerModules/mouseHMD.js",
    "controllerModules/scaleEntity.js",
    "controllerModules/nearGrabHyperLinkEntity.js",
    "controllerModules/nearTabletHighlight.js",
    "controllerModules/nearGrabEntity.js",
    "controllerModules/farGrabEntity.js",
    "controllerModules/pushToTalk.js",
    "controllerModules/trackedHandWalk.js",
    "controllerModules/trackedHandTablet.js"
];

var DEBUG_MENU_ITEM = "Debug defaultScripts.js";

function runDefaultsTogether() {
    for (var j in CONTOLLER_SCRIPTS) {
        if (CONTOLLER_SCRIPTS.hasOwnProperty(j)) {
            Script.include(CONTOLLER_SCRIPTS[j]);
        }
    }
}

function runDefaultsSeparately() {
    for (var i in CONTOLLER_SCRIPTS) {
        if (CONTOLLER_SCRIPTS.hasOwnProperty(i)) {
            Script.load(CONTOLLER_SCRIPTS[i]);
        }
    }
}

if (Menu.isOptionChecked(DEBUG_MENU_ITEM)) {
    runDefaultsSeparately();
} else {
    runDefaultsTogether();
}
