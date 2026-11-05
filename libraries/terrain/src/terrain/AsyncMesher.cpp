//
//  AsyncMesher.cpp
//  libraries/terrain/src/terrain
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AsyncMesher.h"

#include <algorithm>
#include <mutex>

#include <QThread>
#include <QThreadPool>

using namespace terrain;

struct AsyncMesher::Shared {
    std::mutex mutex;
    std::vector<Result> results;
    uint32_t generation { 0 };   // reset() moves it on; a job finishing under an older one is dropped
};

QThreadPool* AsyncMesher::defaultPool() {
    // Lives for the process: a pool's destructor waits for its jobs, which is nothing to do at static destruction.
    static QThreadPool* pool = [] {
        auto created = new QThreadPool();
        created->setObjectName("terrain meshing");
        // a brush step hands over up to 27 chunks at once; leave the main thread and the render thread their cores
        created->setMaxThreadCount(std::min(8, std::max(1, QThread::idealThreadCount() - 2)));
        return created;
    }();
    return pool;
}

AsyncMesher::AsyncMesher(QThreadPool* pool) : _shared(std::make_shared<Shared>()), _pool(pool ? pool : defaultPool()) {
}

AsyncMesher::~AsyncMesher() {
    reset();
}

int AsyncMesher::submit(VoxelGrid& grid) {
    if (grid.getDirtyChunks().empty()) {
        return 0;
    }
    const ChunkKeySet dirty = grid.getDirtyChunks();   // a copy: the chunks come off the set as they go out
    int started = 0;
    for (const ChunkKey& chunk : dirty) {
        if (_inFlight.count(chunk)) {
            continue;
        }
        if (!grid.hasChunk(chunk)) {
            grid.clearDirtyChunk(chunk);
            continue;
        }
        auto samples = std::make_shared<ChunkSamples>();
        grid.sampleChunk(chunk, *samples);
        grid.clearDirtyChunk(chunk);
        _inFlight.insert(chunk);
        ++started;
        if (samples->isOneSided()) {
            // all air or all ground: no surface, no mesh, and no need of a thread to say so
            Result result;
            result.key = chunk;
            std::lock_guard<std::mutex> lock(_shared->mutex);
            _shared->results.push_back(std::move(result));
            continue;
        }
        std::shared_ptr<Shared> shared = _shared;
        const uint32_t generation = _generation;
        _pool->start([shared, samples, generation] {
            Result result;
            result.key = samples->getKey();
            result.mesh = meshChunk(*samples);
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->generation == generation) {
                shared->results.push_back(std::move(result));
            }
        });
    }
    return started;
}

void AsyncMesher::submitSampler(const ChunkKey& key, std::function<bool(ChunkSamples&)> sampler, uint64_t tag, int priority) {
    std::shared_ptr<Shared> shared = _shared;
    const uint32_t generation = _generation;
    _pool->start([shared, key, sampler = std::move(sampler), generation, tag] {
        Result result;
        result.key = key;
        result.tag = tag;
        ChunkSamples samples;
        if (sampler(samples) && !samples.isOneSided()) {
            result.mesh = meshChunk(samples);
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->generation == generation) {
            shared->results.push_back(std::move(result));
        }
    }, priority);
}

std::vector<AsyncMesher::Result> AsyncMesher::collect() {
    std::vector<Result> results;
    {
        std::lock_guard<std::mutex> lock(_shared->mutex);
        results.swap(_shared->results);
    }
    for (const Result& result : results) {
        if (result.tag == 0) {
            _inFlight.erase(result.key);
        }
    }
    return results;
}

void AsyncMesher::reset() {
    {
        std::lock_guard<std::mutex> lock(_shared->mutex);
        _generation = ++_shared->generation;
        _shared->results.clear();
    }
    _inFlight.clear();
}

bool AsyncMesher::hasResults() const {
    std::lock_guard<std::mutex> lock(_shared->mutex);
    return !_shared->results.empty();
}
