//
//  TrackEntityItem.h
//  libraries/entities/src
//
//  The Track entity (PHYSICS.md 4.8): a curve through control points in the entity's own space, with a bank angle and
//  a dwell time per point, open or closed, and the schedule a train keeps on it. The curve itself (a centripetal
//  Catmull-Rom by arc length) is libraries/vehicles' TrackCurve, built from these properties by the physics on each
//  client and by the physics server; the editor's gizmo draws and edits the points. An entity with trackID set
//  rides this track at trackOffset metres along the train; the physics moves it. The track has no body of its own.
//  The train's state (round 5) lives here too: the physics server writes it as it drives, and every client runs the
//  same follower on from the last state it received, so the train is a function of the domain's clock everywhere.
//  The guideway (the monorail's beam and pylons, TrackGuideway in libraries/vehicles) is what the track draws and
//  collides as: its settings are properties here, the mesh is the client renderer's and the physics server's.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the train's state (trainDistance, trainSpeed, trainReversed, trainDwell, trainStation, trainTime) and
//   TrainState.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#ifndef hifi_TrackEntityItem_h
#define hifi_TrackEntityItem_h

#include "EntityItem.h"

class TrackEntityItem : public EntityItem {
    using Pointer = std::shared_ptr<TrackEntityItem>;
public:
    static EntityItemPointer factory(const EntityItemID& entityID, const EntityItemProperties& properties);

    TrackEntityItem(const EntityItemID& entityItemID);

    ALLOW_INSTANTIATION // This class can be instantiated

    // methods for getting/setting all properties of an entity
    EntityItemProperties getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const override;
    bool setSubClassProperties(const EntityItemProperties& properties) override;

    EntityPropertyFlags getEntityProperties(EncodeBitstreamParams& params) const override;

    void appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                            EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                            EntityPropertyFlags& requestedProperties,
                            EntityPropertyFlags& propertyFlags,
                            EntityPropertyFlags& propertiesDidntFit,
                            int& propertyCount,
                            OctreeElement::AppendState& appendState) const override;

    int readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                         ReadBitstreamToTreeParams& args,
                                         EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                         bool& somethingChanged) override;

    bool shouldBePhysical() const override { return false; }

    static const int MAX_POINTS;
    static const float DEFAULT_SPEED;          // m/s
    static const float DEFAULT_ACCELERATION;   // m/s^2
    static const float MAX_SPEED;
    static const float MAX_ACCELERATION;
    static const quint16 NO_STATION;   // trainStation when the train stands at or heads for no station
    static const QString DEFAULT_PROFILE;      // "beam"
    static const float DEFAULT_WIDTH;          // m, the beam's
    static const float DEFAULT_HEIGHT;         // m
    static const float DEFAULT_CLEARANCE;      // m, ride line to the beam's top
    static const float DEFAULT_PYLON_SPACING;  // m along the track
    static const float DEFAULT_PYLON_FLOOR;    // m, the pylons' feet in the track's own space
    static const float MAX_GUIDEWAY_SIZE;      // m, any of the above
    static const float MAX_POINT_COORDINATE;   // m, a control point's coordinates either way (the domain's half extent)

    void setTrackPoints(const QVector<glm::vec3>& points);
    QVector<glm::vec3> getTrackPoints() const;

    void setTrackBank(const QVector<float>& bank);   // degrees per point
    QVector<float> getTrackBank() const;

    void setTrackDwell(const QVector<float>& dwell);   // seconds per point; more than zero makes a station
    QVector<float> getTrackDwell() const;

    void setTrackClosed(bool closed);
    bool getTrackClosed() const;

    void setTrackSpeed(float speed);
    float getTrackSpeed() const;

    void setTrackAcceleration(float acceleration);
    float getTrackAcceleration() const;

    // counts every change to the properties above, so a curve built from them knows when to rebuild
    uint32_t getTrackVersion() const;

    // the guideway: what the track draws and collides as along its curve
    void setTrackProfile(const QString& profile);   // "beam" or "none"
    QString getTrackProfile() const;
    void setTrackWidth(float value);
    float getTrackWidth() const;
    void setTrackHeight(float value);
    float getTrackHeight() const;
    void setTrackClearance(float value);
    float getTrackClearance() const;
    void setTrackPylonSpacing(float value);
    float getTrackPylonSpacing() const;
    void setTrackPylonFloor(float value);
    float getTrackPylonFloor() const;
    void setColor(const glm::u8vec3& color);
    glm::u8vec3 getColor() const;
    // counts every change to the guideway's settings (not the curve's: those count in the track version)
    uint32_t getGuidewayVersion() const;

    // FROZEN (vehicle path): see the note at the top of this file.
    // the train's state, as the physics server last published it (TrackFollower::State plus the domain time it is
    // for; trainTime 0 means no server has driven this track)
    void setTrainDistance(float value);
    float getTrainDistance() const;
    void setTrainSpeed(float value);
    float getTrainSpeed() const;
    void setTrainReversed(bool value);
    bool getTrainReversed() const;
    void setTrainDwell(float value);
    float getTrainDwell() const;
    void setTrainStation(quint16 value);
    quint16 getTrainStation() const;
    void setTrainTime(quint64 value);
    quint64 getTrainTime() const;
    // The six as one, read under one lock, with the version that counts the states received: the version moves only
    // when trainTime lands, the last of the six on the wire, so a reader never takes half of one state and half of the
    // next.
    struct TrainState {
        float distance { 0.0f };
        float speed { 0.0f };
        bool reversed { false };
        float dwell { 0.0f };
        quint16 station { 0xFFFF };
        quint64 time { 0 };
        uint32_t version { 0 };
    };
    TrainState getTrainState() const;
    uint32_t getTrainStateVersion() const;

protected:
    QVector<glm::vec3> _trackPoints;
    QVector<float> _trackBank;
    QVector<float> _trackDwell;
    bool _trackClosed { false };
    float _trackSpeed { DEFAULT_SPEED };
    float _trackAcceleration { DEFAULT_ACCELERATION };
    uint32_t _trackVersion { 1 };

    QString _trackProfile { DEFAULT_PROFILE };
    float _trackWidth { DEFAULT_WIDTH };
    float _trackHeight { DEFAULT_HEIGHT };
    float _trackClearance { DEFAULT_CLEARANCE };
    float _trackPylonSpacing { DEFAULT_PYLON_SPACING };
    float _trackPylonFloor { DEFAULT_PYLON_FLOOR };
    glm::u8vec3 _color { 255, 255, 255 };
    uint32_t _guidewayVersion { 1 };

    float _trainDistance { 0.0f };
    float _trainSpeed { 0.0f };
    bool _trainReversed { false };
    float _trainDwell { 0.0f };
    quint16 _trainStation { NO_STATION };
    quint64 _trainTime { 0 };
    uint32_t _trainStateVersion { 0 };
};

#endif // hifi_TrackEntityItem_h
