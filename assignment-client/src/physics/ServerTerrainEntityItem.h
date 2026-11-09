//
//  ServerTerrainEntityItem.h
//  assignment-client/src/physics
//
//  The Terrain entity on the physics server (PHYSICS.md round 6, TERRAIN.md 8): the voxel grid fetched from the entity's
//  terrain file (or made flat when it has none), meshed chunk by chunk with the terrain library's surface nets, and given
//  to the physics as one static mesh, so a vehicle drives on the same ground the clients see. The client's renderable does
//  the same with a renderer around it (RenderableTerrainEntityItem); this is that without the renderer, built the same way
//  so the two surfaces agree. The server asks it to update once a second (PhysicsServer::updateTerrains).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ServerTerrainEntityItem_h
#define hifi_ServerTerrainEntityItem_h

#include <TerrainEntityItem.h>

namespace terrain {
    class VoxelGrid;
}
class ResourceRequest;

class ServerTerrainEntityItem : public TerrainEntityItem {
public:
    static EntityItemPointer factory(const EntityItemID& entityID, const EntityItemProperties& properties);
    ServerTerrainEntityItem(const EntityItemID& entityItemID);
    ~ServerTerrainEntityItem();

    // the physics: a static mesh of the ground, once it is built
    ShapeType getShapeType() const override;
    bool shouldBePhysical() const override;
    bool isReadyToComputeShape() const override;
    void computeShapeInfo(ShapeInfo& info) override;

    // Fetches the file and builds the shape when the entity points at another file or version, or has another size;
    // nothing when the shape is current. Called from the server's step.
    void update();

    bool hasShape() const { return _shapeReady; }
    size_t getTriangleCount() const { return _triangles; }

private:
    void fetch(const QString& url, uint32_t version);
    void dropRequest();
    void build(const terrain::VoxelGrid& grid);
    glm::mat4 voxelToLocalMatrix(const glm::ivec3& voxelCount) const;

    QString _builtURL;
    uint32_t _builtVersion { 0 };
    glm::ivec3 _builtVoxelCount { 0 };
    bool _built { false };
    quint64 _retryTime { 0 };   // a fetch that failed is tried again from this time (the file's server may be late)
    ResourceRequest* _request { nullptr };
    bool _building { false };
    uint64_t _fetchSerial { 0 };   // counts the fetches; a finished handler acts only for the fetch it was made for   // the fetched file is being read and built: no new fetch for the same URL and version
    QString _requestURL;
    uint32_t _requestVersion { 0 };

    ShapeInfo _shapeInfo;
    bool _shapeReady { false };
    uint64_t _shapeSerial { 0 };
    size_t _triangles { 0 };
};

#endif // hifi_ServerTerrainEntityItem_h
