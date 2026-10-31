//
//  judder.js
//  tests-manual/physics-baseline
//
//  The kart at speed from its chase camera (Caity, 2026-10-26: "very juddery at high speeds ... maybe the camera and
//  the vehicle are out of sync"). The go-kart (kart-rig.js, concatenated before this file by run-judder.sh) on a floor of
//  its own with a 0.9 grip, the avatar in its seat as kart.js does, driven straight at 6, 12 and 17 m/s and round a
//  gentle curve at 12 (half a g), four seconds of each recorded once the speed has settled. The camera is one of:
//    JUDDER_CONFIG.camera "script" (the default): vehicles.js's chase camera, copied below and run as it runs, on this
//      script's update (a client script's own thread, SCRIPT_FPS 60), Camera.mode "independent";
//    "entity": the camera fixed behind and above the kart in its frame (Camera.mode "entity", parented to the chassis),
//      which the engine works out every frame from the kart as drawn: the same view without the script's timing;
//    "chase": the engine's chase camera (Camera.setChaseTarget, 1.1), set up as vehicles.js sets it up for a car: the
//      script's chase, smoothing and all, worked out every frame.
//  The engine's frame trace (Test.frameTrace; the cam.* and ride.* columns ViewState.cpp writes, FrameTrace) holds the
//  camera and the kart as each frame showed them, dumped at each phase's end into frame-trace-<phase>.csv in the results
//  location (JUDDER_CONFIG.results); the render and present traces (Test.traceRenderedEntity, tracePresents) say what
//  reached the screen. Prints JUDDER lines with the phases' times; the traces are in judder.json, read by judder-report.py.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    var config = (typeof JUDDER_CONFIG !== "undefined" && JUDDER_CONFIG) ? JUDDER_CONFIG : {};
    var CAMERA = config.camera || "script";
    var SERVERLESS = config.serverless !== false;
    var RESULTS = config.results || "";
    var FRAME_TRACE = typeof Test.frameTrace === "function";
    var SIT_ANIMATION_URL = "https://s3-us-west-1.amazonaws.com/hifi-content/clement/production/animations/sitting_idle.fbx";
    var LIFETIME = 300;
    var FLOOR_FRICTION = 0.9;
    var FLOOR_TOP = 0.01;      // m: just over world.json's Ground (whose top is at 0), and an empty domain's nothing
    var START_Z = 500;         // the kart starts here facing -z, so the straights have a kilometre of floor
    var RECORD_SECONDS = 4;
    var SETTLE_SECONDS = 1.5;  // at the speed this long before recording
    var phases = [];
    var kart = null;
    var floor = null;
    var t0 = Date.now();

    function log(s) { print("JUDDER " + s); }
    function after(seconds, f) { Script.setTimeout(f, seconds * 1000); }

    // ---- vehicles.js's chase camera for a wheeled vehicle (scripts/system/vehicles.js, update's wheeled branch), as is ----
    var CAMERA_BACK = 7.0, CAMERA_UP = 2.5, CAMERA_RATE = 6.0;
    var CAMERA_BACK_MIN = 3.2, CAMERA_UP_MIN = 1.5, CAMERA_RATE_MAX = 9.0;
    var CAMERA_EASE_OUT = 0.12;
    var cameraPosition = null;
    var boomLength = null;
    function keptOutOfWalls(pivot, position, dt) {
        var length = Vec3.distance(pivot, position);
        if (typeof Camera.collideBoom !== "function" || length < 0.001) {
            return position;
        }
        var allowed = Camera.collideBoom(pivot, position, [kart.chassis]);
        if (boomLength === null || allowed < boomLength) {
            boomLength = allowed;
        } else {
            boomLength += (allowed - boomLength) * (1 - Math.exp(-dt / CAMERA_EASE_OUT));
        }
        return Vec3.sum(pivot, Vec3.multiply(boomLength / length, Vec3.subtract(position, pivot)));
    }
    function chaseCamera(properties, dt) {
        var front = Quat.getFront(properties.rotation);
        front = Vec3.normalize({ x: front.x, y: 0, z: front.z });
        var length = properties.dimensions.z;
        var back = Math.max(CAMERA_BACK_MIN, Math.min(CAMERA_BACK, 2.0 * length));
        var up = Math.max(CAMERA_UP_MIN, Math.min(CAMERA_UP, 0.7 * length));
        var rate = Math.max(CAMERA_RATE, Math.min(CAMERA_RATE_MAX, CAMERA_RATE * 3.6 / length));
        var wanted = Vec3.sum(properties.position, Vec3.sum(Vec3.multiply(-back, front), { x: 0, y: up, z: 0 }));
        if (!cameraPosition) {
            cameraPosition = wanted;
        } else {
            cameraPosition = Vec3.mix(cameraPosition, wanted, 1 - Math.exp(-dt * rate));
        }
        var pivot = Vec3.sum(properties.position, { x: 0, y: 1, z: 0 });
        Camera.position = keptOutOfWalls(pivot, cameraPosition, dt);
        Camera.orientation = Quat.lookAtSimple(cameraPosition, pivot);
    }

    // the same view, fixed in the kart's frame: back and up as the chase camera settles, looking at the same pivot
    function entityCamera() {
        var length = Kart.CHASSIS.z;
        var back = Math.max(CAMERA_BACK_MIN, Math.min(CAMERA_BACK, 2.0 * length));
        var up = Math.max(CAMERA_UP_MIN, Math.min(CAMERA_UP, 0.7 * length));
        Camera.mode = "entity";
        Camera.parentID = kart.chassis;
        Camera.parentJointIndex = -1;
        Camera.localPosition = { x: 0, y: up, z: back };   // the kart's front is -z
        Camera.localRotation = Quat.fromPitchYawRollDegrees(-Math.atan2(up - 1, back) * 180 / Math.PI, 0, 0);
    }

    function sit() {
        var seat = JSON.parse(Kart.SPEC.seats)[0];
        MyAvatar.collisionsEnabled = false;
        MyAvatar.getAnimationRoles().filter(function (role) {
            return !(role.indexOf("right") === 0 || role.indexOf("left") === 0);
        }).forEach(function (role) { MyAvatar.overrideRoleAnimation(role, SIT_ANIMATION_URL, 30, true, 1, 350); });
        MyAvatar.parentID = kart.chassis;
        MyAvatar.parentJointIndex = 65535;
        MyAvatar.localPosition = { x: seat.position[0], y: seat.position[1], z: seat.position[2] };
        MyAvatar.localOrientation = Quat.IDENTITY;
        Entities.editEntity(kart.chassis, { vehicle: { driver: MyAvatar.sessionUUID } });
        if (CAMERA === "entity") {
            entityCamera();
        } else if (CAMERA === "chase") {
            var length = Kart.CHASSIS.z;
            Camera.mode = "independent";
            Camera.setChaseTarget(kart.chassis, {
                back: Math.max(CAMERA_BACK_MIN, Math.min(CAMERA_BACK, 2.0 * length)),
                up: Math.max(CAMERA_UP_MIN, Math.min(CAMERA_UP, 0.7 * length)),
                rate: Math.max(CAMERA_RATE, Math.min(CAMERA_RATE_MAX, CAMERA_RATE * 3.6 / length)),
                pivotHeight: 1, lookAhead: 0, lookHeight: 1, followPitch: true
            });
        } else {
            Camera.mode = "independent";
        }
    }

    // the phases: a speed held by a proportional throttle (full throttle for the top), the steering for half a g of
    // sideways grip on the curve (atan(0.5 g L / v^2) of the 50 degree lock), settled, then recorded
    var PLAN = [
        { name: "straight 6", speed: 6, curve: false },
        { name: "straight 12", speed: 12, curve: false },
        { name: "curve 12", speed: 12, curve: true },
        { name: "straight top", speed: 0, curve: false }
    ];
    var phaseIndex = 0, phaseStart = 0, settledAt = 0, recording = false;
    var lastTick = 0;
    function steeringFor(speed) {
        var wheelbase = 1.0;
        var angle = Math.atan(0.5 * 9.8 * wheelbase / Math.max(1, speed * speed));
        return angle / (Kart.SPEC.maxSteerAngle * Math.PI / 180);
    }
    function tick(dt) {
        var now = Date.now();
        var p = Entities.getEntityProperties(kart.chassis, ["position", "rotation", "velocity", "dimensions"]);
        if (!p || !p.position) {
            return;
        }
        if (CAMERA === "script") {
            chaseCamera(p, dt);
        }
        var plan = PLAN[phaseIndex];
        var speed = Math.sqrt(p.velocity.x * p.velocity.x + p.velocity.z * p.velocity.z);
        var throttle = plan.speed ? Math.max(0, Math.min(1, 0.3 + (plan.speed - speed) * 0.5)) : 1;
        var steering = plan.curve ? steeringFor(plan.speed) : 0;
        Entities.setVehicleControls(kart.chassis, { throttle: throttle, steering: steering, brake: 0, handBrake: 0 });
        var t = (now - phaseStart) / 1000;
        var settled = plan.speed ? Math.abs(speed - plan.speed) < 0.4 : t > 6;
        if (!recording) {
            if (!settled) {
                settledAt = 0;
            } else if (!settledAt) {
                settledAt = now;
            } else if ((now - settledAt) / 1000 > SETTLE_SECONDS) {
                recording = true;
                phases.push({ name: plan.name, start: now, speedAtStart: speed });
                Test.traceRenderedEntity(kart.chassis);
                Test.tracePresents(true);
                if (FRAME_TRACE) {
                    Test.frameTrace(true);   // starts afresh: the ring holds about 14 s at 144, this phase's 4
                }
            }
            if (t > 20) {
                log("FAIL " + plan.name + ": never settled (" + speed.toFixed(2) + " m/s after 20 s)");
                phaseIndex = PLAN.length;
            }
        } else {
            var phase = phases[phases.length - 1];
            phase.speedSum = (phase.speedSum || 0) + speed;
            phase.ticks = (phase.ticks || 0) + 1;
            phase.tickGaps = phase.tickGaps || [];
            if (lastTick) {
                phase.tickGaps.push(now - lastTick);
            }
            if ((now - phase.start) / 1000 >= RECORD_SECONDS) {
                phase.end = now;
                phase.speed = phase.speedSum / phase.ticks;
                phase.renderTrace = Test.takeRenderTrace();
                phase.presentTrace = Test.takePresentTrace();
                Test.tracePresents(false);
                Test.traceRenderedEntity(Uuid.NULL);
                if (FRAME_TRACE) {
                    phase.frameTraceFile = "frame-trace-" + plan.name.replace(/[^a-z0-9]+/gi, "-") + ".csv";
                    phase.frameRows = Test.dumpFrameTrace(RESULTS + "/" + phase.frameTraceFile);
                    Test.frameTrace(false);
                }
                log(plan.name + ": " + phase.speed.toFixed(2) + " m/s, " + phase.ticks + " script updates in " +
                    ((phase.end - phase.start) / 1000).toFixed(2) + " s, " + phase.renderTrace.length + " drawn frames, " +
                    phase.presentTrace.length + " presents, " + (phase.frameRows === undefined ? "no" : phase.frameRows) +
                    " frame trace rows; window " + phase.start + " to " + phase.end + " ms");
                recording = false;
                settledAt = 0;
                phaseIndex++;
                phaseStart = now;
            }
        }
        lastTick = now;
        if (phaseIndex >= PLAN.length) {
            Script.update.disconnect(tick);
            finish();
        }
    }

    function finish() {
        Entities.setVehicleControls(kart.chassis, { throttle: 0, steering: 0, brake: 1, handBrake: 1 });
        if (CAMERA === "chase") {
            Camera.clearChaseTarget();
        }
        var frustum = Camera.frustum;
        Test.saveObject({ camera: CAMERA, serverless: SERVERLESS, displayRefreshRate: Test.getDisplayRefreshRate(),
                          fieldOfView: frustum.fieldOfView, aspectRatio: frustum.aspectRatio,
                          gameLoopRate: Test.getGameLoopRate(), renderLoopRate: Test.getRenderLoopRate(),
                          vsync: Test.isVsyncEnabled(), presentPeriod: Test.getPresentPeriod(),
                          stats: Entities.getVehicleStats ? Entities.getVehicleStats() : {}, phases: phases }, "judder.json");
        log("display " + Test.getDisplayRefreshRate().toFixed(0) + " Hz, game loop " + Test.getGameLoopRate().toFixed(0) +
            " Hz, render " + Test.getRenderLoopRate().toFixed(0) + " Hz, vsync " + Test.isVsyncEnabled() + ", camera " + CAMERA);
        log("DONE");
        after(1, function () {
            Entities.deleteEntity(kart.chassis);
            Entities.deleteEntity(floor);
            after(1, function () { Test.quit(); });
        });
    }

    function start(tries) {
        if (!Window.isPhysicsEnabled()) {
            if (tries < 120) {
                after(1, function () { start(tries + 1); });
            } else {
                log("FAIL no physics after 120 s");
                Test.quit();
            }
            return;
        }
        floor = Entities.addEntity({
            type: "Box", shape: "Cube", name: "judder-floor", position: { x: 0, y: FLOOR_TOP - 0.05, z: 0 },
            dimensions: { x: 1200, y: 0.1, z: 1200 }, color: { red: 70, green: 90, blue: 110 }, dynamic: false,
            collisionless: false, friction: FLOOR_FRICTION, lifetime: LIFETIME, grab: { grabbable: false }
        });
        kart = Kart.create(0, FLOOR_TOP, START_Z, 0, LIFETIME);
        MyAvatar.position = { x: 2, y: 1, z: START_Z };
        after(4, function () {
            sit();
            log("seated, camera " + CAMERA + (SERVERLESS ? ", serverless" : ", on a domain"));
            after(1.5, function () {
                phaseStart = Date.now();
                Script.update.connect(tick);
            });
        });
    }

    after(2, function () { start(0); });
}());
