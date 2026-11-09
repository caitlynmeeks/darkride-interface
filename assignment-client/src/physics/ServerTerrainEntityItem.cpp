//
//  ServerTerrainEntityItem.cpp
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

#include "ServerTerrainEntityItem.h"

#include <atomic>

#include <glm/gtc/matrix_transform.hpp>

#include <QtCore/QUrl>
#include <QtCore/QScopedPointer>
#include <QtCore/QUrlQuery>

#include <EntityTree.h>
#include <PhysicsLogging.h>
#include <ResourceManager.h>
#include <SharedUtil.h>
#include <ResourceRequest.h>
#include <terrain/SurfaceNets.h>
#include <terrain/TerrainFile.h>
#include <terrain/VoxelGrid.h>

// The serial in a shape key ("terrain/<id>/<serial>") comes from one counter for the whole process: per entity object it
// restarted at 1 whenever the entity was made again (leaving and re-entering a domain), and the shape manager keeps
// released shapes for a while under their keys, so a changed shape could come back as the old one (10-21 review).
static uint64_t nextShapeSerial() {
    static std::atomic<uint64_t> serial { 0 };
    return ++serial;
}

EntityItemPointer ServerTerrainEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    std::shared_ptr<ServerTerrainEntityItem> entity(new ServerTerrainEntityItem(entityID), [](ServerTerrainEntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

ServerTerrainEntityItem::ServerTerrainEntityItem(const EntityItemID& entityItemID) : TerrainEntityItem(entityItemID) {
}

ServerTerrainEntityItem::~ServerTerrainEntityItem() {
    dropRequest();
}

ShapeType ServerTerrainEntityItem::getShapeType() const {
    return _collisionless ? SHAPE_TYPE_NONE : SHAPE_TYPE_STATIC_MESH;
}

bool ServerTerrainEntityItem::shouldBePhysical() const {
    // EntityItem keeps static meshes out of physics and only models opt back in; the ground must be driven on
    return !isDead() && !isLocalEntity() && getShapeType() != SHAPE_TYPE_NONE;
}

bool ServerTerrainEntityItem::isReadyToComputeShape() const {
    if (getShapeType() == SHAPE_TYPE_NONE) {
        return true;
    }
    return resultWithReadLock<bool>([&] { return _shapeReady; });
}

void ServerTerrainEntityItem::computeShapeInfo(ShapeInfo& info) {
    if (getShapeType() == SHAPE_TYPE_NONE) {
        info.setParams(SHAPE_TYPE_NONE, 0.5f * getScaledDimensions());
        return;
    }
    withReadLock([&] { info = _shapeInfo; });
}

void ServerTerrainEntityItem::update() {
    // The properties are read outside the lock (their getters take the entity's read lock); the fetch and build state
    // under it
    const QString url = getTerrainURL();
    const uint32_t version = getTerrainVersion();
    const glm::ivec3 voxelCount = getVoxelCount();
    bool flatGround = false;
    bool fetchNow = false;
    withWriteLock([&] {
        if (_built && url == _builtURL && version == _builtVersion && voxelCount == _builtVoxelCount) {
            if (_retryTime == 0 || usecTimestampNow() < _retryTime) {
                return;
            }
            _retryTime = 0;   // a failed fetch, tried again
        }
        if ((_request || _building) && url == _requestURL && version == _requestVersion) {
            return;   // on its way, or being built
        }
        if (url.trimmed().isEmpty()) {
            flatGround = true;
        } else {
            fetchNow = true;
        }
    });
    if (flatGround) {
        // no file yet: flat ground halfway up the box, as the client makes it
        dropRequest();
        terrain::VoxelGrid grid(voxelCount);
        grid.fillBelow(0.5f * voxelCount.y);
        withWriteLock([&] {
            _builtURL = url;
            _builtVersion = version;
        });
        build(grid);
        return;
    }
    if (fetchNow) {
        fetch(url, version);
    }
}

void ServerTerrainEntityItem::fetch(const QString& url, uint32_t version) {
    dropRequest();
    QUrl fetchURL(url);
    QUrlQuery query(fetchURL);
    query.addQueryItem("v", QString::number(version));
    fetchURL.setQuery(query);
    ResourceRequest* request = DependencyManager::get<ResourceManager>()->createResourceRequest(this, fetchURL, true, -1, "ServerTerrainEntityItem::fetch");
    const glm::ivec3 voxelCount = getVoxelCount();
    uint64_t serial = 0;
    withWriteLock([&] {
        _requestURL = url;
        _requestVersion = version;
        _request = request;
        serial = ++_fetchSerial;   // this fetch's own number: the handler answers to it, never to an address
        if (!request) {
            _built = true;   // nothing more to try until the entity changes
            _builtURL = url;
            _builtVersion = version;
            _builtVoxelCount = voxelCount;
        }
    });
    if (!request) {
        qCWarning(physics) << "physics-server: terrain file" << url << "cannot be fetched: no request for that kind of URL";
        return;
    }
    request->setCacheEnabled(false);
    // The handler owns the request from the moment it takes it below: it keeps the object alive until it returns, on
    // every way out, and lets it go only then. While the file is being read and built the entity counts as still
    // fetching (_building), so update() asks once per URL and version.
    connect(request, &ResourceRequest::finished, this, [this, request, url, version, serial] {
        bool mine = false;
        withWriteLock([&] {
            if (_request == request && _fetchSerial == serial) {
                _request = nullptr;
                _building = true;
                mine = true;
            }
        });
        if (!mine) {
            return;   // dropped meanwhile: dropRequest() lets it go
        }
        QScopedPointer<ResourceRequest, QScopedPointerDeleteLater> holder(request);
        const ResourceRequest::Result result = request->getResult();
        const QString resultString = request->getResultString();
        const QByteArray data = request->getData();
        const glm::ivec3 expected = getVoxelCount();
        auto done = [this] { withWriteLock([&] { _building = false; }); };
        withWriteLock([&] {
            _builtURL = url;
            _builtVersion = version;
        });
        if (result != ResourceRequest::Success) {
            const quint64 RETRY_INTERVAL = 10 * USECS_PER_SECOND;
            qCWarning(physics) << "physics-server: terrain file" << url << "could not be fetched:" << resultString
                << "(trying again in 10 s)";
            withWriteLock([&] {
                _built = true;
                _builtVoxelCount = expected;
                _retryTime = usecTimestampNow() + RETRY_INTERVAL;
            });
            done();
            return;
        }
        terrain::VoxelGrid grid;
        std::string error;
        if (!terrain::loadTerrain(reinterpret_cast<const uint8_t*>(data.constData()), (size_t)data.size(), grid, nullptr, nullptr,
                                  &error)) {
            qCWarning(physics) << "physics-server: terrain file" << url << "is not usable:" << QString::fromStdString(error);
            withWriteLock([&] {
                _built = true;
                _builtVoxelCount = expected;
            });
            done();
            return;
        }
        if (grid.getVoxelCount() != expected) {
            qCWarning(physics) << "physics-server: terrain" << getID() << "file holds" << grid.getVoxelCount().x << "x" << grid.getVoxelCount().y
                << "x" << grid.getVoxelCount().z << "voxels but the entity's dimensions hold" << expected.x << "x" << expected.y << "x" << expected.z;
            withWriteLock([&] {
                _built = true;
                _builtVoxelCount = expected;
            });
            done();
            return;
        }
        build(grid);
        done();
    });
    request->send();
}

void ServerTerrainEntityItem::dropRequest() {
    ResourceRequest* request = nullptr;
    withWriteLock([&] {
        request = _request;
        _request = nullptr;
    });
    if (request) {
        request->disconnect(this);
        request->deleteLater();
    }
}

glm::mat4 ServerTerrainEntityItem::voxelToLocalMatrix(const glm::ivec3& voxelCount) const {
    // voxel (0, 0, 0) is the box's low corner; the box's dimensions over the voxel count is the voxel's size
    glm::vec3 dimensions = getScaledDimensions();
    glm::vec3 scale = dimensions / glm::vec3(glm::max(voxelCount, glm::ivec3(1)));
    return glm::translate(glm::mat4(1.0f), -0.5f * dimensions) * glm::scale(glm::mat4(1.0f), scale);
}

// Every chunk the surface crosses is meshed and the meshes are gathered into one static mesh in the entity's local
// space, the same points the client's collision shape is built from.
void ServerTerrainEntityItem::build(const terrain::VoxelGrid& grid) {
    const glm::ivec3 voxelCount = grid.getVoxelCount();
    const glm::mat4 voxelToLocal = voxelToLocalMatrix(voxelCount);
    ShapeInfo::PointList points;
    ShapeInfo::TriangleIndices indices;
    int meshed = 0;
    terrain::ChunkSamples samples;
    grid.forEachChunk([&](const terrain::ChunkKey& chunk, const terrain::Chunk&) {
        grid.sampleChunk(chunk, samples);
        if (samples.isOneSided()) {
            return;
        }
        terrain::ChunkMesh mesh = terrain::meshChunk(samples);
        if (mesh.isEmpty()) {
            return;
        }
        ++meshed;
        // the mesh is in the chunk's own voxels, from its origin (a finer chunk's are smaller): as the client's
        // collision shape (RenderableTerrainEntityItem::updateCollisionShape)
        const glm::vec3 origin(chunk.getOrigin());
        const float perVoxel = 1.0f / (float)chunk.getScale();
        const uint32_t base = (uint32_t)points.size();
        for (const glm::vec3& position : mesh.positions) {
            points.push_back(glm::vec3(voxelToLocal * glm::vec4(position * perVoxel + origin, 1.0f)));
        }
        for (uint32_t index : mesh.indices) {
            indices.push_back((int32_t)(base + index));
        }
    });

    _shapeSerial = nextShapeSerial();
    ShapeInfo info;
    if (points.size() >= 3 && indices.size() >= 3) {
        info.setParams(SHAPE_TYPE_STATIC_MESH, 0.5f * getScaledDimensions(),
                       QString("terrain/%1/%2").arg(getID().toString()).arg(_shapeSerial));
        info.setPointCollection({ points });
        info.getTriangleIndices() = std::move(indices);
        adjustShapeInfoByRegistration(info);
    } else {
        info.setParams(SHAPE_TYPE_NONE, 0.5f * getScaledDimensions());
    }
    _triangles = info.getTriangleIndices().size() / 3;
    QString builtURL;
    withWriteLock([&] {
        builtURL = _builtURL;
        _shapeInfo = info;
        _shapeReady = true;
        _built = true;
        _builtVoxelCount = voxelCount;
        if (!_collisionless) {
            _flags |= Simulation::DIRTY_SHAPE | Simulation::DIRTY_MASS;
        }
    });
    if (auto tree = getTree()) {
        tree->entityChanged(getThisPointer());
    }
    qCInfo(physics) << "physics-server: terrain" << getName() << getID() << "built:" << meshed << "chunks," << points.size()
        << "points," << _triangles << "triangles from" << (builtURL.isEmpty() ? QString("flat ground") : builtURL);
}
