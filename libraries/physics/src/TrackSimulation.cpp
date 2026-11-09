//
//  TrackSimulation.cpp
//  libraries/physics/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: these functions, the train on its track as each client follows it: startFromCar, adoptState, advance,
//   drive, takePublications, isServerDriven, each with everything it calls on the vehicle's path.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "TrackSimulation.h"

#include <cmath>

#include <EntityTree.h>
#include <NodeList.h>
#include <PhysicsHelpers.h>
#include <TrackEntityItem.h>

#include "ObjectMotionState.h"
#include "PhysicsLogging.h"

namespace {
    // a train not driven for this many substeps is picked up afresh from where its car is
    const uint32_t RESYNC_AFTER_STEPS = 10;
    const quint64 SUBSTEP_USECS = (quint64)(PHYSICS_ENGINE_FIXED_SUBSTEP * (float)USECS_PER_SECOND + 0.5f);

    qint64 entityServerSkew() {
        auto nodeList = DependencyManager::get<NodeList>();
        if (!nodeList) {
            return 0;
        }
        SharedNodePointer entityServer = nodeList->soloNodeOfType(NodeType::EntityServer);
        return entityServer ? entityServer->getClockSkewUsec() : 0;
    }
}

quint64 TrackSimulation::domainTime(quint64 localTime) {
    return (quint64)((qint64)localTime + entityServerSkew());
}

quint64 TrackSimulation::localTime(quint64 domainTime) {
    return (quint64)((qint64)domainTime - entityServerSkew());
}

quint64 TrackSimulation::substepDomainTime() {
    quint64 substepTime = ObjectMotionState::getSubstepTime();
    return domainTime(substepTime ? substepTime : usecTimestampNow());
}

// FROZEN (vehicle path): see the note at the top of this file.
bool TrackSimulation::isServerDriven(const QUuid& trackID) const {
    auto it = _trains.find(trackID);
    if (it == _trains.end() || it->second.serverStateTime == 0) {
        return false;
    }
    return substepDomainTime() < it->second.serverStateTime + SERVER_STATE_TTL;
}

// FROZEN (vehicle path): see the note at the top of this file.
void TrackSimulation::takePublications(std::vector<Publication>& publications) {
    publications.swap(_publications);
    _publications.clear();
}

float TrackSimulation::wrap(const Train& train, float distance) const {
    if (!train.curve.isClosed()) {
        return distance;
    }
    float length = train.curve.length();
    if (length <= 0.0f) {
        return 0.0f;
    }
    distance = std::fmod(distance, length);
    if (distance > 0.5f * length) {
        distance -= length;
    } else if (distance < -0.5f * length) {
        distance += length;
    }
    return distance;
}

void TrackSimulation::rebuild(Train& train, const TrackEntityItem& track) {
    QVector<glm::vec3> points = track.getTrackPoints();
    QVector<float> bank = track.getTrackBank();
    QVector<float> dwell = track.getTrackDwell();
    train.curve.build(std::vector<glm::vec3>(points.begin(), points.end()),
                      std::vector<float>(bank.begin(), bank.end()), track.getTrackClosed());
    train.schedule.speed = track.getTrackSpeed();
    train.schedule.acceleration = track.getTrackAcceleration();
    train.schedule.dwell.assign(dwell.begin(), dwell.end());
    train.version = track.getTrackVersion();
    if (train.running && train.curve.isValid()) {
        // the track changed under a running train: keep its place, its way and its speed, find the stations again
        TrackFollower::State state = train.follower.state();
        train.follower.reset(train.curve, train.schedule, state.s, state.direction, state.speed);
        train.publishPending = true;
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void TrackSimulation::startFromCar(Train& train, const EntityItemPointer& car, const glm::vec3& trackPosition,
                                   const glm::quat& toTrack, uint32_t step) {
    // start driving from where the car is now: its nearest point on the curve, at its speed along it
    glm::vec3 carOnTrack = toTrack * (car->getWorldPosition() - trackPosition);
    float carArcLength = train.curve.nearest(carOnTrack);
    glm::vec3 velocityOnTrack = toTrack * car->getWorldVelocity();
    float along = glm::dot(velocityOnTrack, train.curve.poseAt(carArcLength).tangent);
    train.follower.reset(train.curve, train.schedule, carArcLength - car->getTrackOffset(), along < 0.0f ? -1 : 1,
                         std::fabs(along));
    train.running = true;
    train.lastStep = step;
    train.residual = 0.0f;
    train.residualRate = 0.0f;
    train.publishPending = true;
    if (Physics::isServerAuthority()) {
        const TrackFollower::State& state = train.follower.state();
        qCInfo(physics) << "physics-server: train on track" << car->getTrackID() << "started from car" << car->getName()
            << "at" << state.s << "m, speed" << state.speed << ", station" << state.station << ", dwell" << state.dwellRemaining;
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
// A state from the server (or, on the server, a state persisted by the entity server): the follower takes it and runs
// it on to this substep in the same substeps the server took, so both agree; what this client showed meanwhile becomes
// the residual, which closes slowly when it is small (the clocks' noise, CLOSE_RATE), decays over BLEND_TIME when it is
// a desync, or is dropped when it is too large to blend.
void TrackSimulation::adoptState(Train& train, const TrackEntityItem& track, const EntityItemPointer& car,
                                 const glm::vec3& trackPosition, const glm::quat& toTrack, quint64 stepTime, uint32_t step) {
    float shown;
    if (train.running) {
        // The follower is brought to this substep first (the frame's own one or two, which drive() would run next), so
        // what this client shows is read at the same time the server's state is caught up to: the residual is then
        // the two timelines' true offset, never the frame's own travel. Booked as a residual, that travel held the
        // car one substep at every adoption and closed at CLOSE_RATE after (entry 25).
        if (step > train.lastStep) {
            advance(train, glm::min(step - train.lastStep, (uint32_t)MAX_CATCHUP_STEPS), false);
        }
        shown = train.follower.state().s + train.residual;
    } else {
        glm::vec3 carOnTrack = toTrack * (car->getWorldPosition() - trackPosition);
        shown = train.curve.nearest(carOnTrack) - car->getTrackOffset();
    }

    TrackEntityItem::TrainState received = track.getTrainState();   // the six as one
    if (!std::isfinite(received.distance) || !std::isfinite(received.speed) || !std::isfinite(received.dwell)) {
        qCWarning(physics) << "train on track" << track.getID() << ": a state from" << received.time << "is not finite; ignored";
        return;   // the setters refuse these; this covers an older server's
    }
    TrackFollower::State state;
    state.s = received.distance;
    state.speed = received.speed;
    state.direction = received.reversed ? -1 : 1;
    state.dwellRemaining = received.dwell;
    state.station = received.station == TrackEntityItem::NO_STATION ? -1 : (int)received.station;
    train.follower.setState(state);
    train.adoptedStateVersion = received.version;

    quint64 stateTime = received.time;
    int steps = 0;
    float remainder = 0.0f;
    if (stepTime > stateTime) {
        quint64 behind = stepTime - stateTime;
        steps = (int)glm::min((quint64)MAX_CATCHUP_STEPS, behind / SUBSTEP_USECS);
        // The state's time sits anywhere between this client's substeps: the whole substeps first, then the part of
        // one left over, so the state lands at this substep's time itself, not up to half a substep either side of
        // it (rounded, that half read as a residual of one substep at some adoptions; entry 25, part 2). A state
        // older than the cap is run the cap's substeps and no further, as before.
        if (steps < MAX_CATCHUP_STEPS) {
            remainder = (float)(behind - (quint64)steps * SUBSTEP_USECS) / (float)USECS_PER_SECOND;
        }
    }
    for (int i = 0; i < steps; ++i) {
        train.follower.advance(train.curve, train.schedule, PHYSICS_ENGINE_FIXED_SUBSTEP);
    }
    if (remainder > 0.0f) {
        train.follower.advance(train.curve, train.schedule, remainder);
    }

    float residual = wrap(train, shown - train.follower.state().s);
    if (train.running && (std::fabs(residual) > 0.5f || steps > 180)) {
        qCWarning(physics) << "train on track" << track.getID() << ": a state from" << stateTime << "adopted at" << stepTime
            << "(" << steps << "substeps of catch-up, skew" << entityServerSkew() << "us): shown at" << shown << "m, state puts it at"
            << train.follower.state().s << "m, speed" << train.follower.state().speed << ", dwell" << train.follower.state().dwellRemaining
            << "; residual" << residual << (std::fabs(residual) > SNAP_DISTANCE ? "(snapped)" : "(blended)");
    }
    bool snapped = !std::isfinite(residual) || std::fabs(residual) > SNAP_DISTANCE;
    if (train.running) {
        // a running train's correction, counted and into the frame trace (item 6): the rate a small residual closes at
        // (CLOSE_RATE, or less for one under a substep's worth), a desync's over BLEND_TIME, a snap's nothing
        float size = std::isfinite(residual) ? std::fabs(residual) : 0.0f;
        float closing = snapped ? 0.0f : (size <= BLEND_DISTANCE ? glm::min(size / PHYSICS_ENGINE_FIXED_SUBSTEP, CLOSE_RATE) : size / BLEND_TIME);
        _corrections.note(size, closing, snapped);
    }
    if (!train.running || snapped) {
        residual = 0.0f;
    }
    train.residual = residual;
    train.residualRate = 0.0f;
    train.running = true;
    train.lastStep = step;
    train.serverStateTime = stateTime;
    if (Physics::isServerAuthority()) {
        qCInfo(physics) << "physics-server: train on track" << car->getTrackID() << "resumed from its persisted state, " << steps
            << "substeps old, now at" << train.follower.state().s << "m";
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void TrackSimulation::advance(Train& train, uint32_t steps, bool watchEvents) {
    TrackFollower::State before = train.follower.state();
    for (uint32_t i = 0; i < steps; ++i) {
        train.follower.advance(train.curve, train.schedule, PHYSICS_ENGINE_FIXED_SUBSTEP);
    }
    if (watchEvents) {
        const TrackFollower::State& after = train.follower.state();
        bool stoodBefore = before.dwellRemaining > 0.0f || before.speed == 0.0f;
        bool standsNow = after.dwellRemaining > 0.0f || after.speed == 0.0f;
        if (stoodBefore != standsNow || before.direction != after.direction || before.station != after.station) {
            train.publishPending = true;   // a departure, an arrival, a turn at the end of an open track
        }
    }
    // the residual closes: a small one (the clocks' noise) at no more than CLOSE_RATE, a desync's exponentially over
    // BLEND_TIME; what it closed this step is a velocity the car reports
    if (train.residual != 0.0f && steps > 0) {
        float dt = (float)steps * PHYSICS_ENGINE_FIXED_SUBSTEP;
        float remaining = train.residual * std::exp(-dt / BLEND_TIME);
        if (std::fabs(train.residual) <= BLEND_DISTANCE) {
            float closing = glm::min(std::fabs(train.residual), CLOSE_RATE * dt);
            remaining = train.residual - std::copysign(closing, train.residual);
        }
        if (std::fabs(remaining) < 1.0e-4f) {
            remaining = 0.0f;
        }
        train.residualRate = (remaining - train.residual) / dt;
        train.residual = remaining;
    } else {
        train.residualRate = 0.0f;
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
bool TrackSimulation::drive(const EntityItemPointer& car, uint32_t step, bool mayDrive, glm::vec3& position,
                            glm::quat& rotation, glm::vec3& linearVelocity, glm::vec3& angularVelocity) {
    QUuid trackID = car->getTrackID();
    EntityTreePointer tree = car->getTree();
    if (trackID.isNull() || !tree) {
        return false;
    }
    auto track = std::dynamic_pointer_cast<TrackEntityItem>(tree->findEntityByID(trackID));
    if (!track) {
        if (Physics::isServerAuthority() && _trains.count(trackID)) {
            qCInfo(physics) << "physics-server: track" << trackID << "of car" << car->getName() << "is not in the tree";
        }
        _trains.erase(trackID);
        return false;
    }
    Train& train = _trains[trackID];

    // the curve is in the track's own space
    glm::vec3 trackPosition = track->getWorldPosition();
    glm::quat trackRotation = track->getWorldOrientation();
    glm::quat toTrack = glm::inverse(trackRotation);

    if (train.version != track->getTrackVersion()) {
        rebuild(train, *track);
    }
    if (!train.curve.isValid()) {
        return false;
    }

    bool authority = Physics::isServerAuthority();
    quint64 stepTime = substepDomainTime();

    // a state on the track: a client adopts every new one; the server adopts a persisted one once, when it starts a
    // train and the state is recent, so a restart keeps the timetable
    TrackEntityItem::TrainState latest = track->getTrainState();
    if (latest.version != train.adoptedStateVersion) {
        train.adoptedStateVersion = latest.version;
        if (latest.time != 0) {
            if (!authority) {
                adoptState(train, *track, car, trackPosition, toTrack, stepTime, step);
            } else if (!train.running && stepTime > latest.time && stepTime - latest.time < MAX_RESUME_AGE) {
                adoptState(train, *track, car, trackPosition, toTrack, stepTime, step);
                train.serverStateTime = 0;
                train.publishPending = true;
            }
        }
    }

    bool serverDriven = !authority && train.serverStateTime != 0 && stepTime < train.serverStateTime + SERVER_STATE_TTL;
    if (!(authority || serverDriven || mayDrive)) {
        return false;
    }

    if (!train.running) {
        startFromCar(train, car, trackPosition, toTrack, step);
    } else if (step > train.lastStep) {
        uint32_t steps = step - train.lastStep;
        if (steps > RESYNC_AFTER_STEPS && !authority) {
            // the car was out of the simulation for a while: back to the server's state when there is one, else to
            // where the car is (the server itself never does this: a stall there is just time to catch up)
            qCDebug(physics) << "train on track" << trackID << ": car" << car->getName() << "was not driven for" << steps << "substeps; "
                << (serverDriven ? "back to the server's state" : "restarting from the car");
            if (serverDriven) {
                adoptState(train, *track, car, trackPosition, toTrack, stepTime, step);
            } else {
                startFromCar(train, car, trackPosition, toTrack, step);
            }
        } else {
            advance(train, glm::min(steps, (uint32_t)MAX_CATCHUP_STEPS), authority);
            train.lastStep = step;
        }
    }

    if (authority) {
        if (train.publishPending || stepTime >= train.lastPublishedTime + HEARTBEAT) {
            Publication publication;
            publication.trackID = trackID;
            publication.state = train.follower.state();
            publication.domainTime = stepTime;
            _publications.push_back(publication);
            train.publishPending = false;
            train.lastPublishedTime = stepTime;
        }
    }

    glm::vec3 localPosition, localLinear, localAngular;
    glm::quat localRotation;
    train.follower.carPose(train.curve, car->getTrackOffset() + train.residual, localPosition, localRotation, localLinear,
                           localAngular);
    if (train.residualRate != 0.0f) {
        localLinear += train.residualRate * train.curve.poseAt(train.curve.normalize(train.follower.state().s
            + car->getTrackOffset() + train.residual)).tangent;
    }
    // into the car's parent's coordinates: the world for a car without a parent; for a car under a parent (a track
    // on a ship's deck, 4.10) the track's transform relative to that parent, which is constant while both ride the
    // ship, so the ship's motion never enters the car's local pose
    bool hasParent = false;
    Transform parentTransform = car->getParentTransform(hasParent);
    hasParent = hasParent && !car->getParentID().isNull();
    if (hasParent) {
        glm::quat fromParent = glm::inverse(parentTransform.getRotation());
        glm::vec3 trackInParent = fromParent * (trackPosition - parentTransform.getTranslation());
        glm::quat trackRotationInParent = fromParent * trackRotation;
        position = trackInParent + trackRotationInParent * localPosition;
        rotation = glm::normalize(trackRotationInParent * localRotation);
        linearVelocity = trackRotationInParent * localLinear;
        angularVelocity = trackRotationInParent * localAngular;
        return true;
    }
    position = trackPosition + trackRotation * localPosition;
    rotation = glm::normalize(trackRotation * localRotation);
    linearVelocity = trackRotation * localLinear;
    angularVelocity = trackRotation * localAngular;
    return true;
}
