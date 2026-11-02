"use strict";
//
//  vehicles.js
//  scripts/system
//
//  Sitting in and driving vehicles (PHYSICS.md round 6). Click a vehicle (an entity with vehicle.type "wheeled", or one of
//  its wheels) within reach to take a seat: the driver's seat if nobody holds it, else the nearest other seat. Seated, the
//  avatar is parented to the vehicle at the seat; the driver's W/S or up/down (throttle, brake), A/D or left/right
//  (steering) and Space (hand brake) go to the vehicle through Entities.setVehicleControls, and a chase camera follows.
//  F stands up.
//
//  An airship (vehicle.type "airship", PHYSICS.md round 7) is a moving reference frame whose deck the crew walks on, so
//  nobody is parented to it by hand: click the ship (or anything that is part of it) within reach to take the helm (the
//  seat marked driver) if nobody holds it, which puts you at the helm standing. At the helm: W/S set the throttle (it
//  stays where you leave it; X cuts it), A/D or left/right steer with the rudder, up/down raise and lower the nose with
//  the elevator, Space and C run the vertical thruster up and down, B drops ballast, V vents gas, F leaves the helm.
//  The camera is your own.
//
//  An aircraft (vehicle.type "aircraft", PHYSICS.md round 8) is taken the same way, and flown through the flight
//  assistant (libraries/flightAssist.js) the way San Andreas' planes fly: W/S faster and slower (the speed stays where
//  you leave it; X stops, into a hover), A/D bank in flight and turn in a hover, Up/Down (or Space/C) climb and
//  descend; let go and it levels and holds its height. A tilt-rotor hovers when slow and flies on its wing when fast,
//  by itself. A chase camera follows, and a line at the top left shows the speed and height. F leaves the stick.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the driver's camera and seat: the CAMERA_* and AIR_CAMERA_* numbers, takeHelm, leaveHelm, sit and
//   stand.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

(function() {
    Script.include("/~/system/libraries/flightAssist.js");

    // our own copy of High Fidelity's sitting animation, once on its storage (Caity 10-29; physics's OK)
    var SIT_ANIMATION_URL = Script.resourcesPath() + "avatar/animations/vehicle_sitting_idle.fbx";
    var SIT_ANIMATION_FPS = 30;
    var SIT_ANIMATION_FIRST_FRAME = 1;
    var SIT_ANIMATION_LAST_FRAME = 350;
    var REACH = 4.0;             // m from the avatar to the vehicle's surface, to get in
    // FROZEN (vehicle path): see the note at the top of this file.
    var CAMERA_BACK = 7.0;       // m behind the vehicle
    var CAMERA_UP = 2.5;         // m above it
    var CAMERA_RATE = 6.0;       // per second: how fast the camera settles behind the vehicle
    var CAMERA_BACK_MIN = 3.2;   // m: a small vehicle's camera (a go-kart's) comes this close, and no closer
    var CAMERA_UP_MIN = 1.5;     // m
    var CAMERA_RATE_MAX = 9.0;   // per second: and settles this quickly
    var REVERSE_SPEED = 0.5;     // m/s: pressing against the direction of travel brakes above this speed
    var STAND_KEY = "f";
    var DRIVE_KEYS = [DriveKeys.TRANSLATE_X, DriveKeys.TRANSLATE_Y, DriveKeys.TRANSLATE_Z, DriveKeys.YAW, DriveKeys.STEP_YAW,
                      DriveKeys.STEP_TRANSLATE_X, DriveKeys.STEP_TRANSLATE_Y, DriveKeys.STEP_TRANSLATE_Z];

    // FROZEN (vehicle path): see the note at the top of this file.
    var AIR_CAMERA_BACK = 14.0;  // m behind an aircraft
    var AIR_CAMERA_UP = 4.0;     // m above it
    var AIR_CAMERA_RATE = 4.0;   // per second
    var GROUND_RAY_EVERY = 3;    // frames between the aircraft's ground rays
    var THROTTLE_RATE = 0.5;     // per second: how fast W/S move an airship's throttle setting

    var seated = null;           // { vehicleID, seat, driver, airship }
    var previousCameraMode = null;
    var throttleSetting = 0;     // an airship's

    function parseList(text) {
        if (!text) {
            return [];
        }
        try {
            var value = JSON.parse(text);
            return Array.isArray(value) ? value : [];
        } catch (e) {
            return [];
        }
    }

    function vec3Of(value) {
        if (Array.isArray(value) && value.length >= 3) {
            return { x: value[0], y: value[1], z: value[2] };
        }
        if (value && typeof value.x === "number") {
            return { x: value.x, y: value.y, z: value.z };
        }
        return { x: 0, y: 0, z: 0 };
    }

    function rolesToOverride() {
        return MyAvatar.getAnimationRoles().filter(function(role) {
            return !(role.indexOf("right") === 0 || role.indexOf("left") === 0);
        });
    }

    // the vehicle behind a clicked entity: itself, the chassis of a clicked wheel, or the airship a clicked part belongs to
    function vehicleOf(entityID) {
        var properties = Entities.getEntityProperties(entityID, ["vehicle", "parentID", "position", "rotation", "dimensions"]);
        if (!properties) {
            return null;
        }
        var hops = 0;
        while ((!properties.vehicle || properties.vehicle.type === "none" || properties.vehicle.type === "wheel") &&
                properties.parentID && properties.parentID !== Uuid.NULL && hops++ < 4) {
            entityID = properties.parentID;
            properties = Entities.getEntityProperties(entityID, ["vehicle", "parentID", "position", "rotation", "dimensions"]);
            if (!properties) {
                return null;
            }
        }
        if (!properties.vehicle || (properties.vehicle.type !== "wheeled" && properties.vehicle.type !== "airship" && properties.vehicle.type !== "aircraft")) {
            return null;
        }
        properties.id = entityID;
        return properties;
    }

    // FROZEN (vehicle path): see the note at the top of this file.
    // the helm of an airship, or the stick of an aircraft (round 8, the same keys: Space and C tilt its nacelles up toward
    // hover and down toward forward flight while held): the crew stands on the deck in the ship's own world (the physics
    // makes the avatar a member of the frame by where it stands), so taking the helm is walking to it and taking the
    // keys, no parenting
    function takeHelm(vehicle, seat) {
        seated = { vehicleID: vehicle.id, seat: seat, driver: true, airship: true };
        throttleSetting = 0;
        for (var i = 0; i < DRIVE_KEYS.length; i++) {
            MyAvatar.disableDriveKey(DRIVE_KEYS[i]);
        }
        var helm = Vec3.sum(vehicle.position, Vec3.multiplyQbyV(vehicle.rotation, vec3Of(seat.position)));
        MyAvatar.position = Vec3.sum(helm, { x: 0, y: 1.0, z: 0 });
        MyAvatar.orientation = Quat.multiply(vehicle.rotation, Quat.fromPitchYawRollDegrees(0, seat.yaw || 0, 0));
        Entities.editEntity(vehicle.id, { vehicle: { driver: MyAvatar.sessionUUID } });
        Script.update.connect(update);
        if (vehicle.vehicle.type === "aircraft" && typeof FlightAssist !== "undefined") {
            seated.aircraft = { assist: FlightAssist.create(vehicle.vehicle.mass), previous: null, frame: 0, aboveGround: 1000, readout: null };
            previousCameraMode = Camera.mode;
            Camera.mode = "independent";
            // the chase camera, worked out by the engine every frame: level behind the heading (not the bank, so a turn
            // does not roll the view), settling at a rate, kept out of walls, looking 6 m ahead of the nose
            Camera.setChaseTarget(vehicle.id, { back: AIR_CAMERA_BACK, up: AIR_CAMERA_UP, rate: AIR_CAMERA_RATE,
                                                pivotHeight: 1, lookAhead: 6, lookHeight: 0 });
            seated.aircraft.readout = Overlays.addOverlay("text", {
                x: 20, y: 80, width: 430, height: 58, font: { size: 16 }, text: "",
                color: { red: 244, green: 246, blue: 248 }, backgroundColor: { red: 28, green: 33, blue: 54 }, backgroundAlpha: 0.7,
                leftMargin: 10, topMargin: 6, visible: true
            });
            print("vehicles.js: at the stick of " + vehicle.id + ": W/S faster and slower, X stop, A/D bank (turn in a hover), Up/Down climb and descend, F leave");
        } else {
            if (vehicle.vehicle.type === "aircraft") {
                print("vehicles.js: the flight assistant is missing; the aircraft's own controls instead");
            }
            print("vehicles.js: at the helm of " + vehicle.id + ": W/S throttle, X cut, A/D rudder, up/down elevator, Space/C up and down, B ballast, V vent, F leave");
        }
    }

    // FROZEN (vehicle path): see the note at the top of this file.
    function leaveHelm() {
        var vehicleID = seated.vehicleID;
        if (seated.aircraft) {
            Overlays.deleteOverlay(seated.aircraft.readout);
            Camera.clearChaseTarget();
            if (previousCameraMode !== null) {
                Camera.mode = previousCameraMode;
                previousCameraMode = null;
            }
        }
        seated = null;
        try {
            Script.update.disconnect(update);
        } catch (e) {
            // never connected (the helm was not fully taken): leaving must still give the keys back
        }
        Entities.clearVehicleControls(vehicleID);
        var properties = Entities.getEntityProperties(vehicleID, ["vehicle"]);
        if (properties && properties.vehicle && properties.vehicle.driver === MyAvatar.sessionUUID) {
            Entities.editEntity(vehicleID, { vehicle: { driver: Uuid.NULL } });
        }
        for (var i = 0; i < DRIVE_KEYS.length; i++) {
            MyAvatar.enableDriveKey(DRIVE_KEYS[i]);
        }
        print("vehicles.js: left the helm of " + vehicleID);
    }

    // FROZEN (vehicle path): see the note at the top of this file.
    function sit(vehicle, seat) {
        var isDriver = !!seat.driver;
        seated = { vehicleID: vehicle.id, seat: seat, driver: isDriver, world: worldNow() };
        MyAvatar.collisionsEnabled = false;
        MyAvatar.hmdLeanRecenterEnabled = false;
        var roles = rolesToOverride();
        for (var i = 0; i < roles.length; i++) {
            MyAvatar.overrideRoleAnimation(roles[i], SIT_ANIMATION_URL, SIT_ANIMATION_FPS, true, SIT_ANIMATION_FIRST_FRAME, SIT_ANIMATION_LAST_FRAME);
        }
        for (i = 0; i < DRIVE_KEYS.length; i++) {
            MyAvatar.disableDriveKey(DRIVE_KEYS[i]);
        }
        // the avatar rides the vehicle: parented at the seat, facing the seat's way (0 is the vehicle's forward)
        MyAvatar.parentID = vehicle.id;
        MyAvatar.parentJointIndex = 65535;
        MyAvatar.localPosition = vec3Of(seat.position);
        MyAvatar.localOrientation = Quat.fromPitchYawRollDegrees(0, seat.yaw || 0, 0);
        if (isDriver) {
            Entities.editEntity(vehicle.id, { vehicle: { driver: MyAvatar.sessionUUID } });
        }
        previousCameraMode = Camera.mode;
        Camera.mode = "independent";
        // the chase camera, worked out by the engine every frame from the vehicle as drawn (a script's camera moves only
        // 60 times a second, and at speed the view judders against the car): behind along its forward and above,
        // settling at a rate so a bump does not swing it, kept out of walls, looking just over it; by the vehicle's
        // length, the rover's (3.6 m) at CAMERA_BACK and CAMERA_UP, a go-kart's (1.45 m) low, near and quick
        var length = vehicle.dimensions.z;
        Camera.setChaseTarget(vehicle.id, {
            back: Math.max(CAMERA_BACK_MIN, Math.min(CAMERA_BACK, 2.0 * length)),
            up: Math.max(CAMERA_UP_MIN, Math.min(CAMERA_UP, 0.7 * length)),
            rate: Math.max(CAMERA_RATE, Math.min(CAMERA_RATE_MAX, CAMERA_RATE * 3.6 / length)),
            pivotHeight: 1, lookAhead: 0, lookHeight: 1, followPitch: true
        });
        Script.update.connect(update);
        print("vehicles.js: sitting in " + (seat.name || "a seat") + " of " + vehicle.id + (isDriver ? " as the driver" : " (the driver is " + vehicle.vehicle.driver + ")"));
    }

    // FROZEN (vehicle path): see the note at the top of this file.
    // worldChanged: the vehicle stayed behind in the world just left (the engine has already dropped the avatar's
    // parent and stood it where the seat carried it: vehicle master plan entry 12), so the avatar's own state is
    // restored and nothing else: no step beside a vehicle that is not there, no edit of its driver
    function stand(worldChanged) {
        if (!seated) {
            return;
        }
        if (seated.airship) {
            leaveHelm();
            return;
        }
        var vehicleID = seated.vehicleID;
        var wasDriver = seated.driver;
        seated = null;
        Script.update.disconnect(update);
        if (worldChanged) {
            MyAvatar.parentID = Uuid.NULL;   // dropped by the engine already; nothing if so
        } else {
            Entities.clearVehicleControls(vehicleID);
            if (wasDriver) {
                var properties = Entities.getEntityProperties(vehicleID, ["vehicle"]);
                if (properties && properties.vehicle && properties.vehicle.driver === MyAvatar.sessionUUID) {
                    Entities.editEntity(vehicleID, { vehicle: { driver: Uuid.NULL } });
                }
            }
            // step out beside the vehicle, upright whatever way up the vehicle lies: the level right of where the seat
            // faces, half a metre up. goToLocation, not setting position and orientation: those race the physics step,
            // which hands back the pose the avatar had in the seat (a tilted one in a car on its side); goToLocation is
            // applied between steps and finds a free spot if the one asked for is inside something
            var position = MyAvatar.position;
            var level = Quat.cancelOutRollAndPitch(MyAvatar.orientation);
            var right = Quat.getRight(level);
            MyAvatar.parentID = Uuid.NULL;
            MyAvatar.goToLocation(Vec3.sum(position, Vec3.sum(Vec3.multiply(1.5, right), { x: 0, y: 0.5, z: 0 })), true, level, false);
        }
        for (var i = 0; i < DRIVE_KEYS.length; i++) {
            MyAvatar.enableDriveKey(DRIVE_KEYS[i]);
        }
        var roles = rolesToOverride();
        for (i = 0; i < roles.length; i++) {
            MyAvatar.restoreRoleAnimation(roles[i]);
        }
        MyAvatar.collisionsEnabled = true;
        MyAvatar.hmdLeanRecenterEnabled = true;
        Camera.clearChaseTarget();
        if (previousCameraMode !== null) {
            Camera.mode = previousCameraMode;
            previousCameraMode = null;
        }
        print("vehicles.js: standing up from " + vehicleID + (worldChanged ? " (left behind in another world)" : ""));
    }

    // the world the avatar sat down in, as a world change is told apart from a vehicle deleted under the avatar: the
    // address changes the moment a new world is asked for, before its entities arrive
    function worldNow() {
        return location.hostname + "|" + location.domainID;
    }

    // metres from the aircraft's belly to whatever is below it (not itself or its parts)
    function groundBelow(properties) {
        var ignore = [seated.vehicleID].concat(Entities.getChildrenIDs(seated.vehicleID));
        var hit = Entities.findRayIntersection({ origin: properties.position, direction: { x: 0, y: -1, z: 0 } }, true, [], ignore);
        var halfHeight = properties.dimensions ? 0.5 * properties.dimensions.y : 0;
        return hit && hit.intersects ? Math.max(0, hit.distance - halfHeight) : 1000;
    }

    function flyAircraft(properties, dt) {
        var a = seated.aircraft;
        var k = Controller.Hardware.Keyboard;
        var held = function (key) { return Controller.getValue(key) > 0.5 ? 1 : 0; };
        var keys = {
            faster: held(k.W), slower: held(k.S), stop: held(k.X),
            left: held(k.A) || held(k.Left), right: held(k.D) || held(k.Right),
            up: held(k.Up) || held(k.Space), down: held(k.Down) || held(k.C)
        };
        if (a.frame++ % GROUND_RAY_EVERY === 0) {
            a.aboveGround = groundBelow(properties);
        }
        var live = (Entities.getVehicleStats() || {}).live;
        if (live && !live.aircraft) {
            live = null;
        }
        var s = FlightAssist.state(properties, live, a.aboveGround, a.previous, dt);
        a.previous = s;
        Entities.setVehicleControls(seated.vehicleID, a.assist.controls(keys, s, dt));
        Overlays.editOverlay(a.readout, {
            text: (a.assist.mode === "hover" ? "HOVER" : "FLIGHT") + "   speed " + s.airspeed.toFixed(0) + " m/s  (wanted " +
                a.assist.speedTarget.toFixed(0) + ")   height " + Math.max(0, s.aboveGround).toFixed(0) + " m\n" +
                "W/S speed  A/D " + (a.assist.mode === "hover" ? "turn" : "bank") + "  Up/Down climb  X stop  F leave"
        });
    }

    var HELM_REACH = 20;  // m from the vehicle's centre: farther, the helm is left

    function update(dt) {
        if (!seated) {
            return;
        }
        var properties = Entities.getEntityProperties(seated.vehicleID, ["id", "position", "rotation", "velocity", "vehicle", "dimensions"]);
        if (!properties || properties.id !== seated.vehicleID) {
            // the vehicle is gone (a deleted entity's properties come back as defaults, a null id among them), or the
            // avatar left for another world and the vehicle stayed behind
            stand(seated.world !== worldNow());
            return;
        }
        if (seated.airship && Vec3.distance(MyAvatar.position, properties.position) > HELM_REACH) {
            stand();   // at a helm without a seat, so whoever is taken away from it (a teleport) has left it
            return;
        }
        var front = Quat.getFront(properties.rotation);
        if (seated.aircraft) {
            flyAircraft(properties, dt);
            return;
        }
        if (seated.airship) {
            var k = Controller.Hardware.Keyboard;
            throttleSetting += dt * THROTTLE_RATE * (Controller.getValue(k.W) - Controller.getValue(k.S));
            throttleSetting = Math.max(-1, Math.min(1, throttleSetting));
            if (Controller.getValue(k.X) > 0.5) {
                throttleSetting = 0;
            }
            var rudder = (Controller.getValue(k.D) || Controller.getValue(k.Right)) - (Controller.getValue(k.A) || Controller.getValue(k.Left));
            var elevator = Controller.getValue(k.Up) - Controller.getValue(k.Down);
            var lift = (Controller.getValue(k.Space) > 0.5 ? 1 : 0) - (Controller.getValue(k.C) > 0.5 ? 1 : 0);
            var trim = (Controller.getValue(k.B) > 0.5 ? 1 : 0) - (Controller.getValue(k.V) > 0.5 ? 1 : 0);
            Entities.setVehicleControls(seated.vehicleID, { throttle: throttleSetting, steering: rudder, pitch: elevator, lift: lift, trim: trim });
            return;
        }
        if (seated.driver) {
            // the keys themselves (the avatar's drive actions are off while seated, and the keyboard's own inputs are
            // always there): W/S or the up and down arrows, A/D or the left and right arrows, Space
            var keys = Controller.Hardware.Keyboard;
            var forward = (Controller.getValue(keys.W) || Controller.getValue(keys.Up)) - (Controller.getValue(keys.S) || Controller.getValue(keys.Down));
            var steering = (Controller.getValue(keys.D) || Controller.getValue(keys.Right)) - (Controller.getValue(keys.A) || Controller.getValue(keys.Left));
            var handBrake = Controller.getValue(keys.Space) > 0.5 ? 1 : 0;
            var brake = 0;
            // pressing against the way the car is going is the brake until it has stopped; then it is reverse
            var speedAlong = Vec3.dot(properties.velocity, front);
            if (forward !== 0 && forward * speedAlong < 0 && Math.abs(speedAlong) > REVERSE_SPEED) {
                brake = Math.abs(forward);
                forward = 0;
            }
            Entities.setVehicleControls(seated.vehicleID, { throttle: forward, steering: steering, brake: brake, handBrake: handBrake });
        }
    }

    function onMousePress(entityID, event) {
        if (seated || !event.isPrimaryButton || event.isAlt || event.isControl) {
            return;
        }
        var vehicle = vehicleOf(entityID);
        if (!vehicle) {
            return;
        }
        var reach = REACH + 0.5 * Math.max(vehicle.dimensions.x, vehicle.dimensions.z);
        if (Vec3.distance(MyAvatar.position, vehicle.position) > reach) {
            return;
        }
        var seats = parseList(vehicle.vehicle.seats);
        if (seats.length === 0) {
            return;
        }
        // the driver's seat is free when nobody holds it, or when whoever holds it has left the domain (a client that
        // quit in the seat leaves its session in vehicle.driver)
        var driver = vehicle.vehicle.driver;
        var driverFree = !driver || driver === Uuid.NULL || driver === MyAvatar.sessionUUID ||
            AvatarList.getAvatarIdentifiers().indexOf(driver) < 0;
        var chosen = null;
        var best = Infinity;
        for (var i = 0; i < seats.length; i++) {
            var seat = seats[i];
            if (seat.driver && driverFree) {
                chosen = seat;
                break;
            }
            if (seat.driver) {
                continue;
            }
            var world = Vec3.sum(vehicle.position, Vec3.multiplyQbyV(vehicle.rotation, vec3Of(seat.position)));
            var distance = Vec3.distance(MyAvatar.position, world);
            if (distance < best) {
                best = distance;
                chosen = seat;
            }
        }
        if (chosen) {
            if (vehicle.vehicle.type === "airship" || vehicle.vehicle.type === "aircraft") {
                if (chosen.driver) {
                    takeHelm(vehicle, chosen);
                }
                return;   // the crew just walks aboard
            }
            sit(vehicle, chosen);
        }
    }

    function onKeyPress(event) {
        if (seated && event.text && event.text.toLowerCase() === STAND_KEY && !event.isControl && !event.isAlt) {
            stand();
        }
    }

    Entities.mousePressOnEntity.connect(onMousePress);
    Controller.keyPressEvent.connect(onKeyPress);
    Script.scriptEnding.connect(function() {
        Entities.mousePressOnEntity.disconnect(onMousePress);
        Controller.keyPressEvent.disconnect(onKeyPress);
        stand();
    });
}());
