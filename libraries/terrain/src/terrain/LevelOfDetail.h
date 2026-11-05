//
//  LevelOfDetail.h
//  libraries/terrain/src/terrain
//
//  Level of detail by distance for a terrain (TERRAIN.md section 4): the ground near the camera is drawn from the
//  grid's own chunks, the ground further away from coarser nodes that read every 2nd, 4th, 8th... voxel, so a far
//  hill costs a few hundred triangles instead of tens of thousands and a far ring of the terrain a few dozen draw
//  items instead of thousands. The nodes drawn are a cut through the octree of chunks (VoxelGrid.h) extended upward
//  with coarser levels (negative levels, down to MIN_LOD_LEVEL): a set of nodes covering every place of the grid
//  once, each as coarse as its distance from the camera allows, neighbours at most one level apart (the 2:1 rule),
//  so their meshes meet without a crack as the grid's own chunks of different levels do (SurfaceNets.h). Where the
//  grid's chunks are finer than the camera needs, a node draws them coarser too.
//
//  A node is meshed from samples like a chunk's (ChunkSamples), with the levels around it taken from the cut instead
//  of from the grid's chunks: its own voxels are the grid's at the node's points (the voxel of whichever chunk covers
//  each point, at the node's level or finer, rescaled), and the field between nodes of different levels is read as
//  the grid reads it between chunks of different levels, interpolated between a coarser node's points. A node that
//  is one of the grid's chunks with the grid's own chunks all around it would get exactly the chunk's own samples,
//  so it is drawn with the chunk's own mesh, which the terrain keeps anyway for collisions and picking. Only the
//  others (the coarser nodes, and the chunks along the edges of the rings) have meshes made here.
//
//  Collision and picking keep the grid's chunks' meshes at full detail everywhere: this is for drawing only.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_terrain_LevelOfDetail_h
#define hifi_terrain_LevelOfDetail_h

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "AsyncMesher.h"
#include "SurfaceNets.h"
#include "VoxelGrid.h"

class QThreadPool;

namespace terrain {

struct LodSettings {
    // A node is drawn as its eight children while the camera is nearer to it than this many times its own size
    // (both in metres): a larger value keeps full detail further out. 0 draws the grid's own chunks everywhere.
    float detail { 4.0f };
    // A node drawn as its children stays so until the camera is this fraction further away again, so a camera
    // moving to and fro across the line does not switch it at every step.
    float hysteresis { 0.25f };
    // Time a frame may spend sampling nodes for meshing on the editing thread (the meshing itself runs on a pool).
    float sampleBudgetMs { 4.0f };
    // A new cut is shown only once every node of it has its mesh (LevelOfDetail). A node that has had none for the
    // nudge time is asked for again, in case its mesh was lost; one that still has none at the force time no longer
    // holds the cut back: the cut is shown without it (LevelOfDetail::getStall says which node, and why).
    float stallNudgeMs { 2000.0f };
    float stallForceMs { 6000.0f };

    bool operator==(const LodSettings& other) const {
        return detail == other.detail && hysteresis == other.hysteresis && sampleBudgetMs == other.sampleBudgetMs &&
               stallNudgeMs == other.stallNudgeMs && stallForceMs == other.stallForceMs;
    }
    bool operator!=(const LodSettings& other) const { return !(*this == other); }
};

// The levels of the nodes around a node, less its own (-1 coarser, 0 the same, +1 finer), across each of its six
// faces, twelve edges and eight corners. Places are named by (sx, sy, sz), each -1 (below the node on that axis),
// 0 (beside it) or 1 (above it). A place beyond the grid's side reads what the grid's edge is clamped to there, as
// the samples do.
class NodeContext {
public:
    static const int CENTRE = 13;
    static int indexOf(int sx, int sy, int sz) { return (sx + 1) + 3 * (sy + 1) + 9 * (sz + 1); }
    static glm::ivec3 sidesOf(int index) { return glm::ivec3(index % 3 - 1, (index / 3) % 3 - 1, index / 9 - 1); }

    int get(int sx, int sy, int sz) const { return _deltas[indexOf(sx, sy, sz)]; }
    int at(int index) const { return _deltas[index]; }
    void set(int index, int delta) { _deltas[index] = (int8_t)delta; }
    // Two bits a place: two contexts are the same exactly when their signatures are.
    uint64_t getSignature() const;

    bool operator==(const NodeContext& other) const { return _deltas == other._deltas; }
    bool operator!=(const NodeContext& other) const { return !(*this == other); }

private:
    std::array<int8_t, 27> _deltas {};
};

// The point at which a node's context reads the place on the given side of it (NodeContext's index): just outside
// the node on the axes where the place is below or above it, halfway along the others; in finest units, clamped
// into the grid.
glm::ivec3 contextProbe(const VoxelGrid& grid, const ChunkKey& key, int index);
// The levels of the grid's own chunks around one of them: the context its samples from VoxelGrid::sampleChunk() see.
NodeContext gridContext(const VoxelGrid& grid, const ChunkKey& key);
// The samples a node is meshed from, with the levels around it given by the context: as VoxelGrid::sampleChunk()
// gives a chunk's for meshing, to the bit where the node is one of the grid's chunks and the context the grid's.
void sampleNode(const VoxelGrid& grid, const ChunkKey& key, const NodeContext& context, ChunkSamples& samples);

// The nodes to draw for a camera: a cut through the octree of the grid's chunks extended upward with coarser levels.
class LodCut {
public:
    struct Node {
        ChunkKey key;
        NodeContext context;
        // One of the grid's chunks with the grid's own chunks around it: drawn with the chunk's own mesh.
        bool isGridChunk { false };
        float distance { 0.0f };   // metres from the camera to the node's box (within the grid)

        bool operator==(const Node& other) const {
            return key == other.key && context == other.context && isGridChunk == other.isGridChunk;
        }
        bool operator!=(const Node& other) const { return !(*this == other); }
    };

    // Chooses the nodes for a camera at a point of the grid's voxel space, a voxel being voxelMetres along each axis;
    // the nodes split last time stay split within the hysteresis.
    void update(const VoxelGrid& grid, const glm::vec3& camera, const glm::vec3& voxelMetres, const LodSettings& settings);
    void clear();

    // In key order.
    const std::vector<Node>& getNodes() const { return _list; }
    bool isEmpty() const { return _list.empty(); }
    // The level the coarsest nodes are at: one node of it spans the grid's longest side.
    int getRootLevel() const { return _rootLevel; }
    // The node covering a point in finest units (clamped into the grid); a default key when the cut is empty.
    ChunkKey nodeCovering(const glm::ivec3& fine) const;
    // Metres from a camera to a node's box (the part within the grid).
    static float distanceTo(const glm::ivec3& voxelCount, const ChunkKey& key, const glm::vec3& camera, const glm::vec3& voxelMetres);

private:
    using KeySet = std::unordered_set<ChunkKey, ChunkKeyHash>;
    void visit(const ChunkKey& key, KeySet& nodes, KeySet& split) const;
    bool childIsInGrid(const ChunkKey& child) const;
    ChunkKey covering(const KeySet& nodes, const glm::ivec3& fine, int nearLevel) const;
    void balance(KeySet& nodes, KeySet& split) const;

    // what update() works with
    const VoxelGrid* _grid { nullptr };
    glm::vec3 _camera { 0.0f };
    glm::vec3 _voxelMetres { 1.0f };
    LodSettings _settings;

    glm::ivec3 _voxelCount { 0 };
    int _rootLevel { 0 };
    KeySet _nodes;
    KeySet _split;
    std::vector<Node> _list;
};

// The meshes a terrain draws: the grid's own chunks' near the camera, meshes of coarser nodes made here further
// away, kept up to date as the camera moves and the grid is edited. Lives on the editing thread; only the meshing
// runs on the pool. A new cut is shown only once every node of it has its mesh, so the ground never shows a crack
// or a hole while the camera moves (meshes of nodes edited meanwhile are swapped in one by one, as the grid's own
// chunks' are). The first cut, and the first after the grid is cleared, is shown node by node as the meshes come.
class LevelOfDetail {
public:
    struct Drawn {
        ChunkKey key;
        bool isGridChunk { false };          // draw the grid's own mesh of the chunk at the key (the terrain keeps those)
        const ChunkMesh* mesh { nullptr };   // otherwise this one: null while the node has none yet, or no surface
        uint64_t version { 0 };              // changes whenever the mesh does
    };

    explicit LevelOfDetail(QThreadPool* pool = nullptr);

    void setSettings(const LodSettings& settings);
    const LodSettings& getSettings() const { return _settings; }

    // Once a frame on the editing thread, after the grid's own chunks' meshes have been brought up to date: takes
    // the grid's changes, chooses the cut for the camera (a point of the grid's voxel space, voxels of voxelMetres),
    // collects the meshes the pool has finished, sends out the nodes whose meshes are missing or out of date
    // (nearest first, within the settings' time budget) and shows the cut wanted once it is ready.
    // gridChunkReady(key) says whether the terrain's own mesh of the grid's chunk at the key is there to draw (or
    // known to be empty). Returns whether what is drawn changed.
    bool update(VoxelGrid& grid, const glm::vec3& camera, const glm::vec3& voxelMetres,
                const std::function<bool(const ChunkKey&)>& gridChunkReady);

    // What to draw, a node of the cut shown each, in key order.
    const std::vector<Drawn>& getDrawn() const { return _drawn; }
    uint64_t getDrawnVersion() const { return _drawnVersion; }
    // The cut shown is the cut wanted, and no mesh is missing, out of date or being made.
    bool isSettled() const;

    // The node of the cut wanted that has had no mesh the longest while holding the cut shown back, if any: the
    // first such node in key order, with how long it has (LodSettings::stallNudgeMs and stallForceMs say what is
    // done about it). The events count goes up at each nudge and each force, for a caller that logs them.
    struct Stall {
        ChunkKey key;
        bool isGridChunk { false };
        bool active { false };     // a node is holding the cut back now
        float ms { 0.0f };         // for how long (the last value stays after the cut moved on)
        bool nudged { false };     // its mesh was asked for again
        bool forced { false };     // the cut wanted was shown without it
        uint64_t events { 0 };
    };
    const Stall& getStall() const { return _stall; }
    // The time this frame may spend sampling nodes, when less than the settings' sampleBudgetMs: the caller sets it
    // every frame from the frames it is given (a share of the display's interval), so a cut spreads over frames
    // instead of landing in one. 0, or more than the settings' value, means the settings' value.
    void setFrameBudgetMs(float ms) { _frameBudgetMs = ms; }
    float getFrameBudgetMs() const;   // the budget in force: the smaller of the two
    // The cut last shown, for a caller that logs it: which (a serial), how long after its choice it came (ms and
    // frames), and the meshes it brings to the drawing that the last drawn set did not have (new, or made again).
    struct CutShown {
        uint64_t serial { 0 };
        float msSinceChosen { 0.0f };
        uint64_t framesSinceChosen { 0 };
        size_t nodes { 0 };          // nodes of the cut
        size_t newMeshes { 0 };
        size_t newVertices { 0 };
        size_t newIndices { 0 };
    };
    const CutShown& getCutShown() const { return _cutShown; }
    // Whether a cut shown counts its nodes and new meshes (a map of the drawn set and a lookup per node): off, only
    // the serial, ms and frames are kept, so the count is paid only by a caller that reads it.
    void setCutShownCounting(bool counting) { _cutShownCounting = counting; }

    // The grid was replaced: every mesh here is out of date. What is drawn stays until the new cut's meshes are in.
    void gridReplaced();
    // Forgets everything, drawn or not: for a terrain without a grid.
    void clear();

    const LodCut& getCut() const { return _cut; }
    size_t getMeshCount() const { return _entries.size(); }
    size_t getInFlightCount() const { return _jobs.size(); }

private:
    struct EntryKey {
        ChunkKey key;
        uint64_t signature { 0 };
        bool operator<(const EntryKey& other) const {
            return key != other.key ? key < other.key : signature < other.signature;
        }
    };
    struct Entry {
        NodeContext context;
        ChunkMesh mesh;
        bool hasMesh { false };    // a mesh has come back (it may be empty: no surface)
        bool stale { false };      // the grid changed under it since it was sampled
        bool inFlight { false };
        uint64_t version { 0 };
        uint64_t lastUsed { 0 };   // the frame it was last wanted or shown
        glm::ivec3 readLow { 0 };  // the finest-unit box its samples read, to tell which changes reach it
        glm::ivec3 readHigh { 0 };
    };

    Entry& entryFor(const LodCut::Node& node);
    // Samples the node and sends it to the pool, or settles it at once when no surface can cross it.
    void send(const EntryKey& entryKey, Entry& entry, const VoxelGrid& grid);
    void rebuildDrawn();
    // The cut wanted is about to be shown in place of the cut shown: counts what it brings (CutShown).
    void noteCutShown(const std::vector<LodCut::Node>& wanted);
    void evict();
    // The wanted cut's node without a mesh (first in key order), while the cut shown is held back by it; returns
    // whether the cut may be shown anyway (no node is stalled, or the stall has reached the force time).
    bool trackStall(VoxelGrid& grid, const LodCut::Node* stalled);

    LodSettings _settings;
    LodCut _cut;
    std::vector<LodCut::Node> _shown;
    std::vector<Drawn> _drawn;
    uint64_t _drawnVersion { 0 };
    std::map<EntryKey, Entry> _entries;
    std::unordered_map<uint64_t, EntryKey> _jobs;   // tag of a job out on the pool: whose it is
    AsyncMesher _mesher;
    uint64_t _frame { 0 };
    uint64_t _nextTag { 0 };
    uint64_t _nextVersion { 0 };
    bool _cutWanted { true };       // choose the cut again whatever the camera did
    bool _requestsDirty { true };   // look for meshes to make again
    glm::vec3 _cutCamera { 0.0f };
    glm::vec3 _cutVoxelMetres { 0.0f };
    uint64_t _cutStructure { 0 };
    glm::ivec3 _cutVoxelCount { 0 };
    std::vector<std::pair<glm::ivec3, glm::ivec3>> _changes;
    Stall _stall;
    std::chrono::steady_clock::time_point _stallSince;
    float _frameBudgetMs { 0.0f };
    std::map<int, float> _nodeCostMs;   // what sampling a node of a level last took, smoothed: the estimate before a send
    std::chrono::steady_clock::time_point _cutChosenAt;   // when the cut wanted was last chosen
    uint64_t _cutChosenFrame { 0 };
    CutShown _cutShown;
    bool _cutShownCounting { false };
};

} // namespace terrain

#endif // hifi_terrain_LevelOfDetail_h
