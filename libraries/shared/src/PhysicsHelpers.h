//
//  PhysicsHelpers.h
//  libraries/shared/src
//
//  Created by Andrew Meadows 2015.01.27
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the fixed substep, NUM_SUBSTEPS_PER_SECOND and PHYSICS_ENGINE_FIXED_SUBSTEP, the same on every client
//   and the physics server.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#ifndef hifi_PhysicsHelpers_h
#define hifi_PhysicsHelpers_h

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <QUuid>

// TODO: move everything in here to the physics library after the physics/entities library
// dependency order is swapped.

const int32_t PHYSICS_ENGINE_MAX_NUM_SUBSTEPS = 6; // Bullet will start to "lose time" at 10 FPS.
// FROZEN (vehicle path): see the note at the top of this file.
const uint32_t NUM_SUBSTEPS_PER_SECOND = 90;
const float PHYSICS_ENGINE_FIXED_SUBSTEP = 1.0f / (float)NUM_SUBSTEPS_PER_SECOND;

const float DYNAMIC_LINEAR_SPEED_THRESHOLD = 0.05f;  // 5 cm/sec
const float DYNAMIC_ANGULAR_SPEED_THRESHOLD = 0.087266f;  // ~5 deg/sec
const float KINEMATIC_LINEAR_SPEED_THRESHOLD = 0.001f;  // 1 mm/sec
const float KINEMATIC_ANGULAR_SPEED_THRESHOLD = 0.008f;  // ~0.5 deg/sec

// return incremental rotation (Bullet-style) caused by angularVelocity over timeStep
glm::quat computeBulletRotationStep(const glm::vec3& angularVelocity, float timeStep);

namespace Physics {
    int32_t getDefaultCollisionMask(int32_t group);

    void setSessionUUID(const QUuid& sessionID);
    const QUuid& getSessionUUID();

    // This process is the domain's physics server (PHYSICS.md 4.5): it drives the trains, owns their cars at
    // SERVER_SIMULATION_PRIORITY and publishes their state; a client never sets this.
    void setServerAuthority(bool authority);
    bool isServerAuthority();
};

#endif // hifi_PhysicsHelpers_h
