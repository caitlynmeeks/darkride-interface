//
//  TrackSimulation.h
//  libraries/physics/src
//
//  The trains on the tracks (PHYSICS.md 4.8, rounds 4 and 5). For each Track entity: its curve, rebuilt when the entity's
//  properties change, and the follower that moves the train along it. A car (an entity with trackID) asks here for its
//  pose every substep, and the first car of a train to ask in a step advances the train.
//
//  Who drives (round 5): the physics server, when the domain has one. It runs the follower, owns the cars at SERVER
//  priority, and publishes the train's state on the Track entity (trainDistance, trainSpeed, trainReversed, trainDwell,
//  trainStation, at domain time trainTime) when something happens (a departure, an arrival, a turn, a start) and once a
//  second besides. Every client runs the same follower on from the last state it received, in the same substeps, so the
//  train is a function of the domain's clock and a lost packet changes nothing; when a state arrives, the difference
//  between where the client showed the train and where the state puts it is a residual that decays over a quarter of a
//  second (error smoothing), never a snap. Without a server (none in the domain, or it has gone quiet), the client that
//  owns a car drives it and the others move it by the velocities the owner sends, as in round 4. Domain time is the
//  entity server's clock, which every node already knows through its pings.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_TrackSimulation_h
#define hifi_TrackSimulation_h

#include <map>
#include <vector>

#include <QtCore/QUuid>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <EntityItem.h>
#include <TrackCurve.h>
#include <TrackFollower.h>
#include "CorrectionCounters.h"

class TrackEntityItem;

class TrackSimulation {
public:
    // the server-state corrections, counted (item 6 of the instruments plan): for the Stats line
    const CorrectionCounters& getCorrectionCounters() const { return _corrections; }

    // a train state the server wants published on its Track entity
    struct Publication {
        QUuid trackID;
        TrackFollower::State state;
        quint64 domainTime { 0 };   // the domain time the state is for (the substep's)
    };

    // A correction's residual (adoptState) closes at no more than CLOSE_RATE, so the car never visibly lurches: the
    // server's states carry the clocks' noise (the skew estimate, the substep rounding: tens of ms, a few cm at
    // speed), and chasing that noise over BLEND_TIME put a small forward-or-back push into the car once a second
    // (Port Amarelle's monorail, 2026-10-31). A residual of a real desync (past BLEND_DISTANCE) decays over BLEND_TIME
    // as before, and one past SNAP_DISTANCE is taken at once.
    static constexpr float CLOSE_RATE = 0.1f;            // m/s: the most a small residual moves the car against its track speed
    static constexpr float BLEND_DISTANCE = 0.5f;        // m: a residual past this is a desync and blends over BLEND_TIME
    static constexpr float BLEND_TIME = 0.25f;           // s: a desync's residual decays with this time constant
    static constexpr float SNAP_DISTANCE = 3.0f;         // m: a correction larger than this is taken at once
    static constexpr quint64 SERVER_STATE_TTL = 15 * USECS_PER_SECOND;   // a server state older than this is stale
    static constexpr quint64 HEARTBEAT = USECS_PER_SECOND;               // the server republishes at least this often
    static constexpr quint64 MAX_RESUME_AGE = 60 * USECS_PER_SECOND;     // the server resumes a persisted state this old
    static constexpr int MAX_CATCHUP_STEPS = 90 * 20;                    // 20 s of substeps when adopting a state

    // the pose and velocities a car should have at this substep, in its parent's coordinates (the world for an
    // unparented car; a car on a track that rides a frame is the track's child, round 8, and gets the pose relative
    // to its parent); the train is advanced to the step on the first call for it. `mayDrive` is the round 4
    // rule: this client owns the car, or the world is serverless. The car is driven when this process is the physics
    // server, when the track carries a fresh server state, or when mayDrive; false otherwise, and when the car's track
    // is missing or has no curve, and the caller moves the car by its stored velocities.
    bool drive(const EntityItemPointer& car, uint32_t step, bool mayDrive, glm::vec3& position, glm::quat& rotation,
               glm::vec3& linearVelocity, glm::vec3& angularVelocity);
    // the track's train runs from the server's state here, so no client should bid for its cars
    bool isServerDriven(const QUuid& trackID) const;
    // the server's side: the states that want publishing since the last call
    void takePublications(std::vector<Publication>& publications);
    void clear() { _trains.clear(); }
    size_t getNumTrains() const { return _trains.size(); }

    // domain time is the entity server's clock; local time converts through this node's measured skew to it
    static quint64 domainTime(quint64 localTime);
    static quint64 localTime(quint64 domainTime);
    // the domain time of the substep being simulated
    static quint64 substepDomainTime();

private:
    CorrectionCounters _corrections;
    struct Train {
        uint32_t version { 0 };   // the Track entity's property version the curve was built from
        TrackCurve curve;
        TrackFollower::Schedule schedule;
        TrackFollower follower;
        uint32_t lastStep { 0 };
        bool running { false };
        // the server's state, on a client
        uint32_t adoptedStateVersion { 0 };   // the entity's train-state version last looked at
        quint64 serverStateTime { 0 };        // domain time of the last server state adopted; 0 for none
        float residual { 0.0f };              // m along the track: where the train is shown minus where it is
        float residualRate { 0.0f };          // m/s: how fast the residual is closing, for the car's velocity
        // publishing, on the server
        bool publishPending { false };
        quint64 lastPublishedTime { 0 };      // domain time of the last publication
    };

    void rebuild(Train& train, const TrackEntityItem& track);
    void startFromCar(Train& train, const EntityItemPointer& car, const glm::vec3& trackPosition, const glm::quat& toTrack,
                      uint32_t step);
    void adoptState(Train& train, const TrackEntityItem& track, const EntityItemPointer& car, const glm::vec3& trackPosition,
                    const glm::quat& toTrack, quint64 stepTime, uint32_t step);
    void advance(Train& train, uint32_t steps, bool watchEvents);
    float wrap(const Train& train, float distance) const;

    std::map<QUuid, Train> _trains;
    std::vector<Publication> _publications;
};

#endif // hifi_TrackSimulation_h
