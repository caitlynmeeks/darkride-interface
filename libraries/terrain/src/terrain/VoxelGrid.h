//
//  VoxelGrid.h
//  libraries/terrain/src/terrain
//
//  The voxels of one Terrain entity: a box of voxelCount grid voxels cut into chunks (Chunk.h). Grid voxel (x, y, z)
//  sits at that integer point of voxel space; the entity scales voxel space by its voxel size and places it in the
//  world. A chunk at level 0 covers CHUNK_SIZE grid voxels a side. Where finer ground is wanted a chunk is split
//  into eight at the next level, each CHUNK_SIZE voxels a side again over half the distance, so a level-k chunk's
//  voxels are 1/2^k of a grid voxel: under each level-0 chunk's place sits an octree of chunks, at most
//  MAX_CHUNK_LEVEL deep. Every chunk is named by its ChunkKey, its level and its coordinates at that level.
//  Neighbouring chunks differ by at most one level (the 2:1 rule, which refine() keeps and meshing across levels
//  relies on). The field between chunks of different levels is read by resampling on the fly: a coarser chunk's
//  voxels are interpolated at a finer chunk's points, and a finer chunk's are read at the coarser one's points,
//  which it has voxels at too.
//
//  The grid tracks which chunks' meshes are out of date and hands the mesher the samples one chunk needs
//  (ChunkSamples): its own voxels and one layer of its neighbours' all around, at its own level, so neighbouring
//  meshes of one level meet without a seam, and across each low face and low edge where the neighbours are at
//  another level, their voxels at that level (SampleBlock), so the meshes meet there too (SurfaceNets.h). Nothing
//  here locks: edit on one thread; sample there, or on any thread from a snapshot of the grid (VoxelGrid::snapshot,
//  a GridView: it shares the chunks, which are copied before they are written while shared, so it reads the grid
//  as it was when taken); mesh the samples anywhere (AsyncMesher does that on a thread pool).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_terrain_VoxelGrid_h
#define hifi_terrain_VoxelGrid_h

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

#include <glm/glm.hpp>

#include "Chunk.h"

namespace terrain {

// The finest level a chunk can be split to: its voxels are 1 / 2^MAX_CHUNK_LEVEL of a grid voxel.
const int MAX_CHUNK_LEVEL = 2;
// Finest-level voxels per grid voxel: the unit in which a point of any level is a whole number.
const int FINEST_PER_GRID_VOXEL = 1 << MAX_CHUNK_LEVEL;
// The coarsest level a node of the level of detail (LevelOfDetail.h) can be at. Nodes coarser than the grid's own
// chunks have negative levels: a level-k node covers 2^-k level-0 places a side, CHUNK_SIZE voxels of 2^-k grid
// voxels each.
const int MIN_LOD_LEVEL = -8;
const int CHUNK_SHIFT = 5;   // CHUNK_SIZE as a power of two
static_assert(CHUNK_SIZE == 1 << CHUNK_SHIFT, "chunks are a power of two voxels a side");

// The faces of the grid's box a terrain can close (VoxelGrid::setCaps): the ground stops at a closed face, capped
// just past its first or last voxels, instead of running out of the box open there, so a raise that reaches the top
// of the box has a top and the box's edges show walls rather than the inside of the ground. Bit 2 * axis is the low
// face on that axis, bit 2 * axis + 1 the high one.
const uint8_t CAP_LOW_X = 1;
const uint8_t CAP_HIGH_X = 2;
const uint8_t CAP_LOW_Y = 4;
const uint8_t CAP_HIGH_Y = 8;
const uint8_t CAP_LOW_Z = 16;
const uint8_t CAP_HIGH_Z = 32;
const uint8_t CAP_NONE = 0;
const uint8_t CAP_ALL = 63;

struct IVec3Less {
    bool operator()(const glm::ivec3& a, const glm::ivec3& b) const {
        if (a.x != b.x) {
            return a.x < b.x;
        }
        if (a.y != b.y) {
            return a.y < b.y;
        }
        return a.z < b.z;
    }
};

// Which chunk: its level, and its chunk coordinates at that level. A level-k chunk at coords covers the grid voxels
// from coords * (CHUNK_SIZE / 2^k) on, CHUNK_SIZE / 2^k a side, with CHUNK_SIZE voxels a side of its own; its voxel
// (x, y, z) sits at the level's point coords * CHUNK_SIZE + (x, y, z), which is the grid position divided by 2^k.
// The grid's own chunks are at levels 0 to MAX_CHUNK_LEVEL; a key at a negative level names a coarser node of the
// level of detail, whose voxels are 2^-k grid voxels.
struct ChunkKey {
    int level { 0 };
    glm::ivec3 coords { 0 };

    ChunkKey() = default;
    ChunkKey(int level, const glm::ivec3& coords) : level(level), coords(coords) {}
    ChunkKey(int level, int x, int y, int z) : level(level), coords(x, y, z) {}
    // A level-0 chunk's key is its chunk coordinates.
    ChunkKey(const glm::ivec3& coords) : coords(coords) {}

    int getScale() const { return 1 << level; }   // its voxels per grid voxel, at level 0 and finer
    // Grid voxels per voxel of its own, at any level: 1 / 2^level.
    float getVoxelSize() const { return std::ldexp(1.0f, -level); }
    int getSpan() const { return level >= 0 ? CHUNK_SIZE >> level : CHUNK_SIZE << -level; }   // grid voxels a side
    glm::ivec3 getOrigin() const { return coords * getSpan(); }   // the grid voxel at its low corner
    // The level-0 chunk it lies in; for a coarser node, the first of those it covers.
    glm::ivec3 getSlot() const;
    // The chunk's child i at the next level (bit 0 along x, 1 along y, 2 along z), and the node it is a child of
    // (a level-0 chunk's is a node of the level of detail, at level -1).
    ChunkKey getChild(int i) const { return ChunkKey(level + 1, coords * 2 + glm::ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1)); }
    ChunkKey getParent() const;
    // Whether any of its grid voxels lies in the box from low to high (inclusive).
    bool intersects(const glm::ivec3& low, const glm::ivec3& high) const;

    bool operator==(const ChunkKey& other) const { return level == other.level && coords == other.coords; }
    bool operator!=(const ChunkKey& other) const { return !(*this == other); }
    bool operator<(const ChunkKey& other) const {
        return level != other.level ? level < other.level : IVec3Less()(coords, other.coords);
    }
};
using ChunkKeySet = std::set<ChunkKey>;

struct ChunkKeyHash {
    size_t operator()(const ChunkKey& key) const {
        const size_t x = (size_t)(uint32_t)key.coords.x;
        const size_t y = (size_t)(uint32_t)key.coords.y;
        const size_t z = (size_t)(uint32_t)key.coords.z;
        return ((x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u)) * 31u + (size_t)(key.level - MIN_LOD_LEVEL);
    }
};

// One place in the octree under a level-0 chunk: a chunk, or eight nodes at the next level in its stead. The chunk
// is held through a shared pointer and never written while shared: a copy of the node (a GridView's) shares it and
// goes on reading it as it was, and mutableChunk() gives the grid a chunk of its own to write to first (copy on
// write, as Qt's implicitly shared classes do).
class ChunkNode {
public:
    ChunkNode();
    explicit ChunkNode(Chunk chunk);
    ChunkNode(const ChunkNode& other);
    ChunkNode(ChunkNode&&) = default;
    ChunkNode& operator=(const ChunkNode& other);
    ChunkNode& operator=(ChunkNode&&) = default;

    bool isSplit() const { return _children != nullptr; }
    // The chunk, while the node is not split.
    const Chunk& getChunk() const { return *_chunk; }
    // The chunk to write to, while the node is not split: the node's own once nothing else shares it.
    Chunk& mutableChunk();
    // Child i, as ChunkKey::getChild numbers them, while the node is split.
    const ChunkNode& getChild(int i) const { return _children[i]; }
    ChunkNode& getChild(int i) { return _children[i]; }

    // The eight children take the chunk's place.
    void split(std::array<Chunk, 8>&& children);
    // The chunk takes the children's place (or replaces the chunk, for a node that is not split).
    void merge(Chunk chunk);

private:
    std::shared_ptr<Chunk> _chunk;   // never null; written only through mutableChunk()
    std::unique_ptr<ChunkNode[]> _children;
};

const int SAMPLE_SIZE = CHUNK_SIZE + 2;

// Samples at a level next to a chunk's: the voxels of one of the chunk's low neighbours (across a low face or a
// low edge) at that neighbour's level, where it differs from the chunk's, over the neighbour's cells that touch
// the chunk. The mesher builds those cells from them as the neighbour does from its own samples, so the two
// meshes meet (SurfaceNets.h).
struct SampleBlock {
    int levelDelta { 0 };        // the block's level less the chunk's: -1 one coarser, +1 one finer
    glm::ivec3 origin { 0 };     // the block's first point, in its level's lattice, relative to the chunk's origin
    glm::ivec3 size { 0 };
    std::vector<Voxel> voxels;   // x fastest

    bool contains(const glm::ivec3& point) const {
        const glm::ivec3 local = point - origin;
        return glm::all(glm::greaterThanEqual(local, glm::ivec3(0))) && glm::all(glm::lessThan(local, size));
    }
    // The point must lie in the block.
    const Voxel& at(const glm::ivec3& point) const {
        const glm::ivec3 local = point - origin;
        return voxels[((size_t)local.z * size.y + local.y) * size.x + local.x];
    }
};

// The voxels one chunk's mesh is built from, at the chunk's own level: local coordinates run from -1 to CHUNK_SIZE
// inclusive. With them, the level of the neighbours across the chunk's low faces and low edges, and their voxels
// at that level where it is not the chunk's own.
class ChunkSamples {
public:
    ChunkSamples() : _voxels(SAMPLE_SIZE * SAMPLE_SIZE * SAMPLE_SIZE) {}

    static int indexOf(int x, int y, int z) { return ((z + 1) * SAMPLE_SIZE + (y + 1)) * SAMPLE_SIZE + (x + 1); }

    const Voxel& at(int x, int y, int z) const { return _voxels[indexOf(x, y, z)]; }
    Voxel& at(int x, int y, int z) { return _voxels[indexOf(x, y, z)]; }

    // True when every sample is ground or every sample is air: no surface crosses this chunk.
    bool isOneSided() const;
    // True when every sample is the same voxel.
    bool isUniform() const;

    const ChunkKey& getKey() const { return _key; }
    void setKey(const ChunkKey& key) { _key = key; }

    // The level of the neighbours across the chunk's low faces and low edges, less the chunk's own. A region is
    // named by bits: 1 for below x = 0, 2 for below y = 0, 4 for below z = 0, so region 1 lies across the low x
    // face and region 3 across the edge where the low x and y faces meet. 0 where the neighbours are at the
    // chunk's level, or the grid ends.
    int getNeighbourLevel(int region) const { return _neighbourLevels[region & 7]; }
    void setNeighbourLevel(int region, int levelDelta) { _neighbourLevels[region & 7] = levelDelta; }
    // The sample at a point of a neighbouring level's lattice (relative to the chunk's origin), from whichever
    // block holds it; null when none does.
    const Voxel* findAt(int levelDelta, const glm::ivec3& point) const;
    void addBlock(SampleBlock block) { _blocks.push_back(std::move(block)); }
    const std::vector<SampleBlock>& getBlocks() const { return _blocks; }
    // Every neighbour at the chunk's level again, and no blocks.
    void clearBlocks();

    // How many of the chunk's voxels along each axis lie before the grid's end, in its own voxels and in those of a
    // finer neighbour's: the mesher draws no surface from voxels at or past the end, so the ground stops where the
    // grid does even where a chunk reaches past it (a coarse node of the level of detail can reach a long way).
    // The whole chunk unless set.
    const glm::ivec3& getLimit() const { return _limit; }
    const glm::ivec3& getFinerLimit() const { return _finerLimit; }
    void setLimits(const glm::ivec3& own, const glm::ivec3& finer) { _limit = own; _finerLimit = finer; }

    // Whether the chunk lies against a closed low face of the grid's box on an axis (VoxelGrid::applyCaps sets it):
    // the mesher then draws the crossings into the chunk across that face, whose low voxel lies outside the grid
    // and so is no chunk's own. clearBlocks() clears these too.
    bool capsLow(int axis) const { return _capLow[axis]; }
    void setCapLow(int axis, bool caps) { _capLow[axis] = caps; }

private:
    ChunkKey _key;
    std::vector<Voxel> _voxels;
    int _neighbourLevels[8] { 0, 0, 0, 0, 0, 0, 0, 0 };
    std::vector<SampleBlock> _blocks;
    glm::ivec3 _limit { CHUNK_SIZE };
    glm::ivec3 _finerLimit { 2 * CHUNK_SIZE };
    bool _capLow[3] { false, false, false };
};

// What is read of a grid: its size, its closed faces and its chunks, with every reading of the field. A VoxelGrid
// is one of these with the writers added; a copy of one (VoxelGrid::snapshot) is a snapshot: it shares the chunks,
// which the grid copies before writing while they are shared (ChunkNode::mutableChunk), and has its own table of
// places, so it reads the grid as it was when it was taken, on any thread, while the grid goes on being edited on
// its own. Taking one costs a pointer per level-0 place (a few thousand); dropping one releases the chunks the grid
// has replaced since.
class GridView {
public:
    // The distance field between voxels at a point, in grid voxels, and the material there.
    struct FieldSample {
        float distance { SATURATED_DISTANCE };
        uint8_t material { 0 };
    };

    const glm::ivec3& getVoxelCount() const { return _voxelCount; }
    // Level-0 chunks along each side: the places the octrees of finer chunks hang under.
    const glm::ivec3& getChunkCount() const { return _chunkCount; }
    bool isEmpty() const { return _slots.empty(); }

    // The level-0 chunk of a grid voxel (floor division, so voxel -1 lies in chunk -1), and the voxel within it.
    static glm::ivec3 chunkOf(const glm::ivec3& voxel);
    static glm::ivec3 localOf(const glm::ivec3& voxel);
    bool containsVoxel(const glm::ivec3& voxel) const;
    // Whether a level-0 chunk, or a key at any level, lies within the grid (whether or not a chunk is at the key).
    bool containsChunk(const glm::ivec3& chunk) const;
    bool containsChunk(const ChunkKey& key) const;

    // The chunk covering a grid voxel (clamped into the grid), at whatever level it is.
    ChunkKey chunkCovering(const glm::ivec3& voxel) const;
    // The chunk covering one of a level's points (grid position point / 2^level, clamped into the grid).
    ChunkKey chunkCoveringPoint(int level, const glm::ivec3& point) const;
    // The chunk covering a point in finest units (grid position times FINEST_PER_GRID_VOXEL, clamped into the grid).
    ChunkKey chunkCoveringFine(const glm::ivec3& fine) const;
    // Every chunk covering any grid voxel from low to high (inclusive), at their own levels, in key order.
    std::vector<ChunkKey> chunksCovering(const glm::ivec3& low, const glm::ivec3& high) const;
    // Whether a chunk sits at exactly this key: not one coarser over it, nor finer ones under it.
    bool hasChunk(const ChunkKey& key) const;
    // Whether the grid has finer chunks under the key: a place split into eight.
    bool isSplit(const ChunkKey& key) const;
    const Chunk& getChunk(const ChunkKey& key) const;
    // The chunk at a key; null when none is there.
    const Chunk* findChunk(const ChunkKey& key) const;

    // The voxel at a grid voxel: the covering chunk's own voxel there (a finer chunk has one at every grid point).
    // Outside the grid the nearest voxel in it is returned, so the surface runs open to the grid's sides and the
    // mesher draws no walls or floor there.
    const Voxel& get(const glm::ivec3& voxel) const;
    // The voxel at one of a level's points (grid position point / 2^level), from whatever chunk covers it: that
    // chunk's own voxel when it is at the level or finer, interpolated between a coarser chunk's voxels otherwise,
    // the distance in the level's voxels either way. Points beyond the grid read its edge, as get() does. A level
    // coarser than the grid's (MIN_LOD_LEVEL to -1) reads the voxel at the grid voxel its point is, or at the
    // nearest in the grid.
    Voxel sampleAt(int level, const glm::ivec3& point) const;
    // The voxel of the chunk covering a point in finest units, which must lie within the grid's chunks (its sides
    // rounded up to whole level-0 chunks), the distance in the voxels of a level that chunk is at or finer than,
    // the point being one of that level's: the chunk's own voxel there, rescaled.
    Voxel voxelAtFine(const glm::ivec3& fine, int level) const;
    // Whether no surface crosses the grid voxels from low to high (inclusive): every chunk covering them is of one
    // value, and all of those are ground or all air.
    bool isOneSided(const glm::ivec3& low, const glm::ivec3& high) const;
    // How many voxels of a node at a level, its low corner at the grid voxel origin, lie before the grid's end
    // along each axis, at most `most` (ChunkSamples::setLimits).
    glm::ivec3 limitOf(int level, const glm::ivec3& origin, int most) const;
    // The field at any grid position: the distance interpolated between the covering chunk's voxels, in grid
    // voxels, and the material of the voxel nearest.
    FieldSample fieldAt(const glm::vec3& position) const;

    // The chunk's voxels and one layer of its neighbours' around them, at the chunk's level; and, for the
    // mesher, the neighbours' levels across the low faces and edges with their voxels at those levels where they
    // differ (ChunkSamples' blocks), which a resampling of the chunk alone has no use for, and the box's closed
    // faces applied (applyCaps).
    void sampleChunk(const ChunkKey& key, ChunkSamples& samples, bool forMeshing = true) const;

    // Which faces of the box are closed (the CAP_ bits, VoxelGrid::setCaps): the ground is capped at a closed face
    // as if the space beyond it were air, and runs out of the box open at an open one.
    uint8_t getCaps() const { return _caps; }
    // Whether the grid voxels from low to high (inclusive) touch a closed face: a node there may need a cap even
    // where the voxels are all ground.
    bool capsReach(const glm::ivec3& low, const glm::ivec3& high) const;
    // Closes meshing samples at the box's closed faces: a sample's distance becomes at least its distance outside
    // the face's plane, half a voxel of the finest level past the grid's first and last voxels (within every
    // level's layer of samples), so every level meets the plane at the same place and beyond it the samples are
    // air; and marks the samples' closed low faces for the mesher (capsLow). Nothing to do without closed faces.
    void applyCaps(ChunkSamples& samples) const;

    int countChunks() const;
    int countDenseChunks() const;
    // The level of the finest chunk here: 0 while none is split.
    int getFinestLevel() const;

    // f(const ChunkKey& key, const Chunk& chunk) for every chunk, in storage order: level-0 places in x, y, z
    // order, the chunks under a split one in child order.
    template <typename F> void forEachChunk(F&& f) const {
        int index = 0;
        for (int z = 0; z < _chunkCount.z; ++z) {
            for (int y = 0; y < _chunkCount.y; ++y) {
                for (int x = 0; x < _chunkCount.x; ++x) {
                    forEachLeaf(_slots[index++], ChunkKey(0, glm::ivec3(x, y, z)), f);
                }
            }
        }
    }

protected:
    struct Found {
        const ChunkNode* node;
        ChunkKey key;
    };

    template <typename F> static void forEachLeaf(const ChunkNode& node, const ChunkKey& key, F&& f) {
        if (!node.isSplit()) {
            f(key, node.getChunk());
            return;
        }
        for (int i = 0; i < 8; ++i) {
            forEachLeaf(node.getChild(i), key.getChild(i), f);
        }
    }

    int chunkIndexOf(const glm::ivec3& chunk) const {
        return (chunk.z * _chunkCount.y + chunk.y) * _chunkCount.x + chunk.x;
    }
    // The chunk covering a point in finest units (grid position times 2^MAX_CHUNK_LEVEL), which must lie in the grid.
    Found locate(const glm::ivec3& fine) const;
    // The node at a key, split or not; null when a chunk coarser than the key covers it, or the key is outside.
    const ChunkNode* nodeAt(const ChunkKey& key) const;
    Voxel interpolateAt(const Found& found, const glm::ivec3& fine, int level) const;
    void collectLeaves(const ChunkNode& node, const ChunkKey& key, const glm::ivec3& low, const glm::ivec3& high,
                       std::vector<ChunkKey>& keys) const;
    bool leavesAreOneSided(const ChunkNode& node, const ChunkKey& key, const glm::ivec3& low, const glm::ivec3& high,
                           int& side) const;

    glm::ivec3 _voxelCount { 0 };
    glm::ivec3 _chunkCount { 0 };
    std::vector<ChunkNode> _slots;
    uint8_t _caps { CAP_NONE };
};

class VoxelGrid : public GridView {
public:
    VoxelGrid() = default;
    VoxelGrid(const glm::ivec3& voxelCount, const Voxel& fill = Voxel());

    // Every chunk at level 0 again, all of one value.
    void reset(const glm::ivec3& voxelCount, const Voxel& fill = Voxel());

    // The finest level refine() splits chunks to here; at most MAX_CHUNK_LEVEL.
    int getMaxLevel() const { return _maxLevel; }
    void setMaxLevel(int level);

    // The grid as it is now, to read on any thread while this one goes on editing (the class comment of GridView).
    // Take one per batch of readers, not one per reader.
    GridView snapshot() const { return *this; }

    using GridView::findChunk;
    // The chunk at a key, to edit in place; null when none is there. voxelsChanged() afterwards says what to re-mesh.
    Chunk* findChunk(const ChunkKey& key);
    // The chunk's voxels from localLow to localHigh (inclusive) changed: the chunk and every chunk whose samples
    // read them are dirty now.
    void voxelsChanged(const ChunkKey& key, const glm::ivec3& localLow, const glm::ivec3& localHigh);

    // Sets the covering chunk's voxel at a grid voxel. Voxels outside the grid are ignored. Returns true when the
    // voxel changed.
    bool set(const glm::ivec3& voxel, const Voxel& value);

    // The chunk at the key becomes this one and is dirty, with every chunk that samples it. Finer chunks under
    // the key go; a coarser chunk over it is split first, its other parts resampled from it as splitChunk() does,
    // though without the 2:1 rule, so a chunk can be put anywhere.
    void setChunk(const ChunkKey& key, Chunk value);
    // As setChunk(), except that where a coarser chunk is split to make room its other parts are filled with its
    // first voxel rather than resampled: for a set of chunks that covers whole regions (a file, an undo step),
    // which replaces those parts anyway.
    void placeChunk(const ChunkKey& key, Chunk value);
    // The chunk at the key becomes eight at the next level, the same ground resampled finer (the children's new
    // voxels interpolated between the chunk's and its neighbours'); the chunks around are split as the 2:1 rule
    // needs. False when no chunk is at the key or it is at MAX_CHUNK_LEVEL already.
    bool splitChunk(const ChunkKey& key);
    // Every chunk covering any grid voxel from low to high (inclusive) is at the level (at most getMaxLevel()) or
    // finer afterwards, and the 2:1 rule holds around them.
    void refine(const glm::ivec3& low, const glm::ivec3& high, int level);
    // Counts up whenever chunks are split, merged or replaced: a holder of meshes by key knows to look for keys
    // that are gone.
    uint64_t getStructureVersion() const { return _structureVersion; }

    // The chunks whose meshes are out of date: a changed voxel dirties its chunk and every neighbour that samples
    // it. The mesher's caller clears the set once it has re-meshed them.
    const ChunkKeySet& getDirtyChunks() const { return _dirtyChunks; }
    void clearDirtyChunks() { _dirtyChunks.clear(); }
    void clearDirtyChunk(const ChunkKey& key) { _dirtyChunks.erase(key); }
    void markChunkDirty(const ChunkKey& key);
    void markAllChunksDirty();

    // The holders of what is built from the voxels besides the chunks' own meshes, each with a change log of its own:
    // the level of detail's coarser meshes (LevelOfDetail.h) and the top surface's heights (TopSurface.h).
    enum class ChangeLog { LevelOfDetail, TopSurface };
    static const int CHANGE_LOG_COUNT = 2;
    // Where voxels have changed since the holder's last call: boxes in finest units (inclusive), which the holder
    // takes each frame. False when the boxes would not say: the whole grid changed, or more than MAX_CHANGE_BOXES of
    // them piled up with nobody taking them; everything is to be taken as changed then.
    bool takeChanges(std::vector<std::pair<glm::ivec3, glm::ivec3>>& boxes, ChangeLog log = ChangeLog::LevelOfDetail);
    static const size_t MAX_CHANGE_BOXES = 4096;

    // Which faces of the box are closed (the CAP_ bits; GridView::getCaps). A change marks every chunk dirty. Kept
    // by reset(); the terrain file carries them (version 6).
    void setCaps(uint8_t caps);

    // Ground below a height field: height(x, z) in grid voxels is where the surface lies in that column. Every
    // chunk is at level 0 again afterwards.
    void fillHeightField(const std::function<float(int x, int z)>& height, uint8_t material = 0);
    void fillBelow(float height, uint8_t material = 0);

    void compactChunks();

private:
    template <typename F> static void forEachLeaf(ChunkNode& node, const ChunkKey& key, F&& f) {
        if (!node.isSplit()) {
            f(key, node);
            return;
        }
        for (int i = 0; i < 8; ++i) {
            forEachLeaf(node.getChild(i), key.getChild(i), f);
        }
    }

    using GridView::nodeAt;
    ChunkNode* nodeAt(const ChunkKey& key);
    std::array<Chunk, 8> resampleChildren(const ChunkKey& key) const;
    void splitNode(ChunkNode& node, const ChunkKey& key, bool resample);
    void refineNode(const ChunkKey& key, const glm::ivec3& low, const glm::ivec3& high, int level);
    void installChunk(const ChunkKey& key, Chunk value, bool resample);
    void noteChange(const glm::ivec3& fineLow, const glm::ivec3& fineHigh);

    int _maxLevel { MAX_CHUNK_LEVEL };
    ChunkKeySet _dirtyChunks;
    uint64_t _structureVersion { 0 };
    struct Changes {
        std::vector<std::pair<glm::ivec3, glm::ivec3>> boxes;
        bool everything { true };
    };
    std::array<Changes, CHANGE_LOG_COUNT> _changes;
};

} // namespace terrain

#endif // hifi_terrain_VoxelGrid_h
