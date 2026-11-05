//
//  LevelOfDetail.cpp
//  libraries/terrain/src/terrain
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "LevelOfDetail.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <set>

using namespace terrain;

namespace {

const int FINEST = FINEST_PER_GRID_VOXEL;

// Finest units per voxel of a level, and per side of a node at it.
int unitOf(int level) {
    return level >= 0 ? FINEST >> level : FINEST << -level;
}

int spanFineOf(int level) {
    return CHUNK_SIZE * unitOf(level);
}

int ceilDivide(int value, int divisor) {
    return value >= 0 ? (value + divisor - 1) / divisor : -(-value / divisor);
}

glm::ivec3 clampFine(const glm::ivec3& voxelCount, const glm::ivec3& fine) {
    return glm::clamp(fine, glm::ivec3(0), (voxelCount - 1) * FINEST);
}

float trilinear(const float d[8], const glm::vec3& f) {
    const float d00 = glm::mix(d[0], d[1], f.x);
    const float d10 = glm::mix(d[2], d[3], f.x);
    const float d01 = glm::mix(d[4], d[5], f.x);
    const float d11 = glm::mix(d[6], d[7], f.x);
    return glm::mix(glm::mix(d00, d10, f.y), glm::mix(d01, d11, f.y), f.z);
}

// The field as the nodes of a cut around one node read it: VoxelGrid::sampleAt() and interpolateAt() step for
// step, with the level covering each point taken from the node's context (the places around it) rather than from
// the grid's chunks. A node's own voxels are the grid's at its points, whatever chunk holds them.
class NodeSampler {
public:
    NodeSampler(const GridView& grid, const ChunkKey& key, const NodeContext& context) :
        _grid(grid),
        _key(key),
        _context(context),
        _voxelCount(grid.getVoxelCount()),
        _extentFine(grid.getChunkCount() * CHUNK_SIZE * FINEST),
        _originFine(key.getOrigin() * FINEST),
        _spanFine(spanFineOf(key.level)) {}

    // The field at one of a level's points.
    Voxel fieldAt(int level, const glm::ivec3& point) const {
        glm::ivec3 fine;
        if (level >= 0) {
            const glm::ivec3 clamped = glm::clamp(point, glm::ivec3(0), (_voxelCount - 1) << level);
            fine = clamped << (MAX_CHUNK_LEVEL - level);
        } else {
            fine = clampFine(_voxelCount, point * unitOf(level));
        }
        const int covering = coveringLevel(fine);
        if (covering >= level) {
            return _grid.voxelAtFine(fine, level);
        }
        return interpolate(fine, level, covering);
    }

    // A node's own voxel at a point of its level (finest units): the grid's voxel there, from whichever chunk
    // covers it; beyond the grid's chunks (a coarse node reaching past them) the one nearest within the grid.
    Voxel storedAt(const glm::ivec3& fine, int level) const {
        if (glm::all(glm::lessThan(fine, _extentFine))) {
            return _grid.voxelAtFine(fine, level);
        }
        return _grid.voxelAtFine(clampFine(_voxelCount, fine), level);
    }

private:
    int coveringLevel(const glm::ivec3& fine) const {
        int sides[3];
        for (int axis = 0; axis < 3; ++axis) {
            sides[axis] = fine[axis] < _originFine[axis] ? -1 : (fine[axis] >= _originFine[axis] + _spanFine ? 1 : 0);
        }
        return _key.level + _context.get(sides[0], sides[1], sides[2]);
    }

    // Between the points of a coarser node: trilinear, the material of the nearest, as VoxelGrid::interpolateAt().
    Voxel interpolate(const glm::ivec3& fine, int level, int coarse) const {
        const int unit = unitOf(coarse);
        const glm::ivec3 corner = fine / unit;
        const glm::ivec3 remainder = fine - corner * unit;
        const glm::vec3 f = glm::vec3(remainder) / (float)unit;
        const glm::ivec3 next = corner + glm::ivec3(glm::greaterThan(remainder, glm::ivec3(0)));
        const glm::ivec3 origin = (corner / CHUNK_SIZE) * CHUNK_SIZE;   // the coarse node's first point
        auto voxelAt = [&](const glm::ivec3& p) -> Voxel {
            const glm::ivec3 local = p - origin;
            if (glm::all(glm::greaterThanEqual(local, glm::ivec3(0))) && glm::all(glm::lessThan(local, glm::ivec3(CHUNK_SIZE)))) {
                return storedAt(p * unit, coarse);
            }
            return fieldAt(coarse, p);
        };
        float d[8];
        for (int k = 0; k < 8; ++k) {
            const glm::ivec3 p(k & 1 ? next.x : corner.x, k & 2 ? next.y : corner.y, k & 4 ? next.z : corner.z);
            d[k] = voxelAt(p).getDistance();
        }
        const float distance = trilinear(d, f);
        const glm::ivec3 nearest = (fine + unit / 2) / unit;
        return Voxel(rescaleDistance(distance, std::ldexp(1.0f, level - coarse)), voxelAt(nearest).material);
    }

    const GridView& _grid;
    const ChunkKey _key;
    const NodeContext& _context;
    const glm::ivec3 _voxelCount;
    const glm::ivec3 _extentFine;
    const glm::ivec3 _originFine;
    const int _spanFine;
};

// A node's own voxels when it is not one of the grid's chunks: the grid's voxels at its points, read from each chunk
// under it in strides (a coarse node takes every 2nd, 4th... voxel of the chunks it covers).
void sampleOwnVoxels(const GridView& grid, const ChunkKey& key, const NodeSampler& sampler, ChunkSamples& samples) {
    const int unit = unitOf(key.level);
    const glm::ivec3 originFine = key.getOrigin() * FINEST;
    const glm::ivec3 extentFine = grid.getChunkCount() * CHUNK_SIZE * FINEST;
    // its points within the grid's chunks along each axis; the others read the voxel nearest within the grid
    glm::ivec3 inside;
    for (int axis = 0; axis < 3; ++axis) {
        inside[axis] = glm::clamp(ceilDivide(extentFine[axis] - originFine[axis], unit), 0, CHUNK_SIZE);
    }
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                if (x >= inside.x || y >= inside.y || z >= inside.z) {
                    samples.at(x, y, z) = sampler.storedAt(originFine + glm::ivec3(x, y, z) * unit, key.level);
                }
            }
        }
    }
    if (glm::any(glm::equal(inside, glm::ivec3(0)))) {
        return;
    }
    const glm::ivec3 low = key.getOrigin();
    const glm::ivec3 high = (originFine + (inside - 1) * unit) / FINEST;
    const glm::ivec3 origin = key.coords * CHUNK_SIZE;
    for (const ChunkKey& leaf : grid.chunksCovering(low, high)) {
        const Chunk* chunk = grid.findChunk(leaf);
        if (!chunk) {
            continue;
        }
        const glm::ivec3 leafLow = leaf.getOrigin() * FINEST;
        const int leafSpan = spanFineOf(leaf.level);
        glm::ivec3 from, to;   // the node's points within the leaf, to exclusive
        for (int axis = 0; axis < 3; ++axis) {
            from[axis] = std::max(0, ceilDivide(leafLow[axis] - originFine[axis], unit));
            to[axis] = std::min(inside[axis], ceilDivide(leafLow[axis] + leafSpan - originFine[axis], unit));
        }
        if (glm::any(glm::greaterThanEqual(from, to))) {
            continue;
        }
        if (leaf.level < key.level) {
            // a chunk coarser than the node (not in a cut, where nodes are never finer than the grid): between its voxels
            for (int z = from.z; z < to.z; ++z) {
                for (int y = from.y; y < to.y; ++y) {
                    for (int x = from.x; x < to.x; ++x) {
                        samples.at(x, y, z) = sampler.fieldAt(key.level, origin + glm::ivec3(x, y, z));
                    }
                }
            }
            continue;
        }
        const float rescale = std::ldexp(1.0f, key.level - leaf.level);
        auto atLevel = [&](const Voxel& voxel) {
            return leaf.level == key.level ? voxel : Voxel(rescaleDistance(voxel.getDistance(), rescale), voxel.material);
        };
        if (chunk->isUniform()) {
            const Voxel value = atLevel(chunk->getUniformValue());
            for (int z = from.z; z < to.z; ++z) {
                for (int y = from.y; y < to.y; ++y) {
                    for (int x = from.x; x < to.x; ++x) {
                        samples.at(x, y, z) = value;
                    }
                }
            }
            continue;
        }
        const int leafUnit = unitOf(leaf.level);
        for (int z = from.z; z < to.z; ++z) {
            const int lz = (originFine.z + z * unit - leafLow.z) / leafUnit;
            for (int y = from.y; y < to.y; ++y) {
                const int ly = (originFine.y + y * unit - leafLow.y) / leafUnit;
                for (int x = from.x; x < to.x; ++x) {
                    const int lx = (originFine.x + x * unit - leafLow.x) / leafUnit;
                    samples.at(x, y, z) = atLevel(chunk->get(lx, ly, lz));
                }
            }
        }
    }
}

} // namespace

// --- NodeContext ------------------------------------------------------------------------------------------------

uint64_t NodeContext::getSignature() const {
    uint64_t signature = 0;
    for (int i = 0; i < 27; ++i) {
        signature |= (uint64_t)(_deltas[i] + 1) << (2 * i);
    }
    return signature;
}

glm::ivec3 terrain::contextProbe(const GridView& grid, const ChunkKey& key, int index) {
    const glm::ivec3 sides = NodeContext::sidesOf(index);
    const glm::ivec3 originFine = key.getOrigin() * FINEST;
    const int spanFine = spanFineOf(key.level);
    glm::ivec3 probe;
    for (int axis = 0; axis < 3; ++axis) {
        probe[axis] = sides[axis] < 0 ? originFine[axis] - 1 : (sides[axis] > 0 ? originFine[axis] + spanFine : originFine[axis] + spanFine / 2);
    }
    return clampFine(grid.getVoxelCount(), probe);
}

NodeContext terrain::gridContext(const GridView& grid, const ChunkKey& key) {
    NodeContext context;
    for (int i = 0; i < 27; ++i) {
        if (i != NodeContext::CENTRE) {
            context.set(i, glm::clamp(grid.chunkCoveringFine(contextProbe(grid, key, i)).level - key.level, -1, 1));
        }
    }
    return context;
}

void terrain::sampleNode(const GridView& grid, const ChunkKey& key, const NodeContext& context, ChunkSamples& samples) {
    samples.setKey(key);
    samples.clearBlocks();
    samples.setLimits(grid.limitOf(key.level, key.getOrigin(), CHUNK_SIZE), grid.limitOf(key.level + 1, key.getOrigin(), 2 * CHUNK_SIZE));
    if (grid.isEmpty()) {
        return;
    }
    const NodeSampler sampler(grid, key, context);
    const glm::ivec3 origin = key.coords * CHUNK_SIZE;

    // its own voxels: the chunk's when the node is one of the grid's chunks, the grid's at its points otherwise
    const Chunk* chunk = key.level >= 0 ? grid.findChunk(key) : nullptr;
    if (chunk) {
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                for (int x = 0; x < CHUNK_SIZE; ++x) {
                    samples.at(x, y, z) = chunk->get(x, y, z);
                }
            }
        }
    } else {
        sampleOwnVoxels(grid, key, sampler, samples);
    }
    // the layer around them, as the nodes around read it
    for (int z = -1; z <= CHUNK_SIZE; ++z) {
        for (int y = -1; y <= CHUNK_SIZE; ++y) {
            for (int x = -1; x <= CHUNK_SIZE; ++x) {
                const bool own = x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE;
                if (!own) {
                    samples.at(x, y, z) = sampler.fieldAt(key.level, origin + glm::ivec3(x, y, z));
                }
            }
        }
    }
    // across the low faces and edges where the nodes are at another level, theirs at that level (as
    // VoxelGrid::sampleChunk() does for the grid's chunks; the corner too along a closed low face)
    bool anyCapLow = false;
    for (int axis = 0; axis < 3; ++axis) {
        anyCapLow = anyCapLow || ((grid.getCaps() & (1 << (2 * axis))) != 0 && key.getOrigin()[axis] <= 0);
    }
    for (int region = 1; region < (anyCapLow ? 8 : 7); ++region) {
        const glm::ivec3 below(region & 1 ? -1 : 0, region & 2 ? -1 : 0, region & 4 ? -1 : 0);
        const int delta = context.get(below.x, below.y, below.z);
        samples.setNeighbourLevel(region, delta);
        if (delta == 0) {
            continue;
        }
        SampleBlock block;
        block.levelDelta = delta;
        const int side = delta > 0 ? CHUNK_SIZE * 2 : CHUNK_SIZE / 2;   // the node's side in the block's voxels
        block.size = glm::ivec3(region & 1 ? 2 : side + 1, region & 2 ? 2 : side + 1, region & 4 ? 2 : side + 1);
        // along a closed low face of the box the cap's edges start a voxel further out, and their polygons reach the
        // neighbours' cells a point further out along that axis (as VoxelGrid::sampleChunk)
        glm::ivec3 reach = below;
        for (int axis = 0; axis < 3; ++axis) {
            if (!(grid.getCaps() & (1 << (2 * axis))) || key.getOrigin()[axis] > 0) {
                continue;
            }
            if (below[axis] < 0) {
                if (delta > 0) {
                    reach[axis] = -2;
                    block.size[axis] = 3;
                }
            } else {
                const int extra = delta > 0 ? 2 : 1;
                reach[axis] = -extra;
                block.size[axis] += extra;
            }
        }
        block.origin = reach;
        block.voxels.resize((size_t)block.size.x * block.size.y * block.size.z);
        const glm::ivec3 first = key.coords * side + reach;
        size_t index = 0;
        for (int z = 0; z < block.size.z; ++z) {
            for (int y = 0; y < block.size.y; ++y) {
                for (int x = 0; x < block.size.x; ++x) {
                    block.voxels[index++] = sampler.fieldAt(key.level + delta, first + glm::ivec3(x, y, z));
                }
            }
        }
        samples.addBlock(std::move(block));
    }
    grid.applyCaps(samples);
}

// --- LodCut -----------------------------------------------------------------------------------------------------

float LodCut::distanceTo(const glm::ivec3& voxelCount, const ChunkKey& key, const glm::vec3& camera, const glm::vec3& voxelMetres) {
    const glm::vec3 low(key.getOrigin());
    const glm::vec3 high = glm::min(low + (float)key.getSpan(), glm::vec3(voxelCount));
    const glm::vec3 outside = glm::max(glm::max(low - camera, camera - high), glm::vec3(0.0f));
    return glm::length(outside * voxelMetres);
}

void LodCut::clear() {
    _nodes.clear();
    _split.clear();
    _list.clear();
    _rootLevel = 0;
    _voxelCount = glm::ivec3(0);
}

bool LodCut::childIsInGrid(const ChunkKey& child) const {
    return glm::all(glm::lessThan(child.getOrigin(), _voxelCount));
}

void LodCut::visit(const ChunkKey& key, KeySet& nodes, KeySet& split) const {
    if (key.level < 0 || _grid->isSplit(key)) {
        bool finer = _settings.detail <= 0.0f;   // without level of detail, as fine as the grid
        if (!finer) {
            const float size = (float)key.getSpan() * (_voxelMetres.x + _voxelMetres.y + _voxelMetres.z) / 3.0f;
            const float reach = _settings.detail * size * (_split.count(key) ? 1.0f + _settings.hysteresis : 1.0f);
            finer = distanceTo(_voxelCount, key, _camera, _voxelMetres) < reach;
        }
        if (finer) {
            split.insert(key);
            for (int i = 0; i < 8; ++i) {
                const ChunkKey child = key.getChild(i);
                if (childIsInGrid(child)) {
                    visit(child, nodes, split);
                }
            }
            return;
        }
    }
    nodes.insert(key);
}

ChunkKey LodCut::covering(const KeySet& nodes, const glm::ivec3& fine, int nearLevel) const {
    // the levels next to the asking node's first: in a cut that keeps the 2:1 rule, one of them holds the point
    for (int delta : { 0, -1, 1 }) {
        const int level = nearLevel + delta;
        if (level >= _rootLevel && level <= MAX_CHUNK_LEVEL) {
            const ChunkKey key(level, fine / spanFineOf(level));
            if (nodes.count(key)) {
                return key;
            }
        }
    }
    for (int level = _rootLevel; level <= MAX_CHUNK_LEVEL; ++level) {
        if (std::abs(level - nearLevel) > 1) {
            const ChunkKey key(level, fine / spanFineOf(level));
            if (nodes.count(key)) {
                return key;
            }
        }
    }
    assert(false);
    return ChunkKey(_rootLevel, fine / spanFineOf(_rootLevel));
}

void LodCut::balance(KeySet& nodes, KeySet& split) const {
    // A node two levels coarser than a neighbour is drawn as its children instead, until none is. Its children are
    // there to draw: a coarse node always has them, and a chunk of the grid two levels coarser than a node beside it
    // is split in the grid, whose own chunks keep the 2:1 rule.
    std::vector<ChunkKey> work(nodes.begin(), nodes.end());
    while (!work.empty()) {
        const ChunkKey key = work.back();
        work.pop_back();
        if (key.level <= _rootLevel + 1 || !nodes.count(key)) {
            continue;
        }
        for (int i = 0; i < 27; ++i) {
            if (i == NodeContext::CENTRE) {
                continue;
            }
            const ChunkKey neighbour = covering(nodes, contextProbe(*_grid, key, i), key.level);
            if (neighbour.level >= key.level - 1) {
                continue;
            }
            if (neighbour.level >= 0 && !_grid->isSplit(neighbour)) {
                assert(false);
                continue;
            }
            nodes.erase(neighbour);
            split.insert(neighbour);
            for (int c = 0; c < 8; ++c) {
                const ChunkKey child = neighbour.getChild(c);
                if (childIsInGrid(child)) {
                    nodes.insert(child);
                    work.push_back(child);
                }
            }
            work.push_back(key);   // the child beside it may still be too coarse
            break;
        }
    }
}

void LodCut::update(const VoxelGrid& grid, const glm::vec3& camera, const glm::vec3& voxelMetres, const LodSettings& settings) {
    if (grid.isEmpty()) {
        clear();
        return;
    }
    _grid = &grid;
    _camera = camera;
    _voxelMetres = voxelMetres;
    _settings = settings;
    _voxelCount = grid.getVoxelCount();

    // the roots: the coarsest level at which one node spans the grid's longest side
    int root = 0;
    if (settings.detail > 0.0f) {
        const int longest = std::max(std::max(_voxelCount.x, _voxelCount.y), _voxelCount.z);
        while (root > MIN_LOD_LEVEL && (CHUNK_SIZE << -root) < longest) {
            --root;
        }
    }
    _rootLevel = root;
    KeySet nodes;
    KeySet split;
    const int span = ChunkKey(root, glm::ivec3(0)).getSpan();
    const glm::ivec3 roots = (_voxelCount + span - 1) / span;
    for (int z = 0; z < roots.z; ++z) {
        for (int y = 0; y < roots.y; ++y) {
            for (int x = 0; x < roots.x; ++x) {
                visit(ChunkKey(root, x, y, z), nodes, split);
            }
        }
    }
    balance(nodes, split);
    _nodes = std::move(nodes);
    _split = std::move(split);

    _list.clear();
    _list.reserve(_nodes.size());
    for (const ChunkKey& key : _nodes) {
        Node node;
        node.key = key;
        for (int i = 0; i < 27; ++i) {
            if (i != NodeContext::CENTRE) {
                const int level = covering(_nodes, contextProbe(grid, key, i), key.level).level;
                assert(std::abs(level - key.level) <= 1);
                node.context.set(i, glm::clamp(level - key.level, -1, 1));
            }
        }
        node.isGridChunk = key.level >= 0 && grid.hasChunk(key) && node.context == gridContext(grid, key);
        node.distance = distanceTo(_voxelCount, key, camera, voxelMetres);
        _list.push_back(node);
    }
    std::sort(_list.begin(), _list.end(), [](const Node& a, const Node& b) { return a.key < b.key; });
    _grid = nullptr;
}

ChunkKey LodCut::nodeCovering(const glm::ivec3& fine) const {
    if (_nodes.empty()) {
        return ChunkKey();
    }
    return covering(_nodes, clampFine(_voxelCount, fine), _rootLevel);
}

// --- LevelOfDetail ----------------------------------------------------------------------------------------------

LevelOfDetail::LevelOfDetail(QThreadPool* pool) : _mesher(pool) {
}

void LevelOfDetail::setSettings(const LodSettings& settings) {
    if (settings != _settings) {
        _settings = settings;
        _cutWanted = true;
    }
}

LevelOfDetail::Entry& LevelOfDetail::entryFor(const LodCut::Node& node) {
    const EntryKey entryKey { node.key, node.context.getSignature() };
    auto found = _entries.find(entryKey);
    if (found != _entries.end()) {
        return found->second;
    }
    Entry& entry = _entries[entryKey];
    entry.context = node.context;
    // the samples reach a voxel past the node all round, and the blocks and the coarser nodes' points a little
    // further: three voxels covers them
    const int unit = unitOf(node.key.level);
    const glm::ivec3 originFine = node.key.getOrigin() * FINEST;
    entry.readLow = originFine - 3 * unit;
    entry.readHigh = originFine + (CHUNK_SIZE + 3) * unit;
    return entry;
}

void LevelOfDetail::send(const EntryKey& entryKey, Entry& entry, const VoxelGrid& grid, std::shared_ptr<const GridView>& view) {
    entry.stale = false;
    if (!view) {
        view = std::make_shared<const GridView>(grid.snapshot());
    }
    // the grid voxels its samples read, the nearest within the grid where they reach past it: when those are all
    // one value on one side, no surface crosses the node (one against a closed face of the box may need a cap
    // there even with its voxels all ground), and the job says so without sampling
    const glm::ivec3 low = glm::clamp(glm::max(entry.readLow, glm::ivec3(0)) / FINEST, glm::ivec3(0), grid.getVoxelCount() - 1);
    const glm::ivec3 high = glm::max(glm::min(glm::max(entry.readHigh, glm::ivec3(0)) / FINEST, grid.getChunkCount() * CHUNK_SIZE - 1), low);
    const uint64_t tag = ++_nextTag;
    _jobs[tag] = entryKey;
    entry.inFlight = true;
    const ChunkKey key = entryKey.key;
    const NodeContext context = entry.context;
    _mesher.submitSampler(key, [view, key, context, low, high](ChunkSamples& samples) {
        if (!view->capsReach(low, high) && view->isOneSided(low, high)) {
            return false;
        }
        sampleNode(*view, key, context, samples);
        return true;
    }, tag, 1);   // ahead of the grid's chunks: these are what is drawn
}

bool LevelOfDetail::update(VoxelGrid& grid, const glm::vec3& camera, const glm::vec3& voxelMetres,
                           const std::function<bool(const ChunkKey&)>& gridChunkReady) {
    ++_frame;
    if (grid.isEmpty()) {
        if (_shown.empty() && _entries.empty() && _cut.isEmpty()) {
            return false;
        }
        clear();
        return true;
    }
    bool drawnChanged = false;

    // the meshes back from the pool (an empty one for a node no surface crosses: it is settled)
    for (AsyncMesher::Result& result : _mesher.collect()) {
        auto job = _jobs.find(result.tag);
        if (job == _jobs.end()) {
            continue;
        }
        const EntryKey entryKey = job->second;
        _jobs.erase(job);
        auto found = _entries.find(entryKey);
        if (found == _entries.end()) {
            continue;
        }
        Entry& entry = found->second;
        entry.inFlight = false;
        entry.mesh = std::move(result.mesh);
        entry.hasMesh = true;
        entry.version = ++_nextVersion;
        _requestsDirty = true;
        drawnChanged = true;
    }

    // what the grid's edits reached
    if (!grid.takeChanges(_changes)) {
        for (auto& pair : _entries) {
            pair.second.stale = true;
        }
        _requestsDirty = true;
    } else if (!_changes.empty()) {
        for (auto& pair : _entries) {
            Entry& entry = pair.second;
            if (entry.stale) {
                continue;
            }
            for (const auto& box : _changes) {
                if (glm::all(glm::lessThanEqual(box.first, entry.readHigh)) && glm::all(glm::greaterThanEqual(box.second, entry.readLow))) {
                    entry.stale = true;
                    _requestsDirty = true;
                    break;
                }
            }
        }
    }

    // the cut wanted: chosen again when the camera has moved a couple of voxels or the grid's chunks changed
    const float voxel = (voxelMetres.x + voxelMetres.y + voxelMetres.z) / 3.0f;
    const bool moved = glm::length((camera - _cutCamera) * voxelMetres) > 2.0f * voxel;
    if (_cutWanted || moved || grid.getStructureVersion() != _cutStructure || voxelMetres != _cutVoxelMetres ||
            grid.getVoxelCount() != _cutVoxelCount) {
        _cut.update(grid, camera, voxelMetres, _settings);
        _cutChosenAt = std::chrono::steady_clock::now();
        _cutChosenFrame = _frame;
        _cutWanted = false;
        _cutCamera = camera;
        _cutVoxelMetres = voxelMetres;
        _cutStructure = grid.getStructureVersion();
        _cutVoxelCount = grid.getVoxelCount();
        _requestsDirty = true;
    }
    const std::vector<LodCut::Node>& wanted = _cut.getNodes();

    // the meshes to make: the wanted cut's, and the shown cut's that edits put out of date; nearest first
    if (_requestsDirty) {
        _requestsDirty = false;
        std::map<EntryKey, float> requests;
        auto consider = [&](const LodCut::Node& node) {
            if (node.isGridChunk) {
                return;
            }
            Entry& entry = entryFor(node);
            entry.lastUsed = _frame;
            if ((!entry.hasMesh || entry.stale) && !entry.inFlight) {
                requests[EntryKey { node.key, node.context.getSignature() }] = node.distance;
            }
        };
        for (const LodCut::Node& node : wanted) {
            consider(node);
        }
        for (const LodCut::Node& node : _shown) {
            consider(node);
        }
        std::vector<std::pair<float, EntryKey>> order;
        order.reserve(requests.size());
        for (const auto& request : requests) {
            order.emplace_back(request.second, request.first);
        }
        std::sort(order.begin(), order.end(), [](const std::pair<float, EntryKey>& a, const std::pair<float, EntryKey>& b) {
            return a.first < b.first;
        });
        // The first node always goes, so the queue drains; each later one only if the budget still holds it, judged
        // by what its level's last sends took: the frame's spend ends near the budget, not a whole node past it.
        // (A send is the hand-over alone, microseconds: the sampling runs on the pool, from one snapshot of the
        // grid for every node sent this frame.)
        const float budget = getFrameBudgetMs();
        const auto start = std::chrono::steady_clock::now();
        std::shared_ptr<const GridView> view;
        for (size_t i = 0; i < order.size(); ++i) {
            const auto before = std::chrono::steady_clock::now();
            const float spent = std::chrono::duration<float, std::milli>(before - start).count();
            const int level = order[i].second.key.level;
            const auto estimate = _nodeCostMs.find(level);
            const float expected = estimate != _nodeCostMs.end() ? estimate->second : 0.0f;
            if (i > 0 && spent + expected > budget) {
                _requestsDirty = true;   // the rest next frame
                break;
            }
            Entry& entry = _entries[order[i].second];
            const bool hadMesh = entry.hasMesh;
            const uint64_t version = entry.version;
            send(order[i].second, entry, grid, view);
            drawnChanged = drawnChanged || (entry.version != version) || (hadMesh != entry.hasMesh);
            const float cost = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - before).count();
            float& smoothed = _nodeCostMs[level];
            smoothed = smoothed > 0.0f ? smoothed * 0.75f + cost * 0.25f : cost;
        }
    }

    // the wanted cut is shown once every node of it has its mesh (at once when nothing is shown yet), or once a
    // node without one has held it back for the settings' force time
    if (_shown != wanted) {
        const LodCut::Node* stalled = nullptr;
        if (!_shown.empty()) {
            for (const LodCut::Node& node : wanted) {
                bool ready;
                if (node.isGridChunk) {
                    ready = gridChunkReady(node.key);
                } else {
                    auto found = _entries.find(EntryKey { node.key, node.context.getSignature() });
                    ready = found != _entries.end() && found->second.hasMesh;
                }
                if (!ready) {
                    stalled = &node;
                    break;
                }
            }
        }
        if (trackStall(grid, stalled)) {
            noteCutShown(wanted);
            _shown = wanted;
            drawnChanged = true;
        }
    } else {
        _stall.active = false;
    }

    evict();
    if (drawnChanged) {
        rebuildDrawn();
        ++_drawnVersion;
    }
    return drawnChanged;
}

float LevelOfDetail::getFrameBudgetMs() const {
    if (_frameBudgetMs > 0.0f && _frameBudgetMs < _settings.sampleBudgetMs) {
        return _frameBudgetMs;
    }
    return _settings.sampleBudgetMs;
}

void LevelOfDetail::noteCutShown(const std::vector<LodCut::Node>& wanted) {
    CutShown shown;
    shown.serial = _cutShown.serial + 1;
    if (_cutChosenAt != std::chrono::steady_clock::time_point()) {
        shown.msSinceChosen = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - _cutChosenAt).count();
        shown.framesSinceChosen = _frame - _cutChosenFrame;
    }
    if (!_cutShownCounting) {
        _cutShown = shown;
        return;
    }
    shown.nodes = wanted.size();
    // a node's mesh is new to the drawing when the last drawn set had no mesh at its key, or an older one
    std::map<ChunkKey, uint64_t> drawnVersions;
    for (const Drawn& drawn : _drawn) {
        if (!drawn.isGridChunk && drawn.mesh) {
            drawnVersions[drawn.key] = drawn.version;
        }
    }
    for (const LodCut::Node& node : wanted) {
        if (node.isGridChunk) {
            continue;
        }
        auto found = _entries.find(EntryKey { node.key, node.context.getSignature() });
        if (found == _entries.end() || !found->second.hasMesh || found->second.mesh.isEmpty()) {
            continue;
        }
        auto drawn = drawnVersions.find(node.key);
        if (drawn != drawnVersions.end() && drawn->second == found->second.version) {
            continue;
        }
        ++shown.newMeshes;
        shown.newVertices += found->second.mesh.getVertexCount();
        shown.newIndices += found->second.mesh.indices.size();
    }
    _cutShown = shown;
}

bool LevelOfDetail::trackStall(VoxelGrid& grid, const LodCut::Node* stalled) {
    if (!stalled) {
        _stall.active = false;
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!_stall.active || _stall.key != stalled->key || _stall.isGridChunk != stalled->isGridChunk) {
        _stall = Stall();
        _stall.key = stalled->key;
        _stall.isGridChunk = stalled->isGridChunk;
        _stall.active = true;
        _stallSince = now;
    }
    _stall.ms = std::chrono::duration<float, std::milli>(now - _stallSince).count();
    if (!_stall.nudged && _stall.ms >= _settings.stallNudgeMs) {
        // ask for the mesh again: a grid chunk's from the terrain's own mesher, a node's from here, dropping the
        // job out for it (its mesh, should it still come, is ignored: the job is no longer known)
        _stall.nudged = true;
        ++_stall.events;
        if (stalled->isGridChunk) {
            grid.markChunkDirty(stalled->key);
        } else {
            const EntryKey entryKey { stalled->key, stalled->context.getSignature() };
            for (auto it = _jobs.begin(); it != _jobs.end();) {
                it = !(it->second < entryKey) && !(entryKey < it->second) ? _jobs.erase(it) : std::next(it);
            }
            Entry& entry = entryFor(*stalled);
            entry.inFlight = false;
            entry.stale = true;
            _requestsDirty = true;
        }
    }
    if (!_stall.forced && _stall.ms >= _settings.stallForceMs) {
        _stall.forced = true;
        ++_stall.events;
        _stall.active = false;
        return true;
    }
    return false;
}

void LevelOfDetail::rebuildDrawn() {
    _drawn.clear();
    _drawn.reserve(_shown.size());
    for (const LodCut::Node& node : _shown) {
        Drawn drawn;
        drawn.key = node.key;
        drawn.isGridChunk = node.isGridChunk;
        if (!node.isGridChunk) {
            auto found = _entries.find(EntryKey { node.key, node.context.getSignature() });
            if (found != _entries.end() && found->second.hasMesh) {
                drawn.mesh = found->second.mesh.isEmpty() ? nullptr : &found->second.mesh;
                drawn.version = found->second.version;
            }
        }
        _drawn.push_back(drawn);
    }
}

void LevelOfDetail::evict() {
    const size_t limit = std::max<size_t>(256, 2 * (_cut.getNodes().size() + _shown.size()));
    if (_entries.size() <= limit) {
        return;
    }
    std::set<EntryKey> keep;
    const std::vector<LodCut::Node>* cuts[2] = { &_cut.getNodes(), &_shown };
    for (const std::vector<LodCut::Node>* nodes : cuts) {
        for (const LodCut::Node& node : *nodes) {
            if (!node.isGridChunk) {
                keep.insert(EntryKey { node.key, node.context.getSignature() });
            }
        }
    }
    std::vector<std::pair<uint64_t, EntryKey>> unused;
    for (const auto& pair : _entries) {
        if (!pair.second.inFlight && !keep.count(pair.first)) {
            unused.emplace_back(pair.second.lastUsed, pair.first);
        }
    }
    std::sort(unused.begin(), unused.end(), [](const std::pair<uint64_t, EntryKey>& a, const std::pair<uint64_t, EntryKey>& b) {
        return a.first < b.first;
    });
    for (const auto& entry : unused) {
        if (_entries.size() <= limit) {
            break;
        }
        _entries.erase(entry.second);
    }
}

bool LevelOfDetail::isSettled() const {
    if (!_jobs.empty() || _cutWanted || _requestsDirty || _shown != _cut.getNodes()) {
        return false;
    }
    for (const LodCut::Node& node : _shown) {
        if (node.isGridChunk) {
            continue;
        }
        auto found = _entries.find(EntryKey { node.key, node.context.getSignature() });
        if (found == _entries.end() || !found->second.hasMesh || found->second.stale) {
            return false;
        }
    }
    return true;
}

void LevelOfDetail::gridReplaced() {
    for (auto& pair : _entries) {
        pair.second.stale = true;
    }
    _cutWanted = true;
    _requestsDirty = true;
}

void LevelOfDetail::clear() {
    _mesher.reset();
    _stall = Stall();
    _jobs.clear();
    _entries.clear();
    _shown.clear();
    _drawn.clear();
    ++_drawnVersion;
    _cut.clear();
    _cutWanted = true;
    _requestsDirty = true;
}
