//
//  AsyncMesher.h
//  libraries/terrain/src/terrain
//
//  Meshes a grid's dirty chunks on a thread pool. The editing thread samples each dirty chunk (ChunkSamples: its
//  voxels and one layer of its neighbours') and hands the samples to the pool, where meshChunk() runs on them and
//  nothing else; the finished meshes wait in a queue until the editing thread collects them. One job per chunk is
//  out at a time: a chunk edited while its job runs is dirty again and goes out once more after its mesh has come
//  back, and that mesh is still worth showing meanwhile. Nothing here touches the grid off the editing thread: a
//  job that samples for itself (submitSampler) reads a snapshot of it (GridView) that the caller gives it.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_terrain_AsyncMesher_h
#define hifi_terrain_AsyncMesher_h

#include <functional>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "SurfaceNets.h"
#include "VoxelGrid.h"

class QThreadPool;

namespace terrain {

class AsyncMesher {
public:
    struct Result {
        ChunkKey key;
        ChunkMesh mesh;      // empty when no surface crosses the chunk
        uint64_t tag { 0 };  // submitSampler()'s tag; 0 for the grid's chunks
        // What the finisher made from the mesh on the pool (setFinisher), if one is set and the mesh is not empty:
        // the caller's own type, kept with the mesh (a drawing's vertex buffer, say). Null otherwise.
        std::shared_ptr<void> attachment;
    };
    // A function run on the pool right after a mesh is made, on that thread, with the mesh: what it returns travels
    // with the mesh as Result::attachment. For a caller that wants work done beside the meshing rather than on its
    // own thread when the mesh comes back. Set before the first submit; jobs already out finish without it.
    using Finisher = std::function<std::shared_ptr<void>(const ChunkMesh&)>;
    void setFinisher(const Finisher& finisher);

    // The pool to run on; by default one of this library's own, so meshing neither waits behind nor holds up the
    // application's other background work.
    explicit AsyncMesher(QThreadPool* pool = nullptr);
    // Jobs still running finish on their own and their meshes are dropped.
    ~AsyncMesher();

    AsyncMesher(const AsyncMesher&) = delete;
    AsyncMesher& operator=(const AsyncMesher&) = delete;

    // Samples every dirty chunk that has no job out and starts one for it, taking the chunk off the grid's dirty
    // set; returns how many went out. A chunk whose samples hold no surface is answered at once with an empty
    // mesh. Chunks with a job out stay dirty until collect() has taken their mesh. A dirty key with no chunk at it
    // any more (split, or merged) is only taken off the set.
    int submit(VoxelGrid& grid);
    // Samples made on the pool by the function given (a node of the level of detail's, LevelOfDetail.h, read from
    // a snapshot of the grid the function holds), then meshed there, ahead of the grid's chunks when the priority is
    // higher. The function fills the samples and returns whether a surface may cross them; when it says none does,
    // or the samples are all of one side, the mesh comes back empty. The mesh comes back from collect() with the key
    // and this tag. Nothing is counted as in flight for it: the caller keeps track, with a mesher of its own.
    void submitSampler(const ChunkKey& key, std::function<bool(ChunkSamples&)> sampler, uint64_t tag, int priority = 0);
    // The meshes finished since the last call, in no particular order; their chunks have no job out afterwards.
    std::vector<Result> collect();
    // Forgets every job out: their meshes are dropped when they finish. For a grid that was replaced.
    void reset();

    size_t getInFlightCount() const { return _inFlight.size(); }
    bool isInFlight(const ChunkKey& key) const { return _inFlight.count(key) > 0; }
    bool hasResults() const;
    // No job out and no mesh waiting.
    bool isIdle() const { return _inFlight.empty() && !hasResults(); }

    static QThreadPool* defaultPool();

private:
    struct Shared;   // the queue the jobs write to; it outlives the mesher while a job still holds it
    std::shared_ptr<Shared> _shared;
    std::shared_ptr<const Finisher> _finisher;   // shared with the jobs out: a job runs the finisher it was given
    ChunkKeySet _inFlight;
    uint32_t _generation { 0 };
    QThreadPool* _pool;
};

} // namespace terrain

#endif // hifi_terrain_AsyncMesher_h
