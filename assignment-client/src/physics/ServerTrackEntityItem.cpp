//
//  ServerTrackEntityItem.cpp
//  assignment-client/src/physics
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the whole file.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "ServerTrackEntityItem.h"

#include <atomic>

#include <EntityTree.h>
#include <PhysicsLogging.h>
#include <SimulationFlags.h>
#include <TrackCurve.h>
#include <TrackGuideway.h>

// The serial in a shape key ("track/<id>/<serial>") comes from one counter for the whole process: per entity object it
// restarted at 1 whenever the entity was made again (leaving and re-entering a domain), and the shape manager keeps
// released shapes for a while under their keys, so a changed shape could come back as the old one (10-21 review).
static uint64_t nextShapeSerial() {
    static std::atomic<uint64_t> serial { 0 };
    return ++serial;
}

EntityItemPointer ServerTrackEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    std::shared_ptr<ServerTrackEntityItem> entity(new ServerTrackEntityItem(entityID), [](ServerTrackEntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

ServerTrackEntityItem::ServerTrackEntityItem(const EntityItemID& entityItemID) : TrackEntityItem(entityItemID) {
}

ShapeType ServerTrackEntityItem::getShapeType() const {
    if (_collisionless || TrackGuidewaySpec::profileFromName(getTrackProfile().toStdString()) == TrackGuidewaySpec::Profile::None) {
        return SHAPE_TYPE_NONE;
    }
    return SHAPE_TYPE_STATIC_MESH;
}

bool ServerTrackEntityItem::shouldBePhysical() const {
    // EntityItem keeps static meshes out of physics and only models opt back in; the beam must be driven on
    return !isDead() && !isLocalEntity() && getShapeType() != SHAPE_TYPE_NONE;
}

bool ServerTrackEntityItem::isReadyToComputeShape() const {
    if (getShapeType() == SHAPE_TYPE_NONE) {
        return true;
    }
    return resultWithReadLock<bool>([&] { return _shapeReady; });
}

void ServerTrackEntityItem::computeShapeInfo(ShapeInfo& info) {
    if (getShapeType() == SHAPE_TYPE_NONE) {
        info.setParams(SHAPE_TYPE_NONE, 0.5f * getScaledDimensions());
        return;
    }
    withReadLock([&] { info = _shapeInfo; });
}

glm::vec3 ServerTrackEntityItem::registrationOffset() const {
    // the curve is in the entity's own space, from its registration point; the shape is in box-centred space
    return (glm::vec3(0.5f) - getRegistrationPoint()) * getScaledDimensions();
}

void ServerTrackEntityItem::update() {
    if (getShapeType() == SHAPE_TYPE_NONE) {
        return;
    }
    if (!_built || _builtTrackVersion != getTrackVersion() || _builtGuidewayVersion != getGuidewayVersion()
            || _builtOffset != registrationOffset()) {
        build();
    }
}

void ServerTrackEntityItem::build() {
    uint32_t trackVersion = getTrackVersion();
    uint32_t guidewayVersion = getGuidewayVersion();
    glm::vec3 offset = registrationOffset();

    QVector<glm::vec3> points = getTrackPoints();
    QVector<float> bank = getTrackBank();
    TrackCurve curve;
    curve.build(std::vector<glm::vec3>(points.begin(), points.end()), std::vector<float>(bank.begin(), bank.end()), getTrackClosed());
    TrackGuidewaySpec spec;
    spec.profile = TrackGuidewaySpec::profileFromName(getTrackProfile().toStdString());
    spec.width = getTrackWidth();
    spec.height = getTrackHeight();
    spec.clearance = getTrackClearance();
    spec.pylonSpacing = getTrackPylonSpacing();
    spec.pylonFloor = getTrackPylonFloor();
    TrackGuidewayMesh mesh;
    TrackGuideway::build(curve, spec, mesh);

    _shapeSerial = nextShapeSerial();
    ShapeInfo info;
    if (mesh.vertices.size() >= 3 && mesh.indices.size() >= 3) {
        ShapeInfo::PointList shapePoints;
        shapePoints.reserve(mesh.vertices.size());
        for (const TrackGuidewayMesh::Vertex& vertex : mesh.vertices) {
            shapePoints.push_back(vertex.position - offset);
        }
        ShapeInfo::TriangleIndices indices;
        indices.reserve(mesh.indices.size());
        for (uint32_t index : mesh.indices) {
            indices.push_back((int32_t)index);
        }
        info.setParams(SHAPE_TYPE_STATIC_MESH, 0.5f * getScaledDimensions(),
                       QString("track/%1/%2").arg(getID().toString()).arg(_shapeSerial));
        info.setPointCollection({ shapePoints });
        info.getTriangleIndices() = std::move(indices);
        adjustShapeInfoByRegistration(info);
    } else {
        info.setParams(SHAPE_TYPE_NONE, 0.5f * getScaledDimensions());
    }
    _triangles = info.getTriangleIndices().size() / 3;
    withWriteLock([&] {
        _shapeInfo = info;
        _shapeReady = true;
        _built = true;
        _builtTrackVersion = trackVersion;
        _builtGuidewayVersion = guidewayVersion;
        _builtOffset = offset;
        if (!_collisionless) {
            _flags |= Simulation::DIRTY_SHAPE | Simulation::DIRTY_MASS;
        }
    });
    if (auto tree = getTree()) {
        tree->entityChanged(getThisPointer());
    }
    qCInfo(physics) << "physics-server: track" << getName() << getID() << "guideway built:" << mesh.vertices.size()
        << "points," << _triangles << "triangles," << curve.length() << "m";
}
