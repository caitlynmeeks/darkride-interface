//
//  VehicleSimulation.h
//  libraries/physics/src
//
//  The driven vehicles (PHYSICS.md 4.6, 4.8 and round 6). For every entity with a wheeled vehicle group that has a body in
//  the root world: Jolt's wheeled vehicle constraint built from the entity's spec (WheeledVehicleSpec), the driver's inputs
//  applied to it every substep, and the three roles of PHYSICS.md 4.6 in one class, the same code on the physics server
//  and on every client:
//
//  - the authority (the physics server when the domain has one, else the client that owns the vehicle) applies the
//    driver's inputs in sequence, one per substep, and publishes the vehicle's pose, velocities, the last input applied
//    and its sequence number in its ordinary ownership updates (EntityMotionState::sendUpdate asks fillState);
//  - the driver's client, when it is not the authority, applies its own inputs at once (client-side prediction), numbers
//    them, keeps a record of the pose after each, and sends the last few in every VehicleInput packet; when a state from
//    the authority names the last sequence it applied, the record for that sequence is compared with it, and if the error
//    is more than a few centimetres the world is rewound to that substep (the other dynamic bodies from a ring of
//    snapshots, the vehicle from the state) and the inputs since are replayed (reconciliation). The jump that leaves is
//    smoothed out of the shown pose over a tenth of a second;
//  - every other client runs the same vehicle from the last known inputs (extrapolation with the model) and, when a
//    state arrives, rewinds to its time and replays the same way, smoothing the difference over a quarter of a second.
//
//  Wheel entities (type "wheel") that are children of the chassis are placed from the local simulation every frame;
//  they cost no packets. A vehicle's transform is driven locally everywhere (EntityItem::setTransformDrivenLocally), so a
//  network update of it is a correction to adopt, never a placement; the one exception is an explicit edit by someone
//  who is not the authority (the editor moving the ship), which the authority adopts as a teleport (round 7).
//
//  An airship (type "airship", PHYSICS.md round 7) is the same three roles over another model: no constraint, but the
//  forces of AirshipModel (buoyancy, thrust, fins, drag, ballast) added to the hull's body every substep from the same
//  applied input, replayed the same way. Its hull is usually a frame as well, so the deck is a room the crew walks in.
//
//  Round 9 adds the physics server's state stream (PHYSICS.md 4.5's second step): the server sends each vehicle's state
//  straight to the clients that asked for it (VehicleState packets, every STREAM_STEPS substeps, unreliable; a client
//  asks with a VehicleStateRequest naming the vehicles it simulates), and its edits through the entity server drop to
//  RELAY_UPDATE_STEPS, for persistence, late joiners and scripts. On the stream the driver reconciles against every
//  state with the correction's blend scaled to the measured round trip; everyone else shows the vehicle a little behind
//  the newest state and interpolates between the two states that bracket that time (snapshot interpolation,
//  SnapshotBuffer), extrapolating with the model when none does. Without a stream (no physics server, or a server
//  before this round) the relay's states drive the round 6 paths as before.
//
//  Round 10 replaces the authority's queue of a remote driver's inputs with a time-synchronised input buffer
//  (InputBuffer): the driver's sequence numbers are its ticks, one per substep, and the server applies each at the
//  substep its tick names, so a stall or a burst on the way no longer moves an input to another substep (which the
//  reconciliation read as an error at speed). Each state reports the tick applied and the buffer's margin; the
//  driver's client reconciles its record of that tick and keeps a small lead with LeadController, by time dilation
//  (PhysicsEngine::requestTimeShift) or, after a stall, a jump of its tick counter.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_VehicleSimulation_h
#define hifi_VehicleSimulation_h

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <vector>

#include <QtCore/QUuid>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyID.h>

#include "CorrectionCounters.h"
#include <AirshipModel.h>
#include <AircraftModel.h>
#include <EntityItem.h>
#include <InputBuffer.h>
#include <LeadController.h>
#include <SnapshotBuffer.h>
#include <VehicleInput.h>
#include <VehicleState.h>
#include <WheeledVehicleSpec.h>

#include "PhysicsEngine.h"

namespace JPH {
    class VehicleConstraint;
    class VehicleCollisionTester;
    class WheeledVehicleController;
    class Shape;
}

class EntityItemProperties;
class EntityMotionState;
class PhysicsWorld;

class VehicleSimulation : public PhysicsEngine::SubstepListener {
public:
    static constexpr float REPLAY_POSITION_THRESHOLD = 0.02f;          // m: a prediction this far off is rewound and replayed
    static constexpr float REPLAY_ANGLE_THRESHOLD = 0.0349f;           // rad (2 degrees)
    static constexpr float SNAP_DISTANCE = 3.0f;                       // m: a correction larger than this is shown at once
    static constexpr float DRIVER_BLEND_TIME = 0.1f;                   // s: the driver's residual decays with this time constant
    static constexpr float OTHERS_BLEND_TIME = 0.25f;                  // s: everyone else's
    static constexpr int MAX_REPLAY_STEPS = 45;                        // half a second of substeps
    static constexpr int SNAPSHOT_RING = 180;                          // two seconds of substeps
    static constexpr int SENT_RING = 60;                               // the authority remembers this many published poses (6 s at 10 Hz)
    static constexpr int UPDATE_STEPS = 9;                             // a client authority publishes every 9 substeps (10 Hz) while awake
    static constexpr quint64 DRIVER_SILENCE = 2 * USECS_PER_SECOND;    // no input for this long: the authority eases off and brakes
    // the state stream (round 9)
    static constexpr int STREAM_STEPS = 3;                             // the physics server streams every 3 substeps (30 Hz)
    static constexpr int RELAY_UPDATE_STEPS = 45;                      // and relays through the entity server every 45 (2 Hz)
    static constexpr int STREAM_PARKED_STEPS = 90;                     // a vehicle whose body sleeps is streamed this much longer
    static constexpr quint64 STREAM_FRESH = USECS_PER_SECOND;          // a stream is live while a state arrived within this
    static constexpr quint64 SUBSCRIPTION_INTERVAL = USECS_PER_SECOND; // a client asks for its vehicles' states this often
    static constexpr quint64 SUBSCRIPTION_TIMEOUT = 5 * USECS_PER_SECOND; // the server streams this long after the last ask
    static constexpr float DRIVER_BLEND_MIN = 0.03f;                   // s: the driver's blend is the round trip, within these
    static constexpr float DRIVER_BLEND_MAX = 0.5f;
    static constexpr float RTT_SMOOTHING = 0.2f;

    VehicleSimulation();
    ~VehicleSimulation();

    void setEngine(PhysicsEngine* engine) { _engine = engine; }
    void setServerless(bool serverless) { _serverless = serverless; }

    // the chassis's motion state has a body: build its vehicle (idempotent; the body is checked every substep)
    void attach(EntityMotionState* state);
    void detach(EntityMotionState* state);
    bool has(EntityMotionState* state) const { return _vehicles.find(state) != _vehicles.end(); }
    size_t getNumVehicles() const { return _vehicles.size(); }
    void clear();

    // PhysicsEngine::SubstepListener
    void preSubstep(uint32_t step, float dt) override;
    void postSubstep(uint32_t step, float dt) override;

    // once per frame on the simulation's thread with the tree locked: the wheel entities follow the wheels
    void updateVisuals();
    // once per frame: the driver's inputs since the last call go to the physics server
    void sendInputs();
    // the server's side: a driver's inputs arrived (any thread)
    void receiveInputs(const QUuid& vehicleID, const QUuid& sender, const std::vector<VehicleInput>& inputs);

    // the state stream (round 9)
    // a client's side: states arrived from the physics server, stamped on its clock (any thread); the skew is the
    // server's clock less this one's, as the node list measures it
    void receiveStates(const std::vector<VehicleState>& states, qint64 clockSkew, quint64 arrivalTime);
    // the server's side: a client asks for these vehicles' states (any thread)
    void receiveStateRequest(const QUuid& node, const std::vector<QUuid>& vehicleIDs);
    // once per frame on the simulation's thread: the server sends the states it captured to its subscribers; a client
    // asks the physics server for the vehicles it simulates, once a second and when the set changes
    void sendStream();
    void sendStateRequests();
    // how often the authority's edits go through the entity server: a client authority every UPDATE_STEPS, the physics
    // server (which streams) every RELAY_UPDATE_STEPS
    static int relayUpdateSteps();

    // the authority's state for an update: the applied input, its sequence, the engine, the gear, the domain time
    void fillState(EntityMotionState* state, EntityItemProperties& properties);
    // this process is driving the vehicle from local controls (the driver's seat here, or it owns the vehicle)
    bool isLocalDriver(EntityMotionState* state) const;

    // the server-state corrections, counted (item 6 of the instruments plan): for the Stats line
    const CorrectionCounters& getCorrectionCounters() const { return _corrections; }

    struct Stats {
        int vehicles { 0 };
        int corrections { 0 };          // replays and snaps since the start
        int replaySteps { 0 };          // substeps replayed since the start
        float lastError { 0.0f };       // m: the error that caused the last correction
        float maxError { 0.0f };        // m: the largest ever
        int inputsSent { 0 };
        int inputsReceived { 0 };
        int inputsDropped { 0 };        // the server refused them: late (their substep had passed) or early (over a second ahead)
        int inputsStarved { 0 };        // the server had no input for a substep and held the last
        // the input buffer (round 10)
        int inputsLate { 0 };
        int inputsEarly { 0 };
        int bufferRealigned { 0 };      // the server set a driver's alignment again
        int bufferMargin { 0 };         // the server's: the last margin; the driver's: the last reported
        int leadShifts { 0 };           // the driver's: shifts of its lead asked for
        int leadJumps { 0 };            // of them, jumps of the tick counter
        float timeShiftMs { 0.0f };     // the driver's: the shift still to make by time dilation
        // the state stream (round 9)
        int streamStatesSent { 0 };     // the server's, to all subscribers
        int streamStatesReceived { 0 };
        int streamExtrapolations { 0 }; // substeps a follower had no two states to interpolate between (a late packet)
        int streamSubscribers { 0 };    // the server's, now
        float rttMs { 0.0f };           // the driver's smoothed round trip to the server, from its inputs' acknowledgement
        float delayMs { 0.0f };         // a follower's delay behind the newest state
        float streamIntervalMs { 0.0f };// the arrivals' smoothed interval and jitter
        float streamJitterMs { 0.0f };
    };
    const Stats& getStats() const { return _stats; }

    // what the local simulation's first vehicle is doing right now, for a test or an inspector (copied each substep)
    struct Telemetry {
        bool valid { false };
        float engineRPM { 0.0f };
        int gear { 0 };
        float clutch { 0.0f };          // the clutch's friction, 0 (open) to 1
        float forward { 0.0f };         // the inputs as applied
        float right { 0.0f };
        float brake { 0.0f };
        float handBrake { 0.0f };
        float speed { 0.0f };           // m/s along the vehicle's forward
        // an airship's (round 7)
        bool airship { false };
        float altitude { 0.0f };        // m, of the envelope's centre
        float airDensity { 0.0f };
        float buoyancy { 0.0f };        // N up
        float ballastWeight { 0.0f };   // N down
        float thrust { 0.0f };          // N along forward
        float verticalThrust { 0.0f };  // N along up
        float airspeed { 0.0f };        // m/s
        float sideslip { 0.0f };        // degrees
        float incidence { 0.0f };       // degrees
        // an aircraft's (round 8)
        bool aircraft { false };
        float tilt { 0.0f };            // 0 level to 1 vertical
        float angleOfAttack { 0.0f };   // degrees
        float wingLift { 0.0f };        // N
        float wingDrag { 0.0f };        // N
        float liftCoefficient { 0.0f };
        float climbRate { 0.0f };       // m/s
        float ballast { 0.0f };         // kg
        float fill { 0.0f };
        float pitch { 0.0f };           // the inputs as applied
        float lift { 0.0f };
        float trim { 0.0f };
        std::vector<float> wheelRPM;    // per wheel
        std::vector<float> wheelSlip;   // longitudinal slip per wheel
        std::vector<float> suspension;  // suspension length per wheel, m
        std::vector<bool> contact;      // per wheel
    };
    Telemetry getTelemetry() const { std::lock_guard<std::mutex> lock(_telemetryMutex); return _telemetry; }

private:
    struct Record {
        uint32_t sequence { 0 };
        uint32_t step { 0 };
        VehicleInput input;
        glm::vec3 position { 0.0f };
        glm::quat rotation;
        glm::vec3 velocity { 0.0f };
        glm::vec3 angularVelocity { 0.0f };
    };
    struct Visual {
        std::weak_ptr<EntityItem> entity;
        int wheel { -1 };
        glm::vec3 right { 1.0f, 0.0f, 0.0f };
        glm::vec3 up { 0.0f, 1.0f, 0.0f };
    };
    struct Vehicle {
        EntityMotionState* state { nullptr };
        EntityItemPointer entity;
        JPH::BodyID bodyID;
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::Ref<JPH::VehicleCollisionTester> tester;
        uint32_t definitionVersion { 0 };
        uint64_t definitionHash { 0 };
        WheeledVehicleSpec spec;
        // an airship (round 7): the kind, its spec, what changes in flight, and the last forces for telemetry
        bool airship { false };
        bool airshipBuilt { false };
        AirshipSpec airshipSpec;
        AirshipState airshipState;
        AirshipForces airshipForces;
        // an aircraft (round 8): the same, for the tilt-thrust model
        bool aircraft { false };
        bool aircraftBuilt { false };
        AircraftSpec aircraftSpec;
        AircraftState aircraftState;
        AircraftForces aircraftForces;
        // inputs
        VehicleInput applied;                 // the last input applied, with its sequence
        float steering { 0.0f };              // the local driver's steering as the wheels took it (shapeSteering)
        uint32_t nextSequence { 1 };          // the driver's next
        InputBuffer inputs;                   // the authority's: a remote driver's inputs by tick (round 10)
        QUuid queueDriver;                    // whose inputs the buffer holds
        uint32_t lastTick { 0 };              // the authority's: the driver's tick the last substep applied, for the stream
        int lastMargin { 0 };                 // and the buffer's margin then
        LeadController lead;                  // the driver's: its lead over the server, from the margins reported
        quint64 lastInputTime { 0 };          // when the authority last had a fresh input
        std::vector<VehicleInput> unsent;     // the driver's, since the last packet
        std::vector<VehicleInput> recent;     // the driver's last few, for redundancy
        // prediction
        std::deque<Record> history;
        bool pendingRecord { false };
        // the authority's state as received
        uint32_t networkPoseVersion { 0 };
        quint64 lastStateTime { 0 };
        // the stream (round 9): the states as they arrive, and what the roles keep between substeps
        SnapshotBuffer snapshots;
        quint64 lastStreamArrival { 0 };   // local time of the newest arrival; the stream is live for STREAM_FRESH after
        quint64 lastStreamTime { 0 };      // the newest state's time, local, that the driver reconciled against
        bool extrapolating { true };       // a follower: no two states bracketed the shown time last substep
        uint32_t lastActiveStep { 0 };     // the authority's: the last substep the body was awake, for the stream
        float rtt { 0.0f };                // s, the driver's smoothed round trip
        std::deque<std::pair<uint32_t, quint64>> inputSendTimes;   // when each sequence first went out, for the round trip
        // the authority's own: the poses it published lately, with their velocities, to tell its own updates echoed by
        // the entity server (which moves them on by their velocities meanwhile) from someone's explicit edit of the
        // pose (a teleport, round 7)
        struct SentPose {
            glm::vec3 position;
            glm::quat rotation;
            glm::vec3 velocity;
            glm::vec3 angularVelocity;
        };
        std::deque<SentPose> sent;
        bool teleported { false };      // the last teleport adopted, so the same edit delivered again is not taken twice
        glm::vec3 teleportPosition { 0.0f };
        glm::quat teleportRotation;
        // visuals
        std::vector<Visual> visuals;
        uint32_t visualsScanStep { 0 };
        int childCount { -1 };
        bool wasAuthority { false };
        bool wasLocalDriver { false };        // the wheel taken here last substep (round 12: a fresh start at each sit)
        uint32_t driveStartSequence { 0 };    // the driver's first input of this sit: the lead ignores states acked before it
        bool logged { false };
    };
    struct Snapshot {
        uint32_t step { 0 };
        quint64 time { 0 };    // wall clock of the substep
        std::string bodies;    // the world's non-static bodies after the step (Jolt's state recorder)
    };

    // the stream (round 9)
    struct Subscriber {
        std::set<QUuid> vehicles;
        quint64 time { 0 };   // the last request
    };
    bool streamLive(const Vehicle& vehicle, quint64 now) const { return vehicle.lastStreamArrival != 0 && now < vehicle.lastStreamArrival + STREAM_FRESH; }
    VehicleState stateFromRelay(const Vehicle& vehicle) const;         // the entity's relayed state, its time made local
    VehicleState stateFromBody(const Vehicle& vehicle, uint32_t step) const;   // the authority's own, for the stream
    void drainReceivedStates();                                        // a client's: the arrivals into the vehicles' buffers
    void captureStream(uint32_t step);                                 // the server's: every STREAM_STEPS, the states to send

    void followStream(Vehicle& vehicle, uint32_t step);                // a follower's snapshot interpolation
    void reconcileStream(Vehicle& vehicle, uint32_t step);             // the driver's reconciliation against the stream
    void noteRoundTrip(Vehicle& vehicle, uint32_t ack, quint64 now);
    float driverBlendTime(const Vehicle& vehicle) const;

    PhysicsWorld* rootWorld() const;
    bool rebuild(Vehicle& vehicle);
    bool rebuildWheeled(Vehicle& vehicle, JPH::Body* body, PhysicsWorld* world);
    bool rebuildAirship(Vehicle& vehicle, JPH::Body* body, PhysicsWorld* world);
    void wearCenterOfMass(Vehicle& vehicle, const glm::vec3& centerOfMass);
    bool isBuilt(const Vehicle& vehicle) const {
        return vehicle.airship ? vehicle.airshipBuilt : vehicle.aircraft ? vehicle.aircraftBuilt : (bool)vehicle.constraint;
    }
    bool rebuildAircraft(Vehicle& vehicle, JPH::Body* body, PhysicsWorld* world);
    void tearDown(Vehicle& vehicle);
    bool isAuthority(const Vehicle& vehicle) const;
    bool localControls(const Vehicle& vehicle, VehicleInput& input) const;
    void chooseInput(Vehicle& vehicle, uint32_t step);
    void shapeSteering(Vehicle& vehicle, VehicleInput& input);
    void easeOff(Vehicle& vehicle);
    // the model takes this substep's input: the wheeled controller's driver input, or an airship's forces on its body
    void applyModelInput(Vehicle& vehicle, const VehicleInput& input, float dt);
    void checkCorrection(Vehicle& vehicle, uint32_t step);
    bool recentlySent(const Vehicle& vehicle, const glm::vec3& position, const glm::quat& rotation) const;
    // the driver's reconciliation against a state (the relay's or the stream's) with the blend time for the residual
    void correctDriver(Vehicle& vehicle, const VehicleState& state, uint32_t step, float blendTime);
    // a follower's correction from a relayed state: rewind to its time and run the model on (round 6)
    void correctFollower(Vehicle& vehicle, const VehicleState& state, uint32_t step);
    void setBodyState(Vehicle& vehicle, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& velocity,
                      const glm::vec3& angularVelocity);
    void setInternalState(Vehicle& vehicle, const VehicleState& state);
    // rewind the other bodies to the snapshot after `step` and replay `steps` substeps with the given inputs
    void replay(Vehicle& vehicle, const VehicleState& state, uint32_t fromStep, uint32_t toStep,
                const std::function<VehicleInput(uint32_t)>& inputAt);
    void smooth(Vehicle& vehicle, const glm::vec3& shownPosition, const glm::quat& shownRotation, float blendTime);
    void takeSnapshot(uint32_t step);
    const Snapshot* snapshotAt(uint32_t step) const;
    const Snapshot* snapshotBefore(quint64 time) const;
    void scanVisuals(Vehicle& vehicle, uint32_t step);
    bool needsSnapshots() const;

    PhysicsEngine* _engine { nullptr };
    bool _serverless { false };
    std::map<EntityMotionState*, Vehicle> _vehicles;
    std::map<QUuid, EntityMotionState*> _byEntity;
    CorrectionCounters _corrections;
    std::deque<Snapshot> _snapshots;
    std::mutex _inputMutex;
    Stats _stats;
    // the stream (round 9)
    std::mutex _streamMutex;
    std::vector<std::pair<VehicleState, quint64>> _receivedStates;   // a client's: as received, with the arrival time
    std::map<QUuid, Subscriber> _subscribers;                         // the server's
    std::vector<VehicleState> _pendingStream;                         // the server's: captured, not yet sent
    quint64 _lastRequestTime { 0 };
    size_t _lastRequestCount { 0 };

    mutable std::mutex _telemetryMutex;
    Telemetry _telemetry;
    void recordTelemetry();
};

#endif // hifi_VehicleSimulation_h
