//
//  VoxelGrid.cpp
//  libraries/terrain/src/terrain
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "VoxelGrid.h"

#include <algorithm>
#include <cassert>

using namespace terrain;

const size_t VoxelGrid::MAX_CHANGE_BOXES;

namespace {

// Finest-level voxels per grid voxel: the unit in which every level's points are integers.
const int FINEST = 1 << MAX_CHUNK_LEVEL;

int floorDivide(int value, int divisor) {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

glm::ivec3 floorDivide(const glm::ivec3& value, int divisor) {
    return glm::ivec3(floorDivide(value.x, divisor), floorDivide(value.y, divisor), floorDivide(value.z, divisor));
}

int childIndexOf(const glm::ivec3& bits) {
    return bits.x | (bits.y << 1) | (bits.z << 2);
}

float trilinear(const float d[8], const glm::vec3& f) {
    const float d00 = glm::mix(d[0], d[1], f.x);
    const float d10 = glm::mix(d[2], d[3], f.x);
    const float d01 = glm::mix(d[4], d[5], f.x);
    const float d11 = glm::mix(d[6], d[7], f.x);
    return glm::mix(glm::mix(d00, d10, f.y), glm::mix(d01, d11, f.y), f.z);
}

// Whether a chunk's samples (its voxels and one layer around them, at its level) read any voxel of the box from
// changedLow to changedHigh (inclusive, in finest units) of a chunk at changedLevel. A finer reader interpolates
// between the coarser chunk's voxels, so one of them reaches as far as the next; otherwise a reader takes the
// voxels at its own points, exactly.
bool samplesRead(const ChunkKey& reader, int changedLevel, const glm::ivec3& changedLow, const glm::ivec3& changedHigh) {
    const int readerVoxel = FINEST >> reader.level;
    const glm::ivec3 readerLow = reader.coords * (CHUNK_SIZE * readerVoxel) - readerVoxel;
    const glm::ivec3 readerHigh = reader.coords * (CHUNK_SIZE * readerVoxel) + CHUNK_SIZE * readerVoxel;
    if (reader.level > changedLevel) {
        const int support = FINEST >> changedLevel;
        return glm::all(glm::lessThan(changedLow - support, readerHigh)) && glm::all(glm::greaterThan(changedHigh + support, readerLow));
    }
    return glm::all(glm::lessThanEqual(changedLow, readerHigh)) && glm::all(glm::greaterThanEqual(changedHigh, readerLow));
}

} // namespace

// --- ChunkKey -----------------------------------------------------------------------------------

glm::ivec3 ChunkKey::getSlot() const {
    return level >= 0 ? floorDivide(coords, 1 << level) : coords * (1 << -level);
}

ChunkKey ChunkKey::getParent() const {
    return ChunkKey(level - 1, floorDivide(coords, 2));
}

bool ChunkKey::intersects(const glm::ivec3& low, const glm::ivec3& high) const {
    const glm::ivec3 origin = getOrigin();
    return glm::all(glm::lessThanEqual(origin, high)) && glm::all(glm::greaterThanEqual(origin + getSpan() - 1, low));
}

// --- ChunkNode ----------------------------------------------------------------------------------

namespace {

// The chunk of a node that has none of its own (a new node, or one just split): one empty chunk shared by all of
// them, which no node ever writes (shared, so mutableChunk() copies it first).
const std::shared_ptr<Chunk>& emptyChunk() {
    static const std::shared_ptr<Chunk> chunk = std::make_shared<Chunk>();
    return chunk;
}

} // namespace

ChunkNode::ChunkNode() : _chunk(emptyChunk()) {
}

ChunkNode::ChunkNode(Chunk chunk) : _chunk(std::make_shared<Chunk>(std::move(chunk))) {
}

// A copy shares the chunk (a GridView taken from the grid shares every chunk this way) and has children of its own.
ChunkNode::ChunkNode(const ChunkNode& other) : _chunk(other._chunk) {
    if (other._children) {
        _children.reset(new ChunkNode[8]);
        for (int i = 0; i < 8; ++i) {
            _children[i] = other._children[i];
        }
    }
}

ChunkNode& ChunkNode::operator=(const ChunkNode& other) {
    if (this != &other) {
        ChunkNode copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Chunk& ChunkNode::mutableChunk() {
    // Copies of a chunk's pointer are made only on the editing thread (a GridView taken from the grid) and dropped
    // anywhere (a view going out of use on a worker): a count of one read here means nothing else holds it, and no
    // other thread can come to hold it meanwhile. Otherwise the node takes a copy of its own and leaves the shared
    // chunk to its readers as it was.
    if (_chunk.use_count() != 1) {
        _chunk = std::make_shared<Chunk>(*_chunk);
    }
    return *_chunk;
}

void ChunkNode::split(std::array<Chunk, 8>&& children) {
    _children.reset(new ChunkNode[8]);
    for (int i = 0; i < 8; ++i) {
        _children[i]._chunk = std::make_shared<Chunk>(std::move(children[i]));
    }
    _chunk = emptyChunk();
}

void ChunkNode::merge(Chunk chunk) {
    _children.reset();
    _chunk = std::make_shared<Chunk>(std::move(chunk));
}

// --- ChunkSamples -------------------------------------------------------------------------------

bool ChunkSamples::isOneSided() const {
    const bool first = _voxels[0].isGround();
    for (const Voxel& voxel : _voxels) {
        if (voxel.isGround() != first) {
            return false;
        }
    }
    return true;
}

bool ChunkSamples::isUniform() const {
    const Voxel& first = _voxels[0];
    return std::all_of(_voxels.begin(), _voxels.end(), [&](const Voxel& voxel) { return voxel == first; });
}

const Voxel* ChunkSamples::findAt(int levelDelta, const glm::ivec3& point) const {
    for (const SampleBlock& block : _blocks) {
        if (block.levelDelta == levelDelta && block.contains(point)) {
            return &block.at(point);
        }
    }
    return nullptr;
}

void ChunkSamples::clearBlocks() {
    std::fill(std::begin(_neighbourLevels), std::end(_neighbourLevels), 0);
    _blocks.clear();
    std::fill(std::begin(_capLow), std::end(_capLow), false);
}

// --- VoxelGrid: the grid and its chunks ---------------------------------------------------------

VoxelGrid::VoxelGrid(const glm::ivec3& voxelCount, const Voxel& fill) {
    reset(voxelCount, fill);
}

void VoxelGrid::reset(const glm::ivec3& voxelCount, const Voxel& fill) {
    _voxelCount = glm::max(voxelCount, glm::ivec3(0));
    _chunkCount = (_voxelCount + CHUNK_SIZE - 1) / CHUNK_SIZE;
    _slots.clear();
    _slots.resize((size_t)_chunkCount.x * _chunkCount.y * _chunkCount.z, ChunkNode(Chunk(fill)));
    ++_structureVersion;
    markAllChunksDirty();
}

void VoxelGrid::setMaxLevel(int level) {
    _maxLevel = glm::clamp(level, 0, MAX_CHUNK_LEVEL);
}

glm::ivec3 GridView::chunkOf(const glm::ivec3& voxel) {
    return floorDivide(voxel, CHUNK_SIZE);
}

glm::ivec3 GridView::localOf(const glm::ivec3& voxel) {
    return voxel - chunkOf(voxel) * CHUNK_SIZE;
}

bool GridView::containsVoxel(const glm::ivec3& voxel) const {
    return glm::all(glm::greaterThanEqual(voxel, glm::ivec3(0))) && glm::all(glm::lessThan(voxel, _voxelCount));
}

bool GridView::containsChunk(const glm::ivec3& chunk) const {
    return glm::all(glm::greaterThanEqual(chunk, glm::ivec3(0))) && glm::all(glm::lessThan(chunk, _chunkCount));
}

bool GridView::containsChunk(const ChunkKey& key) const {
    return key.level >= 0 && key.level <= MAX_CHUNK_LEVEL && containsChunk(key.getSlot());
}

GridView::Found GridView::locate(const glm::ivec3& fine) const {
    ChunkKey key(0, fine >> (CHUNK_SHIFT + MAX_CHUNK_LEVEL));
    const ChunkNode* node = &_slots[chunkIndexOf(key.coords)];
    while (node->isSplit()) {
        const glm::ivec3 bits = (fine >> (CHUNK_SHIFT + MAX_CHUNK_LEVEL - key.level - 1)) & 1;
        node = &node->getChild(childIndexOf(bits));
        key = ChunkKey(key.level + 1, key.coords * 2 + bits);
    }
    return { node, key };
}

const ChunkNode* GridView::nodeAt(const ChunkKey& key) const {
    if (!containsChunk(key)) {
        return nullptr;
    }
    const ChunkNode* node = &_slots[chunkIndexOf(key.getSlot())];
    for (int level = 1; level <= key.level; ++level) {
        if (!node->isSplit()) {
            return nullptr;
        }
        node = &node->getChild(childIndexOf((key.coords >> (key.level - level)) & 1));
    }
    return node;
}

ChunkNode* VoxelGrid::nodeAt(const ChunkKey& key) {
    return const_cast<ChunkNode*>(static_cast<const VoxelGrid*>(this)->nodeAt(key));
}

ChunkKey GridView::chunkCovering(const glm::ivec3& voxel) const {
    return chunkCoveringPoint(0, voxel);
}

ChunkKey GridView::chunkCoveringPoint(int level, const glm::ivec3& point) const {
    if (_slots.empty()) {
        return ChunkKey();
    }
    level = glm::clamp(level, 0, MAX_CHUNK_LEVEL);
    const glm::ivec3 clamped = glm::clamp(point, glm::ivec3(0), (_voxelCount - 1) << level);
    return locate(clamped << (MAX_CHUNK_LEVEL - level)).key;
}

ChunkKey GridView::chunkCoveringFine(const glm::ivec3& fine) const {
    if (_slots.empty()) {
        return ChunkKey();
    }
    return locate(glm::clamp(fine, glm::ivec3(0), (_voxelCount - 1) * FINEST)).key;
}

void GridView::collectLeaves(const ChunkNode& node, const ChunkKey& key, const glm::ivec3& low, const glm::ivec3& high,
                              std::vector<ChunkKey>& keys) const {
    if (!key.intersects(low, high)) {
        return;
    }
    if (!node.isSplit()) {
        keys.push_back(key);
        return;
    }
    for (int i = 0; i < 8; ++i) {
        collectLeaves(node.getChild(i), key.getChild(i), low, high, keys);
    }
}

std::vector<ChunkKey> GridView::chunksCovering(const glm::ivec3& low, const glm::ivec3& high) const {
    std::vector<ChunkKey> keys;
    if (_slots.empty()) {
        return keys;
    }
    // the last chunks reach past the voxel count, to the end of their own voxels
    const glm::ivec3 lo = glm::max(low, glm::ivec3(0));
    const glm::ivec3 hi = glm::min(high, _chunkCount * CHUNK_SIZE - 1);
    if (glm::any(glm::greaterThan(lo, hi))) {
        return keys;
    }
    const glm::ivec3 slotLow = lo >> CHUNK_SHIFT;
    const glm::ivec3 slotHigh = hi >> CHUNK_SHIFT;
    for (int z = slotLow.z; z <= slotHigh.z; ++z) {
        for (int y = slotLow.y; y <= slotHigh.y; ++y) {
            for (int x = slotLow.x; x <= slotHigh.x; ++x) {
                const glm::ivec3 slot(x, y, z);
                collectLeaves(_slots[chunkIndexOf(slot)], ChunkKey(0, slot), lo, hi, keys);
            }
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool GridView::hasChunk(const ChunkKey& key) const {
    const ChunkNode* node = nodeAt(key);
    return node && !node->isSplit();
}

bool GridView::isSplit(const ChunkKey& key) const {
    const ChunkNode* node = nodeAt(key);
    return node && node->isSplit();
}

const Chunk& GridView::getChunk(const ChunkKey& key) const {
    const ChunkNode* node = nodeAt(key);
    assert(node && !node->isSplit());
    static const Chunk NOTHING;
    return node && !node->isSplit() ? node->getChunk() : NOTHING;
}

Chunk* VoxelGrid::findChunk(const ChunkKey& key) {
    ChunkNode* node = nodeAt(key);
    return node && !node->isSplit() ? &node->mutableChunk() : nullptr;
}

const Chunk* GridView::findChunk(const ChunkKey& key) const {
    const ChunkNode* node = nodeAt(key);
    return node && !node->isSplit() ? &node->getChunk() : nullptr;
}

// --- reading the field --------------------------------------------------------------------------

const Voxel& GridView::get(const glm::ivec3& voxel) const {
    static const Voxel NOTHING;
    if (_slots.empty()) {
        return NOTHING;
    }
    const glm::ivec3 clamped = glm::clamp(voxel, glm::ivec3(0), _voxelCount - 1);
    const Found found = locate(clamped << MAX_CHUNK_LEVEL);
    const glm::ivec3 local = (clamped << found.key.level) - found.key.coords * CHUNK_SIZE;
    return found.node->getChunk().get(local.x, local.y, local.z);
}

bool VoxelGrid::set(const glm::ivec3& voxel, const Voxel& value) {
    if (!containsVoxel(voxel)) {
        return false;
    }
    const Found found = locate(voxel << MAX_CHUNK_LEVEL);
    const glm::ivec3 local = (voxel << found.key.level) - found.key.coords * CHUNK_SIZE;
    // the grid is not const here, and the node is one of its own
    if (!const_cast<ChunkNode*>(found.node)->mutableChunk().set(local.x, local.y, local.z, value)) {
        return false;
    }
    voxelsChanged(found.key, local, local);
    return true;
}

Voxel GridView::sampleAt(int level, const glm::ivec3& point) const {
    if (_slots.empty()) {
        return Voxel();
    }
    if (level < 0) {
        // a coarser level's point is a grid voxel, or beyond the grid the nearest one in it
        level = std::max(level, MIN_LOD_LEVEL);
        return voxelAtFine(glm::clamp(point * (FINEST << -level), glm::ivec3(0), (_voxelCount - 1) * FINEST), level);
    }
    level = std::min(level, MAX_CHUNK_LEVEL);
    const glm::ivec3 clamped = glm::clamp(point, glm::ivec3(0), (_voxelCount - 1) << level);
    const glm::ivec3 fine = clamped << (MAX_CHUNK_LEVEL - level);
    const Found found = locate(fine);
    if (found.key.level >= level) {
        // the chunk has a voxel at the point: its own, or a finer one's where the point is one of its own too
        const glm::ivec3 local = (clamped << (found.key.level - level)) - found.key.coords * CHUNK_SIZE;
        const Voxel& voxel = found.node->getChunk().get(local.x, local.y, local.z);
        if (found.key.level == level) {
            return voxel;
        }
        return Voxel(rescaleDistance(voxel.getDistance(), 1.0f / (float)(1 << (found.key.level - level))), voxel.material);
    }
    return interpolateAt(found, fine, level);
}

Voxel GridView::interpolateAt(const Found& found, const glm::ivec3& fine, int level) const {
    // The point lies between the voxels of a coarser chunk: trilinear between the eight around it, the material
    // of the nearest. A corner beyond the chunk's edge is the neighbour's there, at whatever level it keeps it.
    const int coarse = found.key.level;
    const int shift = MAX_CHUNK_LEVEL - coarse;
    const glm::ivec3 corner = fine >> shift;
    const glm::ivec3 remainder = fine & ((1 << shift) - 1);
    const glm::vec3 f = glm::vec3(remainder) / (float)(1 << shift);
    const glm::ivec3 next = corner + glm::ivec3(glm::greaterThan(remainder, glm::ivec3(0)));
    const glm::ivec3 origin = found.key.coords * CHUNK_SIZE;
    auto voxelAt = [&](const glm::ivec3& p) -> Voxel {
        const glm::ivec3 local = p - origin;
        if (glm::all(glm::greaterThanEqual(local, glm::ivec3(0))) && glm::all(glm::lessThan(local, glm::ivec3(CHUNK_SIZE)))) {
            return found.node->getChunk().get(local.x, local.y, local.z);
        }
        return sampleAt(coarse, p);
    };
    float d[8];
    for (int k = 0; k < 8; ++k) {
        const glm::ivec3 p(k & 1 ? next.x : corner.x, k & 2 ? next.y : corner.y, k & 4 ? next.z : corner.z);
        d[k] = voxelAt(p).getDistance();
    }
    const float distance = trilinear(d, f);
    const glm::ivec3 nearest = (fine + (1 << (shift - 1))) >> shift;
    return Voxel(rescaleDistance(distance, (float)(1 << (level - coarse))), voxelAt(nearest).material);
}

Voxel GridView::voxelAtFine(const glm::ivec3& fine, int level) const {
    if (_slots.empty()) {
        return Voxel();
    }
    const Found found = locate(fine);
    assert(found.key.level >= level);
    const glm::ivec3 local = (fine >> (MAX_CHUNK_LEVEL - found.key.level)) - found.key.coords * CHUNK_SIZE;
    const Voxel& voxel = found.node->getChunk().get(local.x, local.y, local.z);
    if (found.key.level == level) {
        return voxel;
    }
    return Voxel(rescaleDistance(voxel.getDistance(), std::ldexp(1.0f, level - found.key.level)), voxel.material);
}

bool GridView::leavesAreOneSided(const ChunkNode& node, const ChunkKey& key, const glm::ivec3& low, const glm::ivec3& high,
                                  int& side) const {
    if (!key.intersects(low, high)) {
        return true;
    }
    if (node.isSplit()) {
        for (int i = 0; i < 8; ++i) {
            if (!leavesAreOneSided(node.getChild(i), key.getChild(i), low, high, side)) {
                return false;
            }
        }
        return true;
    }
    const Chunk& chunk = node.getChunk();
    if (!chunk.isUniform()) {
        return false;   // it may well be, but finding out costs as much as sampling it
    }
    const int chunkSide = chunk.getUniformValue().isGround() ? -1 : 1;
    if (side == 0) {
        side = chunkSide;
    }
    return side == chunkSide;
}

bool GridView::isOneSided(const glm::ivec3& low, const glm::ivec3& high) const {
    if (_slots.empty()) {
        return true;
    }
    const glm::ivec3 lo = glm::max(low, glm::ivec3(0));
    const glm::ivec3 hi = glm::min(high, _chunkCount * CHUNK_SIZE - 1);
    if (glm::any(glm::greaterThan(lo, hi))) {
        return true;
    }
    const glm::ivec3 slotLow = lo >> CHUNK_SHIFT;
    const glm::ivec3 slotHigh = hi >> CHUNK_SHIFT;
    int side = 0;
    for (int z = slotLow.z; z <= slotHigh.z; ++z) {
        for (int y = slotLow.y; y <= slotHigh.y; ++y) {
            for (int x = slotLow.x; x <= slotHigh.x; ++x) {
                const glm::ivec3 slot(x, y, z);
                if (!leavesAreOneSided(_slots[chunkIndexOf(slot)], ChunkKey(0, slot), lo, hi, side)) {
                    return false;
                }
            }
        }
    }
    return true;
}

glm::ivec3 GridView::limitOf(int level, const glm::ivec3& origin, int most) const {
    glm::ivec3 limit;
    for (int axis = 0; axis < 3; ++axis) {
        const int end = _voxelCount[axis] - origin[axis];   // grid voxels from the node's corner to the grid's end
        int voxels = 0;
        if (end > 0) {
            voxels = level >= 0 ? end << level : (end + (1 << -level) - 1) >> -level;
        }
        limit[axis] = std::min(voxels, most);
    }
    return limit;
}

GridView::FieldSample GridView::fieldAt(const glm::vec3& position) const {
    FieldSample sample;
    if (_slots.empty()) {
        return sample;
    }
    const glm::vec3 p = glm::clamp(position, glm::vec3(0.0f), glm::vec3(_voxelCount - 1));
    const glm::ivec3 fine = glm::clamp(glm::ivec3(glm::floor(p * (float)FINEST)), glm::ivec3(0), (_voxelCount - 1) * FINEST);
    const Found found = locate(fine);
    const int level = found.key.level;
    const float scale = (float)(1 << level);
    const glm::vec3 pointL = p * scale;
    const glm::ivec3 corner = fine >> (MAX_CHUNK_LEVEL - level);
    const glm::vec3 f = glm::clamp(pointL - glm::vec3(corner), glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::ivec3 next = corner + glm::ivec3(glm::greaterThan(f, glm::vec3(0.0f)));
    const glm::ivec3 origin = found.key.coords * CHUNK_SIZE;
    auto voxelAt = [&](const glm::ivec3& q) -> Voxel {
        const glm::ivec3 local = q - origin;
        if (glm::all(glm::greaterThanEqual(local, glm::ivec3(0))) && glm::all(glm::lessThan(local, glm::ivec3(CHUNK_SIZE)))) {
            return found.node->getChunk().get(local.x, local.y, local.z);
        }
        return sampleAt(level, q);
    };
    float d[8];
    for (int k = 0; k < 8; ++k) {
        const glm::ivec3 q(k & 1 ? next.x : corner.x, k & 2 ? next.y : corner.y, k & 4 ? next.z : corner.z);
        d[k] = voxelAt(q).getDistance();
    }
    const float distance = trilinear(d, f);
    // at the limit of the byte the distance means only "far": keep it there, in grid voxels too
    sample.distance = std::abs(distance) >= SATURATED_DISTANCE - SATURATION_EPSILON ? glm::sign(distance) * SATURATED_DISTANCE : distance / scale;
    sample.material = voxelAt(glm::ivec3(glm::round(pointL))).material;
    return sample;
}

// --- changing chunks ----------------------------------------------------------------------------

void VoxelGrid::voxelsChanged(const ChunkKey& key, const glm::ivec3& localLow, const glm::ivec3& localHigh) {
    if (!containsChunk(key)) {
        return;
    }
    const int shift = MAX_CHUNK_LEVEL - key.level;
    const glm::ivec3 changedLow = (key.coords * CHUNK_SIZE + glm::clamp(localLow, glm::ivec3(0), glm::ivec3(CHUNK_SIZE - 1))) << shift;
    const glm::ivec3 changedHigh = (key.coords * CHUNK_SIZE + glm::clamp(localHigh, glm::ivec3(0), glm::ivec3(CHUNK_SIZE - 1))) << shift;
    // every chunk whose samples can reach the voxels lies within a grid voxel of them
    noteChange(changedLow, changedHigh);
    const glm::ivec3 gridLow = (changedLow >> MAX_CHUNK_LEVEL) - 1;
    const glm::ivec3 gridHigh = ((changedHigh + FINEST - 1) >> MAX_CHUNK_LEVEL) + 1;
    for (const ChunkKey& reader : chunksCovering(gridLow, gridHigh)) {
        if (samplesRead(reader, key.level, changedLow, changedHigh)) {
            _dirtyChunks.insert(reader);
        }
    }
}

std::array<Chunk, 8> VoxelGrid::resampleChildren(const ChunkKey& key) const {
    ChunkSamples samples;
    sampleChunk(key, samples, false);
    std::array<Chunk, 8> children;
    if (samples.isUniform()) {
        const Voxel& value = samples.at(0, 0, 0);
        const Voxel finer(rescaleDistance(value.getDistance(), 2.0f), value.material);
        for (Chunk& child : children) {
            child = Chunk(finer);
        }
        return children;
    }
    // A child's voxel at its local (x, y, z) lies at the parent's local (offset * CHUNK_SIZE + local) / 2: on a
    // parent voxel or halfway between two, up to the first voxel beyond the parent (the samples' outer layer).
    std::vector<Voxel> voxels(CHUNK_VOXEL_COUNT);
    for (int i = 0; i < 8; ++i) {
        const glm::ivec3 offset(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                for (int x = 0; x < CHUNK_SIZE; ++x) {
                    const glm::ivec3 doubled = offset * CHUNK_SIZE + glm::ivec3(x, y, z);
                    const glm::ivec3 corner = doubled >> 1;
                    const glm::ivec3 odd = doubled & 1;
                    const glm::ivec3 next = corner + odd;
                    float d[8];
                    for (int k = 0; k < 8; ++k) {
                        d[k] = samples.at(k & 1 ? next.x : corner.x, k & 2 ? next.y : corner.y, k & 4 ? next.z : corner.z).getDistance();
                    }
                    const float distance = trilinear(d, glm::vec3(odd) * 0.5f);
                    const glm::ivec3 nearest = (doubled + 1) >> 1;
                    voxels[Chunk::indexOf(x, y, z)] = Voxel(rescaleDistance(distance, 2.0f), samples.at(nearest.x, nearest.y, nearest.z).material);
                }
            }
        }
        children[i].setVoxels(voxels);
    }
    return children;
}

void VoxelGrid::splitNode(ChunkNode& node, const ChunkKey& key, bool resample) {
    // whoever samples the chunk sees its ground anew (a doubled distance at the byte's limit saturates)
    voxelsChanged(key, glm::ivec3(0), glm::ivec3(CHUNK_SIZE - 1));
    std::array<Chunk, 8> children;
    if (resample) {
        children = resampleChildren(key);
    } else {
        for (Chunk& child : children) {
            child = Chunk(node.getChunk().getFirstVoxel());
        }
    }
    node.split(std::move(children));
    _dirtyChunks.erase(key);
    for (int i = 0; i < 8; ++i) {
        _dirtyChunks.insert(key.getChild(i));
    }
    ++_structureVersion;
}

bool VoxelGrid::splitChunk(const ChunkKey& key) {
    ChunkNode* node = nodeAt(key);
    if (!node || node->isSplit() || key.level >= MAX_CHUNK_LEVEL) {
        return false;
    }
    splitNode(*node, key, true);
    // the 2:1 rule: the chunks touching this one are at most one level coarser than its children now
    const glm::ivec3 origin = key.getOrigin();
    refine(origin - 1, origin + key.getSpan(), key.level);
    return true;
}

void VoxelGrid::refineNode(const ChunkKey& key, const glm::ivec3& low, const glm::ivec3& high, int level) {
    if (key.level >= level) {
        return;
    }
    ChunkNode* node = nodeAt(key);
    if (!node) {
        return;
    }
    if (!node->isSplit()) {
        splitChunk(key);
        node = nodeAt(key);
        if (!node || !node->isSplit()) {
            return;
        }
    }
    for (int i = 0; i < 8; ++i) {
        const ChunkKey child = key.getChild(i);
        if (child.intersects(low, high)) {
            refineNode(child, low, high, level);
        }
    }
}

void VoxelGrid::refine(const glm::ivec3& low, const glm::ivec3& high, int level) {
    level = std::min(level, _maxLevel);
    if (_slots.empty() || level <= 0) {
        return;
    }
    const glm::ivec3 lo = glm::max(low, glm::ivec3(0));
    const glm::ivec3 hi = glm::min(high, _chunkCount * CHUNK_SIZE - 1);
    if (glm::any(glm::greaterThan(lo, hi))) {
        return;
    }
    const glm::ivec3 slotLow = lo >> CHUNK_SHIFT;
    const glm::ivec3 slotHigh = hi >> CHUNK_SHIFT;
    for (int z = slotLow.z; z <= slotHigh.z; ++z) {
        for (int y = slotLow.y; y <= slotHigh.y; ++y) {
            for (int x = slotLow.x; x <= slotHigh.x; ++x) {
                refineNode(ChunkKey(0, glm::ivec3(x, y, z)), lo, hi, level);
            }
        }
    }
}

void VoxelGrid::installChunk(const ChunkKey& key, Chunk value, bool resample) {
    if (!containsChunk(key)) {
        return;
    }
    ChunkNode* node = &_slots[chunkIndexOf(key.getSlot())];
    for (int level = 1; level <= key.level; ++level) {
        if (!node->isSplit()) {
            splitNode(*node, ChunkKey(level - 1, key.coords >> (key.level - level + 1)), resample);
        }
        node = &node->getChild(childIndexOf((key.coords >> (key.level - level)) & 1));
    }
    if (node->isSplit()) {
        forEachLeaf(*node, key, [&](const ChunkKey& leaf, ChunkNode&) { _dirtyChunks.erase(leaf); });
        ++_structureVersion;
    }
    node->merge(std::move(value));
    voxelsChanged(key, glm::ivec3(0), glm::ivec3(CHUNK_SIZE - 1));
}

void VoxelGrid::setChunk(const ChunkKey& key, Chunk value) {
    installChunk(key, std::move(value), true);
}

void VoxelGrid::placeChunk(const ChunkKey& key, Chunk value) {
    installChunk(key, std::move(value), false);
}

// --- dirtiness and sampling ---------------------------------------------------------------------

void VoxelGrid::markChunkDirty(const ChunkKey& key) {
    if (hasChunk(key)) {
        _dirtyChunks.insert(key);
    }
}

void VoxelGrid::markAllChunksDirty() {
    _dirtyChunks.clear();
    forEachChunk([&](const ChunkKey& key, const Chunk&) { _dirtyChunks.insert(key); });
    for (Changes& changes : _changes) {
        changes.boxes.clear();
        changes.everything = true;
    }
}

void VoxelGrid::noteChange(const glm::ivec3& fineLow, const glm::ivec3& fineHigh) {
    for (Changes& changes : _changes) {
        if (changes.everything) {
            continue;
        }
        if (changes.boxes.size() >= MAX_CHANGE_BOXES) {
            changes.boxes.clear();
            changes.everything = true;
            continue;
        }
        changes.boxes.emplace_back(fineLow, fineHigh);
    }
}

bool VoxelGrid::takeChanges(std::vector<std::pair<glm::ivec3, glm::ivec3>>& boxes, ChangeLog log) {
    Changes& changes = _changes[(int)log];
    boxes.clear();
    const bool told = !changes.everything;
    if (told) {
        boxes.swap(changes.boxes);
    }
    changes.boxes.clear();
    changes.everything = false;
    return told;
}

void GridView::sampleChunk(const ChunkKey& key, ChunkSamples& samples, bool forMeshing) const {
    samples.setKey(key);
    samples.clearBlocks();
    samples.setLimits(limitOf(key.level, key.getOrigin(), CHUNK_SIZE), limitOf(key.level + 1, key.getOrigin(), 2 * CHUNK_SIZE));
    const Chunk* chunk = findChunk(key);
    const glm::ivec3 origin = key.coords * CHUNK_SIZE;
    for (int z = -1; z <= CHUNK_SIZE; ++z) {
        for (int y = -1; y <= CHUNK_SIZE; ++y) {
            for (int x = -1; x <= CHUNK_SIZE; ++x) {
                const bool own = chunk && x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE;
                samples.at(x, y, z) = own ? chunk->get(x, y, z) : sampleAt(key.level, origin + glm::ivec3(x, y, z));
            }
        }
    }
    if (!forMeshing) {
        return;
    }
    // Across each low face and low edge where the neighbours are at another level, their voxels at that level
    // over the cells of theirs that touch the chunk: the layer of cells outside the face or along the edge, two
    // points deep, and the chunk's whole side the other way. The neighbours of one region are all at one level
    // (the region is a level-k place beside a level-k chunk, and the 2:1 rule holds), so one point says which.
    // The corner below all three low faces touches the chunk at a point only, where no cell of its meets an edge
    // of the chunk's, so the mesher never needs it.
    // (the corner below all three low faces is needed only for the cap along a closed low face, whose polygons at
    // the chunk's corner take a cell of the neighbour there)
    bool anyCapLow = false;
    for (int axis = 0; axis < 3; ++axis) {
        anyCapLow = anyCapLow || ((_caps & (1 << (2 * axis))) != 0 && key.getOrigin()[axis] <= 0);
    }
    for (int region = 1; region < (anyCapLow ? 8 : 7); ++region) {
        const glm::ivec3 below(region & 1 ? -1 : 0, region & 2 ? -1 : 0, region & 4 ? -1 : 0);
        const int level = chunkCoveringPoint(key.level, origin + below).level;
        const int delta = glm::clamp(level - key.level, -1, 1);
        samples.setNeighbourLevel(region, delta);
        if (delta == 0) {
            continue;
        }
        SampleBlock block;
        block.levelDelta = delta;
        const int side = delta > 0 ? CHUNK_SIZE * 2 : CHUNK_SIZE / 2;   // the chunk's side in the block's voxels
        block.size = glm::ivec3(region & 1 ? 2 : side + 1, region & 2 ? 2 : side + 1, region & 4 ? 2 : side + 1);
        // Along a closed low face of the box the cap's edges start a voxel further out (SurfaceNets.h), and their
        // polygons reach the neighbours' cells a point further out along that axis: a finer neighbour's two points
        // (its cells at -2 and -1), a coarser one's one (its cell at -1).
        glm::ivec3 reach = below;
        for (int axis = 0; axis < 3; ++axis) {
            if (!(_caps & (1 << (2 * axis))) || key.getOrigin()[axis] > 0) {
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
        const glm::ivec3 first = key.coords * side + reach;   // the chunk's origin at the block's level, then the block's
        size_t index = 0;
        for (int z = 0; z < block.size.z; ++z) {
            for (int y = 0; y < block.size.y; ++y) {
                for (int x = 0; x < block.size.x; ++x) {
                    block.voxels[index++] = sampleAt(key.level + delta, first + glm::ivec3(x, y, z));
                }
            }
        }
        samples.addBlock(std::move(block));
    }
    applyCaps(samples);
}

// --- the box's closed faces ---------------------------------------------------------------------

void VoxelGrid::setCaps(uint8_t caps) {
    caps &= CAP_ALL;
    if (caps == _caps) {
        return;
    }
    _caps = caps;
    if (!_slots.empty()) {
        markAllChunksDirty();
    }
}

bool GridView::capsReach(const glm::ivec3& low, const glm::ivec3& high) const {
    if (_caps == CAP_NONE) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if ((_caps & (1 << (2 * axis))) && low[axis] <= 0) {
            return true;
        }
        if ((_caps & (2 << (2 * axis))) && high[axis] >= _voxelCount[axis] - 1) {
            return true;
        }
    }
    return false;
}

void GridView::applyCaps(ChunkSamples& samples) const {
    if (_caps == CAP_NONE || _slots.empty()) {
        return;
    }
    const ChunkKey& key = samples.getKey();
    const float voxel = key.getVoxelSize();   // grid voxels per voxel of the samples
    const glm::vec3 origin(key.getOrigin());
    // the planes half a voxel of the finest level past the grid's first and last voxels: within the layer of samples
    // around a chunk at every level (a finer chunk's layer reaches a shorter way out), so every level meets them
    const float past = 0.5f / (float)FINEST;
    const glm::vec3 lowFace(-past);
    const glm::vec3 highFace = glm::vec3(_voxelCount - 1) + past;
    // a point's distance outside the closed faces' planes, in grid voxels: the largest over the faces, far inside when none is closed
    auto outside = [&](const glm::vec3& p) {
        float distance = -4.0f * SATURATED_DISTANCE;
        for (int axis = 0; axis < 3; ++axis) {
            if (_caps & (1 << (2 * axis))) {
                distance = std::max(distance, lowFace[axis] - p[axis]);
            }
            if (_caps & (2 << (2 * axis))) {
                distance = std::max(distance, p[axis] - highFace[axis]);
            }
        }
        return distance;
    };
    // the field cut by the planes: a sample is at least as far out as the planes put it (the intersection with the box)
    auto cap = [&](Voxel& sample, const glm::vec3& p, float sampleVoxel) {
        const float planes = outside(p) / sampleVoxel;   // in the sample's own voxels
        if (planes > sample.getDistance()) {
            sample.setDistance(planes);
        }
    };
    for (int z = -1; z <= CHUNK_SIZE; ++z) {
        for (int y = -1; y <= CHUNK_SIZE; ++y) {
            for (int x = -1; x <= CHUNK_SIZE; ++x) {
                cap(samples.at(x, y, z), origin + glm::vec3(x, y, z) * voxel, voxel);
            }
        }
    }
    for (SampleBlock& block : const_cast<std::vector<SampleBlock>&>(samples.getBlocks())) {
        // the block's lattice is the neighbour's level: its points are at origin + (block origin + local) * its voxel
        const float blockVoxel = block.levelDelta > 0 ? voxel * 0.5f : (block.levelDelta < 0 ? voxel * 2.0f : voxel);
        size_t index = 0;
        for (int z = 0; z < block.size.z; ++z) {
            for (int y = 0; y < block.size.y; ++y) {
                for (int x = 0; x < block.size.x; ++x) {
                    cap(block.voxels[index++], origin + glm::vec3(block.origin + glm::ivec3(x, y, z)) * blockVoxel, blockVoxel);
                }
            }
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        samples.setCapLow(axis, (_caps & (1 << (2 * axis))) != 0 && key.getOrigin()[axis] <= 0);
    }
}

// --- filling and housekeeping -------------------------------------------------------------------

void VoxelGrid::fillHeightField(const std::function<float(int x, int z)>& height, uint8_t material) {
    std::vector<float> columnHeights(CHUNK_SIZE * CHUNK_SIZE);
    std::vector<Voxel> voxels;
    int index = 0;
    for (int cz = 0; cz < _chunkCount.z; ++cz) {
        for (int cy = 0; cy < _chunkCount.y; ++cy) {
            for (int cx = 0; cx < _chunkCount.x; ++cx) {
                const glm::ivec3 origin = glm::ivec3(cx, cy, cz) * CHUNK_SIZE;
                for (int z = 0; z < CHUNK_SIZE; ++z) {
                    for (int x = 0; x < CHUNK_SIZE; ++x) {
                        columnHeights[z * CHUNK_SIZE + x] = height(origin.x + x, origin.z + z);
                    }
                }
                voxels.assign(CHUNK_VOXEL_COUNT, Voxel());
                for (int z = 0; z < CHUNK_SIZE; ++z) {
                    for (int y = 0; y < CHUNK_SIZE; ++y) {
                        for (int x = 0; x < CHUNK_SIZE; ++x) {
                            const float distance = (float)(origin.y + y) - columnHeights[z * CHUNK_SIZE + x];
                            voxels[Chunk::indexOf(x, y, z)] = Voxel(encodeDistance(distance), material);
                        }
                    }
                }
                Chunk chunk;
                chunk.setVoxels(voxels);
                _slots[index++].merge(std::move(chunk));
            }
        }
    }
    ++_structureVersion;
    markAllChunksDirty();
}

void VoxelGrid::fillBelow(float height, uint8_t material) {
    fillHeightField([height](int, int) { return height; }, material);
}

void VoxelGrid::compactChunks() {
    for (ChunkNode& slot : _slots) {
        forEachLeaf(slot, ChunkKey(), [](const ChunkKey&, ChunkNode& node) {
            if (!node.getChunk().isUniform()) {
                node.mutableChunk().compact();
            }
        });
    }
}

int GridView::countChunks() const {
    int count = 0;
    forEachChunk([&](const ChunkKey&, const Chunk&) { ++count; });
    return count;
}

int GridView::countDenseChunks() const {
    int count = 0;
    forEachChunk([&](const ChunkKey&, const Chunk& chunk) {
        if (!chunk.isUniform()) {
            ++count;
        }
    });
    return count;
}

int GridView::getFinestLevel() const {
    int finest = 0;
    forEachChunk([&](const ChunkKey& key, const Chunk&) { finest = std::max(finest, key.level); });
    return finest;
}
