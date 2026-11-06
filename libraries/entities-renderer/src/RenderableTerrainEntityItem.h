//
//  RenderableTerrainEntityItem.h
//  libraries/entities-renderer/src
//
//  The Terrain entity on a client (TERRAIN.md). The entity item holds the voxel grid and the paint map over it,
//  fetched from the terrain file the entity points at or made flat and unpainted when it has none yet, the chunk
//  meshes built from the grid by surface nets, and the collision shape built from those. Chunks are named by level
//  and coordinates (terrain::ChunkKey): a chunk split for finer ground is eight in place of one, and the mesh of a
//  chunk that is gone stays up until the chunks now in its place have theirs.
//
//  What is drawn is chosen by distance from the camera (terrain::LevelOfDetail): the chunks' own meshes near it,
//  meshes of coarser nodes, each standing for many chunks at every 2nd, 4th... voxel, further away. Collision and
//  picking keep the chunks' own meshes everywhere. The renderer puts one render item per node drawn in the scene,
//  an opaque shape with the terrain's own pipeline and a shadow caster, under the entity's item, which only groups
//  them; each draws with a tile of the paint map cut for its column (a level-0 chunk's column for the grid's
//  chunks, read at the chunk's own place and scale within it; a coarser node's own wider column, the map averaged
//  down), remade when the map changes there.
//
//  The grid, the map, the meshes and the shape are the main thread's: the renderer's update, the editor's brushes
//  and the physics simulation all run there. Only the meshing itself runs elsewhere: the update samples the dirty
//  chunks and the nodes wanted, hands the samples to the terrain library's thread pool (terrain::AsyncMesher), and
//  installs the meshes as they come back. The renderer comes back every frame, since the camera may move.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderableTerrainEntityItem_h
#define hifi_RenderableTerrainEntityItem_h

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>

#include <render/Engine.h>

#include <TextureCache.h>
#include <terrain/AsyncMesher.h>
#include <terrain/Layers.h>
#include <terrain/LevelOfDetail.h>
#include <terrain/PaintMap.h>
#include <terrain/SurfaceNets.h>
#include <terrain/TopSurface.h>
#include <terrain/VoxelGrid.h>
#include <TerrainEntityItem.h>

#include "RenderableEntityItem.h"

class ResourceRequest;

namespace render { namespace entities {
class TerrainEntityRenderer;
} }

// A mesh's vertex buffer as the drawing wants it (TerrainChunkPayload: de-indexed, the packed TerrainVertex), made
// on the mesher's thread right after the mesh (the mesh finisher, terrain::AsyncMesher::setFinisher) and kept with
// it, so the editing thread hands a ready buffer to a chunk item and builds nothing there (entry 24 step 3a). Before
// a cut's meshes are drawn the buffers are given to the backend over frames (TerrainUploadPayload, step 3b): once a
// batch has referenced one, the backend has it and it is resident, and a new cut is shown only when every buffer of
// it is, so its first draw uploads nothing.
struct TerrainMeshBuffer {
    using Pointer = std::shared_ptr<TerrainMeshBuffer>;
    gpu::BufferPointer buffer;
    uint32_t vertexCount { 0 };
    uint64_t uploadSerial { 0 };   // its place in the upload queue (the editing thread's), 0 until it is in it
    // Entry 24 step 3d: only a buffer something waits for is sent. wanted: the serial of the terrain's last update
    // whose readiness pass (a pending cut's nodes) or drawn set (an item still waiting) found it not resident; it
    // counts only while it equals the terrain's current serial, so the marks of an abandoned cut expire with the next
    // update (the editing thread's). posted: the upload post carrying it, 0 until one does.
    uint64_t wanted { 0 };
    uint64_t posted { 0 };
    bool resident { false };       // a batch has referenced it (the editing thread's view of it)
    static Pointer make(const terrain::ChunkMesh& mesh);   // the buffer, on whichever thread
};
// The buffers made on the pool, as they come, for the editing thread to queue (shared with the finisher, which may
// outlive the terrain while a job finishes).
struct TerrainMadeBuffers {
    std::mutex mutex;
    std::vector<TerrainMeshBuffer::Pointer> made;
};

class RenderableTerrainEntityItem : public TerrainEntityItem {
    friend class render::entities::TerrainEntityRenderer;
public:
    static EntityItemPointer factory(const EntityItemID& entityID, const EntityItemProperties& properties);
    RenderableTerrainEntityItem(const EntityItemID& entityItemID);

    struct ChunkMeshEntry {
        terrain::ChunkMesh mesh;
        TerrainMeshBuffer::Pointer buffer;   // made with the mesh on the pool (the mesh finisher)
        uint64_t version { 0 };   // the mesh version the mesh was built at
    };
    using ChunkMeshes = std::map<terrain::ChunkKey, ChunkMeshEntry>;

    terrain::VoxelGrid& getGrid() { return _grid; }
    const terrain::VoxelGrid& getGrid() const { return _grid; }
    bool hasGrid() const { return !_grid.isEmpty(); }

    // The paint map over the grid's footprint; there is one whenever there is a grid (the file's, or a fresh one
    // at about PAINT_TEXEL_METRES a texel when the file had none).
    terrain::PaintMap& getPaint() { return _paint; }
    const terrain::PaintMap& getPaint() const { return _paint; }
    // Texels have changed outside the renderer's own update (a brush, an undo, a file that arrived): the renderer
    // remakes the tiles of the columns the map marks dirty.
    void paintEdited() { ++_paintVersion; somethingChangedNotification(); }
    uint64_t getPaintVersion() const { return _paintVersion; }
    // The ground the sky sees (terrain::TopSurface): the paint map lies on it alone, so a cave's floor shows its
    // voxels' materials. It follows the grid's edits; updateTopSurface() takes those made since its last call (the
    // renderer's update every frame, and the editor before it asks where a stroke landed).
    terrain::TopSurface& getTopSurface() { return _topSurface; }
    const terrain::TopSurface& getTopSurface() const { return _topSurface; }
    void updateTopSurface() { _topSurface.update(_grid); }
    static const float PAINT_TEXEL_METRES;
    static const int MAX_PAINT_TEXELS_PER_AXIS;
    // How many texels a voxel of voxelMetres gets, so a texel is about PAINT_TEXEL_METRES, within the map's size limit.
    static int paintTexelsPerVoxel(float voxelMetres, const glm::ivec2& voxelFootprint);

    // The material layers (terrain::LayerStack): the file's, which the editor's Layers panel sets and the save
    // writes back, or, for a terrain whose file holds none, what the entity's terrainLayers property names (the way
    // layers were given before the file held them) or the defaults. The renderer makes a new layer set whenever
    // the version changes.
    const terrain::LayerStack& getLayers() const { return _layers; }
    // fixed: the stack is the terrain's own (the file's, or the editor's) rather than derived from the property
    void setLayers(const terrain::LayerStack& layers, bool fixed = true);
    uint64_t getLayersVersion() const { return _layersVersion; }
    bool layersAreFixed() const { return _layersFixed; }
    terrain::LayerStack legacyOrDefaultLayers() const;

    // Which terrain file (URL and version) the grid holds, so the renderer knows when the entity points at another;
    // until the first source is set, the entity has no grid at all, whatever its properties say.
    bool hasGridSource() const { return _gridSourceSet; }
    const QString& getGridURL() const { return _gridURL; }
    uint32_t getGridVersion() const { return _gridVersion; }
    void setGridSource(const QString& url, uint32_t version) { _gridURL = url; _gridVersion = version; _gridSourceSet = true; }

    // Flat unpainted ground halfway up the box, for an entity with no terrain file yet.
    void makeDefaultGrid();
    // Replaces the grid and the paint map with a terrain file's contents; false, with both untouched, when the
    // file is bad.
    bool loadGrid(const QByteArray& file, QString& error);
    void clearGrid();
    // Replaces the grid and the paint map with others made outside the renderer's update (the editor's Resample,
    // and its undo): the meshes follow, the collision shape too, and the renderer is told to come back.
    void setGridAndPaint(terrain::VoxelGrid grid, terrain::PaintMap paint);
    // Which faces of the box are closed (terrain::CAP_ bits, VoxelGrid::setCaps): the ground is capped at a closed
    // face, open at an open one. Kept in the terrain file with the grid; a change re-meshes every chunk.
    uint8_t getCaps() const { return _grid.getCaps(); }
    void setCaps(uint8_t caps);

    // Installs the meshes that have come back from the mesher's threads and sends out the chunks the grid marks
    // dirty; returns whether any mesh changed. Called from the renderer's update, every frame while any is out.
    bool updateMeshes();
    // Every chunk's mesh matches the grid: nothing dirty, nothing out, nothing waiting to be installed.
    bool meshesAreSettled() const { return _grid.getDirtyChunks().empty() && _mesher.isIdle(); }
    const ChunkMeshes& getChunkMeshes() const { return _chunkMeshes; }
    uint64_t getMeshVersion() const { return _meshVersion; }

    // Chooses what to draw for a camera at a point of the world (terrain::LevelOfDetail): called by the renderer's
    // update every frame, after updateMeshes(). Returns whether what is drawn changed.
    bool updateLevelOfDetail(const glm::vec3& cameraWorld);
    struct DrawnChunk {
        terrain::ChunkKey key;                       // the chunk or coarser node whose voxels the mesh is in
        const terrain::ChunkMesh* mesh { nullptr };
        uint64_t version { 0 };                      // changes whenever the mesh does
        TerrainMeshBuffer::Pointer buffer;           // its vertex buffer, ready; null only for a mesh made without one
    };
    // What to draw now: a mesh each for the nodes chosen that have a surface and their mesh already.
    void getDrawnChunks(std::vector<DrawnChunk>& drawn) const;
    // Changes whenever getDrawnChunks() may give something else.
    uint64_t getDrawnVersion() const { return _levelOfDetail.getDrawnVersion() + _meshVersion; }
    // The vertex buffers made beside the meshes on the pool, queued in the order they came (entry 24 steps 3b and
    // 3d). Only a buffer something waits for is sent (wantBuffer, in updateLevelOfDetail: the pending cut's readiness
    // checks and the drawn set's items still waiting mark them, and the marks expire with the next update, so an
    // abandoned cut sends nothing): getUploadsDue gives the wanted ones in queue order, the backlog whole up to a share of
    // bytes (the first always), as one post for the renderer to reference from a batch; the same post again until it
    // says the post has gone through (uploadsFed, with the post's serial), which
    // marks those resident, lets a cut waiting on them show, and changes the drawn version. A buffer nobody waits for
    // stays in the queue, live and unsent, until a cut wants it or its mesh is gone (the level of detail evicted the
    // entry or replaced its mesh, or the grid's chunk was meshed again: the queue's reference is the last, and the
    // buffer leaves), so the queue is bounded by the mesh cache, never by the ride's length.
    void wantBuffer(const TerrainMeshBuffer::Pointer& buffer);
    void getUploadsDue(size_t maxShare, std::vector<TerrainMeshBuffer::Pointer>& due, uint64_t& serial);
    void uploadsFed(uint64_t serial);
    size_t getUploadsPending() const { return _uploads.size(); }
    size_t getUploadsWanted() const { return _uploadsWanted; }
    size_t getUploadsWantedBytes() const { return _uploadsWantedBytes; }
    // What is drawn, for the log: nodes by level, how many have meshes, and those without one with their state.
    QString describeDrawn() const;
    // How far out every terrain on this client draws full detail: a part of the ground is drawn finer while the
    // camera is within this many times its size of it (terrain::LodSettings::detail; 0 draws full detail
    // everywhere). The editor's Terrain row sets it.
    static void setLevelOfDetail(float detail);
    static float getLevelOfDetail();

    // Rebuilds the collision shape from the meshes, one static mesh. It costs a Bullet tree build, so the editor
    // asks for it when a stroke ends rather than at every step, a loaded or replaced grid asks for it too, and the
    // renderer builds it once the meshes have settled.
    void updateCollisionShape();
    bool collisionShapeIsStale() const { return _shapeStale; }

    // The editor's brushes edit the grid in place; a stroke's end asks for the collision shape to follow.
    void markCollisionShapeStale() { _shapeStale = true; }
    // Voxels have changed outside the renderer's own update (a brush, an undo, a file that arrived): the renderer
    // only comes to entities that say they changed, so this says so, and it meshes the dirty chunks next frame.
    void gridEdited() { somethingChangedNotification(); }

    // The size of a voxel in the entity's local space: the box's dimensions over the grid's voxel count, so scaling
    // the entity stretches the ground with the box. The voxelSize property is the size the grid was made at.
    glm::vec3 getVoxelScale() const;
    // Voxel space (voxel units, origin at the box's low corner) to the entity's box-centred local space.
    glm::mat4 voxelToLocalMatrix() const;
    glm::mat4 voxelToWorldMatrix() const;
    glm::mat4 worldToVoxelMatrix() const;

    // Picking lands on the ground itself, not the box: a click selects the terrain where there is ground under it,
    // and the editor's brushes find where to work. The ray is in voxel space, its direction as long as it likes:
    // t is the parameter along it (the same in any space the ray is mapped to affinely), the normal in voxel space.
    bool rayHitsGround(const glm::vec3& origin, const glm::vec3& direction, float& t, glm::vec3& normal) const;
    bool supportsDetailedIntersection() const override { return true; }
    bool findDetailedRayIntersection(const glm::vec3& origin, const glm::vec3& direction, OctreeElementPointer& element,
                                     float& distance, BoxFace& face, glm::vec3& surfaceNormal, QVariantMap& extraInfo,
                                     bool precisionPicking) const override;

    void setUnscaledDimensions(const glm::vec3& value) override;

    ShapeType getShapeType() const override;
    bool shouldBePhysical() const override;
    bool isReadyToComputeShape() const override;
    void computeShapeInfo(ShapeInfo& info) override;

private:
    // The grid becomes this one; meshes of chunks the new grid will mesh again stay up until it has, so a reload
    // does not blink, and the others go. The paint map becomes the given one when it covers the grid, otherwise a
    // fresh one.
    void replaceGrid(terrain::VoxelGrid grid, terrain::PaintMap paint);

    terrain::VoxelGrid _grid;
    terrain::PaintMap _paint;
    uint64_t _paintVersion { 0 };
    terrain::TopSurface _topSurface;
    terrain::LayerStack _layers;
    uint64_t _layersVersion { 0 };
    bool _layersFixed { false };
    bool _gridSourceSet { false };
    QString _gridURL;
    uint32_t _gridVersion { 0 };
    terrain::AsyncMesher _mesher;
    ChunkMeshes _chunkMeshes;
    terrain::LevelOfDetail _levelOfDetail;
    // the vertex buffers from the pool (TerrainMeshBuffer, the mesh finisher of both meshers), queued for the backend
    std::shared_ptr<TerrainMadeBuffers> _madeBuffers;
    std::deque<TerrainMeshBuffer::Pointer> _uploads;
    size_t _uploadsWanted { 0 };        // of the queue: wanted and not yet resident, as of the last getUploadsDue
    size_t _uploadsWantedBytes { 0 };
    uint64_t _nextUploadSerial { 0 };
    uint64_t _nextPostSerial { 0 };
    uint64_t _postOut { 0 };            // the post referenced by the upload item and not yet fed; 0 when none
    uint64_t _wantSerial { 1 };         // one per level-of-detail update: the mark a wanted buffer carries (0 = none)
    void takeMadeBuffers();
    void pruneUploads();                // the resident and the dead leave the queue
    uint64_t _meshVersion { 0 };
    uint64_t _structureVersion { 0 };   // the grid's, when the meshes were last checked against its chunks
    bool _staleMeshes { false };        // meshes of chunks that are gone wait for their replacements' meshes
    uint64_t _stallEventsLogged { 0 };  // the level of detail's stall events already in the log
    // The frames this renderer is given, to size the level of detail's sampling budget as a share of one
    // (updateLevelOfDetail): the last periods between updates, in ms, and the update before
    std::array<float, 16> _framePeriodsMs {};
    uint64_t _framePeriodCount { 0 };
    std::chrono::steady_clock::time_point _lastLodUpdate;
    uint64_t _cutShownLogged { 0 };     // the level of detail's cut-shown serial already in the log
    uint64_t _reChoiceLogged { 0 };     // and its re-choice serial
    ShapeInfo _shapeInfo;
    bool _shapeReady { false };
    bool _shapeStale { false };
    uint64_t _shapeSerial { 0 };
};

namespace render { namespace entities {

// A terrain's layers as its chunk items draw them: the parameter buffer the terrain shader reads (TERRAIN.md
// section 5) and three texture arrays with a slice per slot, so the eight layers take three sampler slots: the
// colours (sRGB), the normals (the map's xyz, 0 to 1; flat for a slot without one) and the masks (r roughness,
// g occlusion, b height); shared by every chunk item of one entity. Made on the main thread, which starts the
// texture fetches, handed to the items through the scene's transaction, and used on the render thread from then on.
// The textures come in through the texture cache; as each arrives its slice is drawn from it on the GPU
// (terrain_layer_maps.slf, a quad over the slice, which also scales the map to the slice), since the macOS GL
// backend takes no array slice from memory, by the DrawTerrainLayerMaps job at the start of the frame (until
// 2026-10-19 a chunk's render drew them in a batch slipped in before the pass being recorded, which left the slice's
// framebuffer and viewport bound for that pass: the whole opaque pass of the frame then went into the slice and the
// screen showed a black frame, KNOWN-ISSUES.md); and drawn again as more of the texture reaches the GPU (the backend
// uploads a texture's mips a few at a time, smallest first, and gpu::Texture's residency stamp says when), so the
// slice ends up as sharp as the texture.
// The colour array's slices are as large as the largest colour texture, within bounds, grown as the textures arrive.
// A set made for a changed stack takes over the arrays and the drawn slices of the set it replaces, so a slider's
// scrub draws no slice again.
class TerrainLayerSet {
public:
    using Pointer = std::shared_ptr<TerrainLayerSet>;
    static const int LAYER_COUNT = TerrainEntityItem::MAX_TERRAIN_LAYERS;
    static const int MAP_SIZE = 512;          // a side of a slice of the normal and mask arrays: a map is scaled to it
    static const int ALBEDO_MIN_SIZE = 512;   // of the colour array: the largest colour texture's side, within these
    static const int ALBEDO_MAX_SIZE = 2048;

    // previous: the set this one replaces, whose arrays it takes over
    explicit TerrainLayerSet(const terrain::LayerStack& stack, const Pointer& previous = nullptr);
    ~TerrainLayerSet();

    // World space to the terrain's own: its box's axes, in metres, from the box's centre. The textures are tiled
    // in that space, so they stay put on the ground when the entity moves or turns.
    void setWorldToTerrain(const glm::mat4& worldToTerrain);
    // Draws the slices whose maps have arrived, or changed, since they were drawn, if any, in a batch of its own.
    // Called by drawAllMapsDue at the start of the frame, before any pass has bound its framebuffer, since the batch
    // leaves the slice's bound; nothing to do most frames.
    void drawMapsDue(RenderArgs* args);
    // The same for every live set (the DrawTerrainLayerMaps job).
    static void drawAllMapsDue(RenderArgs* args);
    // Binds the arrays and the parameters.
    void bind(gpu::Batch& batch);

private:
    struct Layer {
        terrain::Layer spec;
        NetworkTexturePointer texture;
        NetworkTexturePointer normalMap;
        NetworkTexturePointer roughnessMap;
        NetworkTexturePointer occlusionMap;
        NetworkTexturePointer heightMap;
        bool textured { false };   // the colour array's slice holds its texture (the plain colour draws until then)
        bool present { false };    // in the stack; a slot without a layer draws a plain colour of its own
    };
    // What a slice holds, as terrain_layer_maps.slf is told.
    enum class Map { Normal = 0, Mask = 1, Albedo = 2 };
    using Sources = std::array<gpu::TexturePointer, 3>;   // what a slice is drawn from: the colour texture; the normal map; or the roughness, occlusion and height maps
    using Stamps = std::array<gpu::Stamp, 3>;             // the sources' residency stamps when the slice was drawn
    // One slice of an array: what it was last drawn from, to draw it again when that changes.
    struct Slice {
        bool drawn { false };
        Sources sources;
        Stamps stamps {};
    };
    using Slices = std::array<Slice, LAYER_COUNT>;
    static gpu::TexturePointer makeArray(const char* name, const gpu::Element& format, int size,
                                         std::vector<gpu::FramebufferPointer>& targets);
    static Stamps stampsOf(const Sources& sources);
    void makeArrays();
    void makeAlbedoArray(int size);
    Sources albedoSources(int slot) const;
    Sources normalSources(int slot) const;
    Sources maskSources(int slot) const;
    void updateParams();

    std::array<Layer, LAYER_COUNT> _layers;   // by slot
    std::vector<uint8_t> _order;              // the slots drawn, bottom to top
    glm::mat4 _worldToTerrain { 1.0f };
    gpu::BufferPointer _params;
    bool _paramsDirty { true };

    gpu::TexturePointer _albedos;                            // the array of the layers' colour textures
    gpu::TexturePointer _normals;                            // the array of their normal maps
    gpu::TexturePointer _masks;                              // the array of their masks
    int _albedoSize { ALBEDO_MIN_SIZE };                     // a side of the colour array's slices
    std::vector<gpu::FramebufferPointer> _albedoTargets;     // a framebuffer on each slice
    std::vector<gpu::FramebufferPointer> _normalTargets;
    std::vector<gpu::FramebufferPointer> _maskTargets;
    Slices _albedoSlices;
    Slices _normalSlices;
    Slices _maskSlices;
    std::mutex _mapsMutex;   // the arrays and slices: drawn on the render thread, taken over by the next set on the main thread

    // the live sets, for drawAllMapsDue: made and destroyed on the main thread, walked on the render thread
    static std::mutex& registryMutex();
    static std::set<TerrainLayerSet*>& registry();
};

// The render engine's job that draws the terrain layer sets' slices due this frame, first thing in the frame
// (GraphicsEngine::initializeRender), so that the framebuffers and viewports the draws leave bound are gone before
// any view's pass relies on its own.
class DrawTerrainLayerMaps {
public:
    using JobModel = render::Job::Model<DrawTerrainLayerMaps>;
    void run(const render::RenderContextPointer& renderContext) { TerrainLayerSet::drawAllMapsDue(renderContext->args); }
};

// One column's tile of the paint map (PaintMap::fillTile: a level-0 chunk column's, or a coarser node's wider one) as
// the two textures the terrain shader reads, made whole on the main thread and never changed after: a column whose
// texels change gets a new tile.
struct TerrainPaintTile {
    using Pointer = std::shared_ptr<const TerrainPaintTile>;
    gpu::TexturePointer layers0to3;
    gpu::TexturePointer layers4to7;

    static Pointer make(const terrain::PaintMap& paint, int level, const glm::ivec2& column);
};

// One chunk's mesh as a render item.
class TerrainChunkPayload {
public:
    using Pointer = std::shared_ptr<TerrainChunkPayload>;

    // The mesh without indices, three vertices to a triangle, each carrying the triangle's three materials and
    // its own corner, so the shader can blend materials across the triangle (terrain.slv). On whichever thread: as a
    // rule the mesher's, through TerrainMeshBuffer::make.
    static gpu::BufferPointer makeVertexBuffer(const terrain::ChunkMesh& mesh);
    static uint32_t getVertexCount(const terrain::ChunkMesh& mesh) { return (uint32_t)mesh.indices.size(); }

    void setBuffers(const gpu::BufferPointer& vertices, uint32_t vertexCount);
    // The transform of the chunk's voxel (0, 0, 0) in the world; the bound follows from it.
    void setPlacement(const Transform& chunkToWorld);
    // Which chunk or node this is: where in its paint tile its voxels are read, and how large they are.
    void setChunk(const terrain::ChunkKey& chunk);
    void setKey(const ItemKey& key) { _key = key; }
    void setWireframe(bool wireframe) { _wireframe = wireframe; }
    void setLayers(const TerrainLayerSet::Pointer& layers) { _layers = layers; }
    void setWorldToTerrain(const glm::mat4& worldToTerrain);
    // The chunk's column's tile; none draws the chunk unpainted.
    void setPaintTile(const TerrainPaintTile::Pointer& tile) { _paintTile = tile; }
    // The heights of the ground the sky sees over the chunk's column (TopSurface::fillTile), under which the paint
    // map does not lie; none (a coarse node, which draws the top alone) paints wherever the ground faces up.
    void setTopTile(const gpu::TexturePointer& tile) { _topTile = tile; }

    const ItemKey& getKey() const { return _key; }
    const Item::Bound& getBound() const { return _bound; }
    ShapeKey getShapeKey() const;
    void render(RenderArgs* args);

private:
    gpu::BufferPointer _vertexBuffer;
    uint32_t _vertexCount { 0 };
    Transform _transform;
    Item::Bound _bound;
    ItemKey _key;
    bool _wireframe { false };
    TerrainLayerSet::Pointer _layers;
    TerrainPaintTile::Pointer _paintTile;
    gpu::TexturePointer _topTile;
    gpu::BufferPointer _chunkParams;
    terrain::ChunkKey _debugKey;   // for the log, with TIVOLI_TERRAIN_DEBUG set
    bool _debugDrawn { false };    // its first draw since its buffers or placement changed is in the log
};

// The item that gives a cut's vertex buffers to the backend before its meshes are drawn (entry 24 step 3b): each
// frame it references the buffers due (the first of the terrain's upload queue, within a share of bytes) from its
// batch and draws nothing; the backend uploads every buffer a batch references as the batch executes, so the
// uploads land over frames instead of in the one execute that first draws a new cut. It records the serial its
// batch reached, for the editing thread. Its bound is the terrain's, so it is culled with it (and the uploads wait).
class TerrainUploadPayload {
public:
    using Pointer = std::shared_ptr<TerrainUploadPayload>;
    explicit TerrainUploadPayload(const std::shared_ptr<std::atomic<uint64_t>>& fed) : _fed(fed) {}
    void setBuffers(std::vector<gpu::BufferPointer> buffers, uint64_t serial) { _buffers = std::move(buffers); _serial = serial; }
    void setBound(const Item::Bound& bound) { _bound = bound; }
    void setKey(const ItemKey& key) { _key = key; }
    const ItemKey& getKey() const { return _key; }
    const Item::Bound& getBound() const { return _bound; }
    void render(RenderArgs* args);
private:
    std::vector<gpu::BufferPointer> _buffers;
    uint64_t _serial { 0 };                        // the post's serial
    std::shared_ptr<std::atomic<uint64_t>> _fed;   // the serial a batch has referenced up to (read on the editing thread)
    Item::Bound _bound;
    ItemKey _key;
};

class TerrainEntityRenderer : public TypedEntityRenderer<RenderableTerrainEntityItem> {
    using Parent = TypedEntityRenderer<RenderableTerrainEntityItem>;
    friend class EntityRenderer;
public:
    TerrainEntityRenderer(const EntityItemPointer& entity);
    ~TerrainEntityRenderer();

protected:
    ItemKey getKey() override;
    uint32_t metaFetchMetaSubItems(ItemIDs& subItems) const override;
    void removeFromScene(const ScenePointer& scene, Transaction& transaction) override;
    bool needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const override;
    void doRenderUpdateSynchronousTyped(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) override;
    void doRenderUpdateAsynchronousTyped(const TypedEntityPointer& entity) override;
    void doRender(RenderArgs* args) override {}   // the chunk items draw; this item only groups them

private:
    void fetchTerrain(const QString& url, uint32_t version);
    // A fetch that failed, or brought a file that cannot be read, is made again a while later, for as long as the
    // entity points at the same file: 1 s, 2 s, 4 s ... then every 30 s
    void fetchTerrainLater(const QString& url, uint32_t version);
    void dropRequest();
    // The tile a chunk or node draws with: its level-0 column's for a chunk of the grid, its own column's for a
    // coarser node; made from the map if there is none yet, null while the map is empty.
    static glm::ivec3 paintTileKey(const terrain::ChunkKey& chunk);   // column x, column z, level
    TerrainPaintTile::Pointer paintTileFor(const terrain::PaintMap& paint, const glm::ivec3& tileKey);
    // The top surface's tile of a chunk's level-0 column, made if there is none yet; null for a coarse node, or while
    // there are no heights.
    gpu::TexturePointer topTileFor(const terrain::TopSurface& top, const terrain::ChunkKey& chunk);
    ItemKey chunkKey() const;
    static Transform chunkToWorld(const Transform& model, const glm::vec3& dimensions, const glm::vec3& voxelScale, const terrain::ChunkKey& chunk);

    struct ChunkItem {
        ItemID id { Item::INVALID_ITEM_ID };
        uint64_t meshVersion { 0 };
    };
    std::map<terrain::ChunkKey, ChunkItem> _chunkItems;
    // the upload item (TerrainUploadPayload) and what it was last given
    ItemID _uploadItem { Item::INVALID_ITEM_ID };
    std::shared_ptr<std::atomic<uint64_t>> _uploadsFed { std::make_shared<std::atomic<uint64_t>>(0) };
    uint64_t _uploadsPosted { 0 };
    std::chrono::steady_clock::time_point _firstDrawn;   // when the first chunk item was made; unset until then
    void postUploads(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity);
    // The chunk items as the render thread asks for them (metaFetchMetaSubItems, for selection outlines): the main
    // thread changes _chunkItems as it likes, and a new item's ID means nothing to the scene until the transaction
    // that resets it has gone through, so the list is handed over with that transaction (the asynchronous update).
    mutable std::mutex _subItemsMutex;
    ItemIDs _pendingSubItems;
    bool _subItemsPending { false };
    ItemIDs _subItems;
    void handOverSubItems();
    ItemKey _lastChunkKey;
    Transform _lastTransform;
    glm::vec3 _lastDimensions { 0.0f };
    glm::vec3 _lastVoxelScale { 0.0f };
    bool _lastWireframe { false };
    uint64_t _drawnVersion { 0 };
    bool _drawnOnce { false };
    std::vector<RenderableTerrainEntityItem::DrawnChunk> _drawn;
    TerrainLayerSet::Pointer _layerSet;
    uint64_t _lastLayersVersion { 0 };
    QString _lastLegacyLayersJSON;
    bool _layerSetMade { false };
    std::map<glm::ivec3, TerrainPaintTile::Pointer, terrain::IVec3Less> _paintTiles;
    uint64_t _paintVersion { 0 };
    std::map<glm::ivec2, gpu::TexturePointer, terrain::IVec2Less> _topTiles;

    ResourceRequest* _request { nullptr };
    QString _requestURL;
    uint32_t _requestVersion { 0 };
    int _fetchFailures { 0 };   // of the file asked for now, in a row
};

} }

namespace render {
template <> const ItemKey payloadGetKey(const entities::TerrainChunkPayload::Pointer& payload);
template <> const Item::Bound payloadGetBound(const entities::TerrainChunkPayload::Pointer& payload);
template <> const ShapeKey shapeGetShapeKey(const entities::TerrainChunkPayload::Pointer& payload);
template <> void payloadRender(const entities::TerrainChunkPayload::Pointer& payload, RenderArgs* args);
template <> const ItemKey payloadGetKey(const entities::TerrainUploadPayload::Pointer& payload);
template <> const Item::Bound payloadGetBound(const entities::TerrainUploadPayload::Pointer& payload);
template <> const ShapeKey shapeGetShapeKey(const entities::TerrainUploadPayload::Pointer& payload);
template <> void payloadRender(const entities::TerrainUploadPayload::Pointer& payload, RenderArgs* args);
}

#endif // hifi_RenderableTerrainEntityItem_h
