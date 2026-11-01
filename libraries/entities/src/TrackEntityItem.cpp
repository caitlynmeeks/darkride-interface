//
//  TrackEntityItem.cpp
//  libraries/entities/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the train's state (trainDistance, trainSpeed, trainReversed, trainDwell, trainStation, trainTime): the
//   properties, their order on the wire (trainTime last) and their setters.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "TrackEntityItem.h"

#include <cmath>

#include "EntityItemProperties.h"

const int TrackEntityItem::MAX_POINTS = 256;
const float TrackEntityItem::DEFAULT_SPEED = 5.0f;
const float TrackEntityItem::DEFAULT_ACCELERATION = 1.0f;
const float TrackEntityItem::MAX_SPEED = 200.0f;
const float TrackEntityItem::MAX_ACCELERATION = 100.0f;
const quint16 TrackEntityItem::NO_STATION = 0xFFFF;
const QString TrackEntityItem::DEFAULT_PROFILE = "beam";
const float TrackEntityItem::DEFAULT_WIDTH = 0.8f;
const float TrackEntityItem::DEFAULT_HEIGHT = 1.0f;
const float TrackEntityItem::DEFAULT_CLEARANCE = 0.3f;
const float TrackEntityItem::DEFAULT_PYLON_SPACING = 10.0f;
const float TrackEntityItem::DEFAULT_PYLON_FLOOR = -8.0f;
const float TrackEntityItem::MAX_GUIDEWAY_SIZE = 1000.0f;
const float TrackEntityItem::MAX_POINT_COORDINATE = 16384.0f;

EntityItemPointer TrackEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    Pointer entity(new TrackEntityItem(entityID), [](EntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

TrackEntityItem::TrackEntityItem(const EntityItemID& entityItemID) : EntityItem(entityItemID) {
    _type = EntityTypes::Track;
}

EntityItemProperties TrackEntityItem::getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const {
    EntityItemProperties properties = EntityItem::getProperties(desiredProperties, allowEmptyDesiredProperties); // get the properties from our base class

    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackPoints, getTrackPoints);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackBank, getTrackBank);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackDwell, getTrackDwell);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackClosed, getTrackClosed);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackSpeed, getTrackSpeed);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackAcceleration, getTrackAcceleration);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trainDistance, getTrainDistance);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trainSpeed, getTrainSpeed);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trainReversed, getTrainReversed);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trainDwell, getTrainDwell);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trainStation, getTrainStation);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trainTime, getTrainTime);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackProfile, getTrackProfile);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackWidth, getTrackWidth);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackHeight, getTrackHeight);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackClearance, getTrackClearance);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackPylonSpacing, getTrackPylonSpacing);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(trackPylonFloor, getTrackPylonFloor);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(color, getColor);

    return properties;
}

bool TrackEntityItem::setSubClassProperties(const EntityItemProperties& properties) {
    bool somethingChanged = false;

    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackPoints, setTrackPoints);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackBank, setTrackBank);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackDwell, setTrackDwell);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackClosed, setTrackClosed);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackSpeed, setTrackSpeed);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackAcceleration, setTrackAcceleration);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trainDistance, setTrainDistance);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trainSpeed, setTrainSpeed);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trainReversed, setTrainReversed);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trainDwell, setTrainDwell);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trainStation, setTrainStation);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trainTime, setTrainTime);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackProfile, setTrackProfile);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackWidth, setTrackWidth);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackHeight, setTrackHeight);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackClearance, setTrackClearance);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackPylonSpacing, setTrackPylonSpacing);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(trackPylonFloor, setTrackPylonFloor);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(color, setColor);

    return somethingChanged;
}

int TrackEntityItem::readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                      ReadBitstreamToTreeParams& args,
                                                      EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                      bool& somethingChanged) {
    int bytesRead = 0;
    const unsigned char* dataAt = data;

    READ_ENTITY_PROPERTY(PROP_TRACK_POINTS, QVector<glm::vec3>, setTrackPoints);
    READ_ENTITY_PROPERTY(PROP_TRACK_BANK, QVector<float>, setTrackBank);
    READ_ENTITY_PROPERTY(PROP_TRACK_DWELL, QVector<float>, setTrackDwell);
    READ_ENTITY_PROPERTY(PROP_TRACK_CLOSED, bool, setTrackClosed);
    READ_ENTITY_PROPERTY(PROP_TRACK_SPEED, float, setTrackSpeed);
    READ_ENTITY_PROPERTY(PROP_TRACK_ACCELERATION, float, setTrackAcceleration);
    READ_ENTITY_PROPERTY(PROP_TRAIN_DISTANCE, float, setTrainDistance);
    READ_ENTITY_PROPERTY(PROP_TRAIN_SPEED, float, setTrainSpeed);
    READ_ENTITY_PROPERTY(PROP_TRAIN_REVERSED, bool, setTrainReversed);
    READ_ENTITY_PROPERTY(PROP_TRAIN_DWELL, float, setTrainDwell);
    READ_ENTITY_PROPERTY(PROP_TRAIN_STATION, quint16, setTrainStation);
    READ_ENTITY_PROPERTY(PROP_TRAIN_TIME, quint64, setTrainTime);
    READ_ENTITY_PROPERTY(PROP_TRACK_PROFILE, QString, setTrackProfile);
    READ_ENTITY_PROPERTY(PROP_TRACK_WIDTH, float, setTrackWidth);
    READ_ENTITY_PROPERTY(PROP_TRACK_HEIGHT, float, setTrackHeight);
    READ_ENTITY_PROPERTY(PROP_TRACK_CLEARANCE, float, setTrackClearance);
    READ_ENTITY_PROPERTY(PROP_TRACK_PYLON_SPACING, float, setTrackPylonSpacing);
    READ_ENTITY_PROPERTY(PROP_TRACK_PYLON_FLOOR, float, setTrackPylonFloor);
    READ_ENTITY_PROPERTY(PROP_COLOR, u8vec3Color, setColor);

    return bytesRead;
}

EntityPropertyFlags TrackEntityItem::getEntityProperties(EncodeBitstreamParams& params) const {
    EntityPropertyFlags requestedProperties = EntityItem::getEntityProperties(params);

    requestedProperties += PROP_TRACK_POINTS;
    requestedProperties += PROP_TRACK_BANK;
    requestedProperties += PROP_TRACK_DWELL;
    requestedProperties += PROP_TRACK_CLOSED;
    requestedProperties += PROP_TRACK_SPEED;
    requestedProperties += PROP_TRACK_ACCELERATION;
    requestedProperties += PROP_TRAIN_DISTANCE;
    requestedProperties += PROP_TRAIN_SPEED;
    requestedProperties += PROP_TRAIN_REVERSED;
    requestedProperties += PROP_TRAIN_DWELL;
    requestedProperties += PROP_TRAIN_STATION;
    requestedProperties += PROP_TRAIN_TIME;
    requestedProperties += PROP_TRACK_PROFILE;
    requestedProperties += PROP_TRACK_WIDTH;
    requestedProperties += PROP_TRACK_HEIGHT;
    requestedProperties += PROP_TRACK_CLEARANCE;
    requestedProperties += PROP_TRACK_PYLON_SPACING;
    requestedProperties += PROP_TRACK_PYLON_FLOOR;
    requestedProperties += PROP_COLOR;

    return requestedProperties;
}

void TrackEntityItem::appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                         EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                         EntityPropertyFlags& requestedProperties,
                                         EntityPropertyFlags& propertyFlags,
                                         EntityPropertyFlags& propertiesDidntFit,
                                         int& propertyCount,
                                         OctreeElement::AppendState& appendState) const {
    bool successPropertyFits = true;

    APPEND_ENTITY_PROPERTY(PROP_TRACK_POINTS, getTrackPoints());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_BANK, getTrackBank());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_DWELL, getTrackDwell());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_CLOSED, getTrackClosed());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_SPEED, getTrackSpeed());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_ACCELERATION, getTrackAcceleration());
    APPEND_ENTITY_PROPERTY(PROP_TRAIN_DISTANCE, getTrainDistance());
    APPEND_ENTITY_PROPERTY(PROP_TRAIN_SPEED, getTrainSpeed());
    APPEND_ENTITY_PROPERTY(PROP_TRAIN_REVERSED, getTrainReversed());
    APPEND_ENTITY_PROPERTY(PROP_TRAIN_DWELL, getTrainDwell());
    APPEND_ENTITY_PROPERTY(PROP_TRAIN_STATION, getTrainStation());
    APPEND_ENTITY_PROPERTY(PROP_TRAIN_TIME, getTrainTime());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_PROFILE, getTrackProfile());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_WIDTH, getTrackWidth());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_HEIGHT, getTrackHeight());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_CLEARANCE, getTrackClearance());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_PYLON_SPACING, getTrackPylonSpacing());
    APPEND_ENTITY_PROPERTY(PROP_TRACK_PYLON_FLOOR, getTrackPylonFloor());
    APPEND_ENTITY_PROPERTY(PROP_COLOR, getColor());
}

namespace {
    // A list from a script or an edit packet with a NaN or an infinity in it is refused whole (the entity keeps what it
    // had): such a value would reach the curve, the meshes and Jolt, and a NaN never equals the stored value, so it
    // would count as a change at every edit (10-21 review).
    template <typename T>
    bool allFinite(const QVector<T>& values) {
        for (const T& value : values) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }
    bool allFinite(const QVector<glm::vec3>& points) {
        for (const glm::vec3& point : points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
                return false;
            }
        }
        return true;
    }
}

void TrackEntityItem::setTrackPoints(const QVector<glm::vec3>& points) {
    if (!allFinite(points)) {
        return;
    }
    withWriteLock([&] {
        QVector<glm::vec3> clipped = points.size() > MAX_POINTS ? points.mid(0, MAX_POINTS) : points;
        // within the domain: a point typed a hundred kilometres out would make a track whose drawing and collision,
        // however capped, stretch across nothing
        for (glm::vec3& point : clipped) {
            point = glm::clamp(point, glm::vec3(-MAX_POINT_COORDINATE), glm::vec3(MAX_POINT_COORDINATE));
        }
        if (_trackPoints != clipped) {
            _trackPoints = clipped;
            _trackVersion++;
            _needsRenderUpdate = true;
        }
    });
}

QVector<glm::vec3> TrackEntityItem::getTrackPoints() const {
    return resultWithReadLock<QVector<glm::vec3>>([&] { return _trackPoints; });
}

void TrackEntityItem::setTrackBank(const QVector<float>& bank) {
    if (!allFinite(bank)) {
        return;
    }
    withWriteLock([&] {
        QVector<float> clipped = bank.size() > MAX_POINTS ? bank.mid(0, MAX_POINTS) : bank;
        if (_trackBank != clipped) {
            _trackBank = clipped;
            _trackVersion++;
            _needsRenderUpdate = true;
        }
    });
}

QVector<float> TrackEntityItem::getTrackBank() const {
    return resultWithReadLock<QVector<float>>([&] { return _trackBank; });
}

void TrackEntityItem::setTrackDwell(const QVector<float>& dwell) {
    if (!allFinite(dwell)) {
        return;
    }
    withWriteLock([&] {
        QVector<float> clipped = dwell.size() > MAX_POINTS ? dwell.mid(0, MAX_POINTS) : dwell;
        if (_trackDwell != clipped) {
            _trackDwell = clipped;
            _trackVersion++;
        }
    });
}

QVector<float> TrackEntityItem::getTrackDwell() const {
    return resultWithReadLock<QVector<float>>([&] { return _trackDwell; });
}

void TrackEntityItem::setTrackClosed(bool closed) {
    withWriteLock([&] {
        if (_trackClosed != closed) {
            _trackClosed = closed;
            _trackVersion++;
            _needsRenderUpdate = true;
        }
    });
}

bool TrackEntityItem::getTrackClosed() const {
    return resultWithReadLock<bool>([&] { return _trackClosed; });
}

void TrackEntityItem::setTrackSpeed(float speed) {
    if (!std::isfinite(speed)) {
        return;   // glm::clamp keeps a NaN
    }
    speed = glm::clamp(speed, 0.0f, MAX_SPEED);
    withWriteLock([&] {
        if (_trackSpeed != speed) {
            _trackSpeed = speed;
            _trackVersion++;
        }
    });
}

float TrackEntityItem::getTrackSpeed() const {
    return resultWithReadLock<float>([&] { return _trackSpeed; });
}

void TrackEntityItem::setTrackAcceleration(float acceleration) {
    if (!std::isfinite(acceleration)) {
        return;
    }
    acceleration = glm::clamp(acceleration, 0.0f, MAX_ACCELERATION);
    withWriteLock([&] {
        if (_trackAcceleration != acceleration) {
            _trackAcceleration = acceleration;
            _trackVersion++;
        }
    });
}

float TrackEntityItem::getTrackAcceleration() const {
    return resultWithReadLock<float>([&] { return _trackAcceleration; });
}

uint32_t TrackEntityItem::getTrackVersion() const {
    return resultWithReadLock<uint32_t>([&] { return _trackVersion; });
}

void TrackEntityItem::setTrackProfile(const QString& profile) {
    withWriteLock([&] {
        if (_trackProfile != profile) {
            _trackProfile = profile;
            _guidewayVersion++;
            _needsRenderUpdate = true;
        }
    });
}

QString TrackEntityItem::getTrackProfile() const {
    return resultWithReadLock<QString>([&] { return _trackProfile; });
}

#define TRACK_GUIDEWAY_FLOAT(Name, member, minimum) \
void TrackEntityItem::setTrack##Name(float value) { \
    if (!std::isfinite(value)) { \
        return; /* glm::clamp keeps a NaN, and a NaN is never equal to the stored value: a rebuild at every edit */ \
    } \
    value = glm::clamp(value, minimum, MAX_GUIDEWAY_SIZE); \
    withWriteLock([&] { \
        if (member != value) { \
            member = value; \
            _guidewayVersion++; \
            _needsRenderUpdate = true; \
        } \
    }); \
} \
float TrackEntityItem::getTrack##Name() const { \
    return resultWithReadLock<float>([&] { return member; }); \
}

TRACK_GUIDEWAY_FLOAT(Width, _trackWidth, 0.01f)
TRACK_GUIDEWAY_FLOAT(Height, _trackHeight, 0.01f)
TRACK_GUIDEWAY_FLOAT(Clearance, _trackClearance, -MAX_GUIDEWAY_SIZE)
TRACK_GUIDEWAY_FLOAT(PylonSpacing, _trackPylonSpacing, 0.0f)
TRACK_GUIDEWAY_FLOAT(PylonFloor, _trackPylonFloor, -MAX_GUIDEWAY_SIZE)
#undef TRACK_GUIDEWAY_FLOAT

void TrackEntityItem::setColor(const glm::u8vec3& color) {
    withWriteLock([&] {
        if (_color != color) {
            _color = color;
            _needsRenderUpdate = true;
        }
    });
}

glm::u8vec3 TrackEntityItem::getColor() const {
    return resultWithReadLock<glm::u8vec3>([&] { return _color; });
}

uint32_t TrackEntityItem::getGuidewayVersion() const {
    return resultWithReadLock<uint32_t>([&] { return _guidewayVersion; });
}

// FROZEN (vehicle path): see the note at the top of this file.
#define TRAIN_STATE_SETTER(Setter, Getter, Type, member) \
    void TrackEntityItem::Setter(Type value) { \
        withWriteLock([&] { member = value; }); \
    } \
    Type TrackEntityItem::Getter() const { \
        return resultWithReadLock<Type>([&] { return member; }); \
    }

// a script can write these, and a NaN would put every follower into a residual no later state clears (and persist)
#define TRAIN_STATE_FLOAT_SETTER(Setter, Getter, member) \
    void TrackEntityItem::Setter(float value) { \
        if (!std::isfinite(value)) { \
            return; \
        } \
        withWriteLock([&] { member = value; }); \
    } \
    float TrackEntityItem::Getter() const { \
        return resultWithReadLock<float>([&] { return member; }); \
    }

TRAIN_STATE_FLOAT_SETTER(setTrainDistance, getTrainDistance, _trainDistance)
TRAIN_STATE_FLOAT_SETTER(setTrainSpeed, getTrainSpeed, _trainSpeed)
TRAIN_STATE_SETTER(setTrainReversed, getTrainReversed, bool, _trainReversed)
TRAIN_STATE_FLOAT_SETTER(setTrainDwell, getTrainDwell, _trainDwell)
TRAIN_STATE_SETTER(setTrainStation, getTrainStation, quint16, _trainStation)

// FROZEN (vehicle path): see the note at the top of this file.
// the time is written last, by the wire and by the server, so it is the one that counts a state
void TrackEntityItem::setTrainTime(quint64 value) {
    withWriteLock([&] {
        _trainTime = value;
        _trainStateVersion++;
    });
}

quint64 TrackEntityItem::getTrainTime() const {
    return resultWithReadLock<quint64>([&] { return _trainTime; });
}

TrackEntityItem::TrainState TrackEntityItem::getTrainState() const {
    return resultWithReadLock<TrainState>([&] {
        TrainState state;
        state.distance = _trainDistance;
        state.speed = _trainSpeed;
        state.reversed = _trainReversed;
        state.dwell = _trainDwell;
        state.station = _trainStation;
        state.time = _trainTime;
        state.version = _trainStateVersion;
        return state;
    });
}

uint32_t TrackEntityItem::getTrainStateVersion() const {
    return resultWithReadLock<uint32_t>([&] { return _trainStateVersion; });
}
