//
//  Created by Bradley Austin Davis on 2016/05/15
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "GLTexture.h"

#include <QtCore/QThread>
#include <NumericalConstants.h>

#include "GLBackend.h"

#define OVERSUBSCRIBED_PRESSURE_VALUE 0.95f
#define UNDERSUBSCRIBED_PRESSURE_VALUE 0.85f
#define DEFAULT_ALLOWED_TEXTURE_MEMORY_MB ((size_t)1024)
#define MAX_RESOURCE_TEXTURES_PER_FRAME 2
#define NO_BUFFER_WORK_SLEEP_TIME_MS 2
#define THREADED_TEXTURE_BUFFERING 1

static const size_t DEFAULT_ALLOWED_TEXTURE_MEMORY = MB_TO_BYTES(DEFAULT_ALLOWED_TEXTURE_MEMORY_MB);
// the loading boost (gpu::Texture::getTransferBoost, entry 33): the per-frame allocation and upload budgets times this
// while the loading screen is up; the GPU and the present thread have nothing else to show then
static const size_t TRANSFER_BOOST_FACTOR = 4;
// the upload budget's factor under the loading screen; TIVOLI_TRANSFER_UPLOAD_BOOST (1-4, test-only, read once)
// overrides it for the sitting that weighs the budget against the memory it holds (entry 33, Amendment 2)
static size_t uploadBoostFactor() {
    static const size_t factor = [] {
        bool ok = false;
        const int value = qgetenv("TIVOLI_TRANSFER_UPLOAD_BOOST").toInt(&ok);
        return (ok && value >= 1 && value <= (int)TRANSFER_BOOST_FACTOR) ? (size_t)value : TRANSFER_BOOST_FACTOR;
    }();
    return factor;
}

namespace gpu { namespace gl {

enum class MemoryPressureState
{
    Idle,
    Transfer,
    Undersubscribed,
};

static MemoryPressureState _memoryPressureState{ MemoryPressureState::Idle };
// the loading boost's turn (entry 33, Amendment 2): allocation one frame, upload the next, while both have work
static bool _boostAllocationTurn{ false };

template <typename T>
struct LessPairSecond {
    bool operator()(const T& a, const T& b) { return a.second < b.second; }
};

using QueuePair = std::pair<TextureWeakPointer, float>;
// Contains a priority sorted list of textures on which work is to be done over many frames
// Uses a weak pointer to the texture to avoid keeping it in scope if the client stops using it
using WorkQueue = std::priority_queue<QueuePair, std::vector<QueuePair>, LessPairSecond<QueuePair>>;

using ImmediateQueuePair = std::pair<TexturePointer, float>;
// Contains a priority sorted list of textures on which work is to be done in the current frame
using ImmediateWorkQueue = std::priority_queue<ImmediateQueuePair, std::vector<ImmediateQueuePair>, LessPairSecond<ImmediateQueuePair>>;

// A map of weak texture pointers to queues of work to be done to transfer their data from the backing store to the GPU
using TransferMap = std::map<TextureWeakPointer, TransferQueue, std::owner_less<TextureWeakPointer>>;

class GLTextureTransferEngineDefault : public GLTextureTransferEngine {
    using Parent = GLTextureTransferEngine;

public:
    // Called once per frame by the GLBackend to manage texture memory
    // Will deallocate textures if oversubscribed,
    void manageMemory() override;
    void shutdown() override;

protected:
    class TextureBufferThread : public QThread {
    public:
        TextureBufferThread(GLTextureTransferEngineDefault& parent) : _parent(parent) { start(); }

    protected:
        void run() override {
            while (!_parent._shutdown) {
                if (!_parent.processActiveBufferQueue()) {
                    QThread::msleep(NO_BUFFER_WORK_SLEEP_TIME_MS);
                }
            }
        }

        GLTextureTransferEngineDefault& _parent;
    };

    using ActiveTransferJob = std::pair<TexturePointer, TransferJobPointer>;
    using ActiveTransferQueue = std::list<ActiveTransferJob>;

    void populateActiveBufferQueue();
    bool processActiveBufferQueue();
    void processTransferQueues();
    void populateTransferQueue(const TexturePointer& texturePointer);
    //void addToWorkQueue(const TexturePointer& texturePointer);
    void updateMemoryPressure();

    void processDemotes(size_t relief, const std::vector<TexturePointer>& strongTextures);
    void processPromotes();

private:
    std::atomic<bool> _shutdown{ false };
    // Contains a priority sorted list of weak texture pointers that have been determined to be eligible for additional allocation
    // While the memory state is 'undersubscribed', items will be removed from this list and processed, allocating additional memory
    // per frame
    WorkQueue _promoteQueue;
    // This queue contains jobs that will buffer data from the texture backing store (ideally a memory mapped KTX file)
    // to a CPU memory buffer.  This queue is populated on the main GPU thread, and drained on a dedicated thread.
    // When an item on the _activeBufferQueue is completed it is put into the _activeTransferQueue
    ActiveTransferQueue _activeBufferQueue;
    // This queue contains jobs that will upload data from a CPU buffer into a GPU.  This queue is populated on the background
    // thread that process the _activeBufferQueue and drained on the main GPU thread
    ActiveTransferQueue _activeTransferQueue;
    // Mutex protecting the _activeTransferQueue & _activeBufferQueue since they are each accessed both from the main GPU thread
    // and the buffering thread
    Mutex _bufferMutex;
    // The buffering thread which drains the _activeBufferQueue and populates the _activeTransferQueue
    TextureBufferThread* _transferThread{ nullptr };
    // The amount of buffering work currently represented by the _activeBufferQueue
    std::atomic<size_t> _queuedBufferSize{ 0 };
    // This contains a map of all textures to queues of pending transfer jobs.  While in the transfer state, this map is used to
    // populate the _activeBufferQueue up to the limit specified in GLVariableAllocationTexture::MAX_BUFFER_SIZE
    TransferMap _pendingTransfersMap;
};

}}  // namespace gpu::gl

using namespace gpu;
using namespace gpu::gl;

void GLBackend::initTextureManagementStage() {
    _textureManagement._transferEngine = std::make_shared<GLTextureTransferEngineDefault>();
}

void GLBackend::killTextureManagementStage() {
    _textureManagement._transferEngine->shutdown();
    _textureManagement._transferEngine.reset();
}

std::vector<TexturePointer> GLTextureTransferEngine::getAllTextures() {
    auto expiredBegin = std::remove_if(_registeredTextures.begin(), _registeredTextures.end(), [&](const std::weak_ptr<Texture>& weak) -> bool {
        return weak.expired();
    });
    _registeredTextures.erase(expiredBegin, _registeredTextures.end());

    std::vector<TexturePointer> result;
    result.reserve(_registeredTextures.size());
    for (const auto& weak : _registeredTextures) {
        auto strong = weak.lock();
        if (!strong) {
            continue;
        }
        result.push_back(strong);
    }
    return result;
}

void GLTextureTransferEngine::addMemoryManagedTexture(const TexturePointer& texturePointer) {
    ++_frameTexturesCreated;
    _registeredTextures.push_back(texturePointer);
}

void GLTextureTransferEngineDefault::shutdown() {
    _shutdown = true;
#if THREADED_TEXTURE_BUFFERING
    if (_transferThread) {
        _transferThread->wait();
        delete _transferThread;
        _transferThread = nullptr;
    }
#endif
}

void GLTextureTransferEngineDefault::manageMemory() {
    PROFILE_RANGE(render_gpu_gl, __FUNCTION__);
    // reset the count used to limit the number of textures created per frame
    resetFrameTextureCreated();
    // Determine the current memory management state.  It will be either idle (no work to do),
    // undersubscribed (need to do more allocation) or transfer (need to upload content from the
    // backing store to the GPU
    updateMemoryPressure();
    if (MemoryPressureState::Undersubscribed == _memoryPressureState) {
        // If we're undersubscribed, we need to process some of the textures that can have additional allocation
        processPromotes();
    } else if (MemoryPressureState::Transfer == _memoryPressureState) {
        // If we're in transfer mode we need to manage the buffering and upload queues
        processTransferQueues();
    }
}

// Each frame we will check if our memory pressure state has changed.
void GLTextureTransferEngineDefault::updateMemoryPressure() {
    PROFILE_RANGE(render_gpu_gl, __FUNCTION__);

    size_t allowedMemoryAllocation = gpu::Texture::getAllowedGPUMemoryUsage();
    if (0 == allowedMemoryAllocation) {
        allowedMemoryAllocation = DEFAULT_ALLOWED_TEXTURE_MEMORY;
    }

    // Clear any defunct textures (weak pointers that no longer have a valid texture)
    auto strongTextures = getAllTextures();

    size_t totalVariableMemoryAllocation = 0;
    size_t idealMemoryAllocation = 0;
    bool canDemote = false;
    bool canPromote = false;
    bool hasTransfers = false;
    for (const auto& texture : strongTextures) {
        GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texture);
        GLVariableAllocationSupport* vartexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);
        vartexture->sanityCheck();

        // Track how much the texture thinks it should be using
        idealMemoryAllocation += texture->evalTotalSize();
        // Track how much we're actually using
        totalVariableMemoryAllocation += gltexture->size();
        if (vartexture->canDemote()) {
            canDemote |= true;
        }
        if (vartexture->canPromote()) {
            canPromote |= true;
        }
        if (vartexture->hasPendingTransfers()) {
            hasTransfers |= true;
        }
    }

    Backend::textureResourceIdealGPUMemSize.set(idealMemoryAllocation);
    size_t unallocated = idealMemoryAllocation - totalVariableMemoryAllocation;
    float pressure = (float)totalVariableMemoryAllocation / (float)allowedMemoryAllocation;

    // If we're oversubscribed we need to demote textures IMMEDIATELY
    if (pressure > OVERSUBSCRIBED_PRESSURE_VALUE && canDemote) {
        auto overPressure = pressure - OVERSUBSCRIBED_PRESSURE_VALUE;
        size_t relief = (size_t)(overPressure * totalVariableMemoryAllocation);
        Backend::textureTransferEngineBusy.set(1);   // demoting is work too (the arrival gate's read, below)
        processDemotes(relief, strongTextures);
        return;
    }

    const bool wantPromote = pressure < UNDERSUBSCRIBED_PRESSURE_VALUE && (unallocated != 0 && canPromote);
    bool queuesBusy = false;
    {
        Lock lock(_bufferMutex);
        queuesBusy = !_activeBufferQueue.empty() || !_activeTransferQueue.empty() || !_pendingTransfersMap.empty();
    }
    // What the engine still has to do, for the arrival gate (entry 33, Amendment 2: the gate opened on a small set of
    // textures that was fully uploaded while most were not yet allocated). Read from the work available, not from the
    // state: processPromotes and processTransferQueues set Idle when their queue empties, a frame before the rebuild.
    Backend::textureTransferEngineBusy.set((wantPromote || hasTransfers || queuesBusy) ? 1 : 0);

    auto newState = MemoryPressureState::Idle;
    if (Texture::getTransferBoost() && wantPromote && hasTransfers) {
        // under the loading screen (entry 33, Amendment 2): allocation and upload take turns, one frame each, while
        // both have work, so the uploads run beside the shader warm-up instead of after every allocation (item 53).
        // The first shape (upload whenever anything was queued) starved the allocation under the warm-up's slow
        // presents: 8 MB allocated where the old order had 120. Each turn rebuilds its queue below, as every state
        // change does. In a presented world the boost is off and the order below is the one before the boost.
        _boostAllocationTurn = !_boostAllocationTurn;
        newState = _boostAllocationTurn ? MemoryPressureState::Undersubscribed : MemoryPressureState::Transfer;
    } else if (wantPromote) {
        newState = MemoryPressureState::Undersubscribed;
    } else if (hasTransfers || queuesBusy) {
        newState = MemoryPressureState::Transfer;
    }

    // If we've changed state then we have to populate the appropriate structure with the work to be done
    if (newState != _memoryPressureState) {
        _memoryPressureState = newState;
        _promoteQueue = WorkQueue();
        _pendingTransfersMap.clear();

        if (MemoryPressureState::Idle == _memoryPressureState) {
            return;
        }

        // For each texture, if it's eligible for work in the current state, put it into the appropriate structure
        for (const auto& texture : strongTextures) {
            GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texture);
            GLVariableAllocationSupport* vargltexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);
            if (MemoryPressureState::Undersubscribed == _memoryPressureState && vargltexture->canPromote()) {
                // Promote smallest first
                _promoteQueue.push({ texture, 1.0f / (float)gltexture->size() });
            } else if (MemoryPressureState::Transfer == _memoryPressureState && vargltexture->hasPendingTransfers()) {
                populateTransferQueue(texture);
            }
        }
    }
}

// Manage the _activeBufferQueue and _activeTransferQueue queues
void GLTextureTransferEngineDefault::processTransferQueues() {
#if THREADED_TEXTURE_BUFFERING
    if (!_transferThread) {
        _transferThread = new TextureBufferThread(*this);
    }
#endif

    // From the pendingTransferMap, queue jobs into the _activeBufferQueue
    // Doing so will lock the weak texture pointer so that it can't be destroyed
    // while the background thread is working.
    //
    // This will queue jobs until _queuedBufferSize can't be increased without exceeding
    // GLVariableAllocationTexture::MAX_BUFFER_SIZE or there is no more work to be done
    populateActiveBufferQueue();
#if !THREADED_TEXTURE_BUFFERING
    processActiveBufferQueue();
#endif

    // Take any tasks which have completed buffering and process them, uploading the buffered
    // data to the GPU.  Drains the _activeTransferQueue
    {
        ActiveTransferQueue activeTransferQueue;
        {
            Lock lock(_bufferMutex);
            activeTransferQueue.swap(_activeTransferQueue);
        }

        while (!activeTransferQueue.empty()) {
            const auto& activeTransferJob = activeTransferQueue.front();
            const auto& texturePointer = activeTransferJob.first;
            GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texturePointer);
            GLVariableAllocationSupport* vargltexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);
            const auto& tranferJob = activeTransferJob.second;
            if (tranferJob->sourceMip() < vargltexture->populatedMip()) {
                tranferJob->transfer(texturePointer);
            }
            // The pop_front MUST be the last call since all of these varaibles in scope are
            // references that will be invalid after the pop
            activeTransferQueue.pop_front();
        }
    }

    // If we have no more work in any of the structures, reset the memory state to idle to
    // force reconstruction of the _pendingTransfersMap if necessary
    {
        Lock lock(_bufferMutex);
        if (_activeTransferQueue.empty() && _activeBufferQueue.empty() && _pendingTransfersMap.empty()) {
            _memoryPressureState = MemoryPressureState::Idle;
        }
    }
}

void GLTextureTransferEngineDefault::populateActiveBufferQueue() {
    size_t queuedBufferSize = _queuedBufferSize;
    static const auto& MAX_BUFFER_SIZE = GLVariableAllocationSupport::MAX_BUFFER_SIZE;
    // entry 33: the loading screen's budget; the queue may hold a boosted frame's worth when the boost ends, so the
    // limit the asserts hold is the boosted one and the available size never underflows
    static const size_t MAX_QUEUED_BUFFER_SIZE = MAX_BUFFER_SIZE * TRANSFER_BOOST_FACTOR;
    const size_t frameBufferSize = MAX_BUFFER_SIZE * (Texture::getTransferBoost() ? uploadBoostFactor() : 1);
    Q_ASSERT(queuedBufferSize <= MAX_QUEUED_BUFFER_SIZE);
    size_t availableBufferSize = queuedBufferSize < frameBufferSize ? frameBufferSize - queuedBufferSize : 0;

    // Queue up buffering jobs
    ActiveTransferQueue newBufferJobs;
    size_t newTransferSize{ 0 };

    for (auto itr = _pendingTransfersMap.begin(); itr != _pendingTransfersMap.end();) {
        const auto& weakTexture = itr->first;
        const auto texture = weakTexture.lock();

        // Texture no longer exists, remove from the transfer map and move on
        if (!texture) {
            itr = _pendingTransfersMap.erase(itr);
            continue;
        }

        GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texture);
        GLVariableAllocationSupport* vargltexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);

        auto& textureTransferQueue = itr->second;
        // Can't find any pending transfers, so move on
        if (textureTransferQueue.empty()) {
            if (vargltexture->hasPendingTransfers()) {
                // qWarning(gpugllogging) << "Texture " << gltexture->_id << "(" << texture->source().c_str() << ") has no transfer jobs, but has pending transfers" ;
            }
            itr = _pendingTransfersMap.erase(itr);
            continue;
        }

        const auto& transferJob = textureTransferQueue.front();
        const auto& transferSize = transferJob->size();
        // If there's not enough space for the buffering, then break out of the loop
        if (transferSize > availableBufferSize) {
            break;
        }
        availableBufferSize -= transferSize;
        Q_ASSERT(availableBufferSize <= MAX_QUEUED_BUFFER_SIZE);
        Q_ASSERT(newTransferSize <= MAX_QUEUED_BUFFER_SIZE);
        newTransferSize += transferSize;
        Q_ASSERT(newTransferSize <= MAX_QUEUED_BUFFER_SIZE);
        newBufferJobs.emplace_back(texture, transferJob);
        textureTransferQueue.pop();
        ++itr;
    }

    {
        Lock lock(_bufferMutex);
        _activeBufferQueue.splice(_activeBufferQueue.end(), newBufferJobs);
        Q_ASSERT(_queuedBufferSize <= MAX_QUEUED_BUFFER_SIZE);
        _queuedBufferSize += newTransferSize;
        Q_ASSERT(_queuedBufferSize <= MAX_QUEUED_BUFFER_SIZE);
    }
}

bool GLTextureTransferEngineDefault::processActiveBufferQueue() {
    ActiveTransferQueue activeBufferQueue;
    {
        Lock lock(_bufferMutex);
        _activeBufferQueue.swap(activeBufferQueue);
    }

    if (activeBufferQueue.empty()) {
        return false;
    }

    for (const auto& activeJob : activeBufferQueue) {
        const auto& texture = activeJob.first;
        const auto& transferJob = activeJob.second;
        // Some jobs don't require buffering, but they pass through this queue to ensure that we don't re-order
        // the buffering & transfer operations.  All jobs in the queue must be processed in order.
        if (!transferJob->bufferingRequired()) {
            continue;
        }
        const auto& transferSize = transferJob->size();
        transferJob->buffer(texture);
        Q_ASSERT(_queuedBufferSize >= transferSize);
        _queuedBufferSize -= transferSize;
    }

    {
        Lock lock(_bufferMutex);
        _activeTransferQueue.splice(_activeTransferQueue.end(), activeBufferQueue);
    }

    Texture::KtxStorage::releaseOpenKtxFiles();
    return true;
}

void GLTextureTransferEngineDefault::populateTransferQueue(const TexturePointer& texturePointer) {
    TextureWeakPointer weakTexture = texturePointer;
    GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texturePointer);
    GLVariableAllocationSupport* vargltexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);
    TransferJob::Queue pendingTransfers;
    PROFILE_RANGE(render_gpu_gl, __FUNCTION__);
    vargltexture->populateTransferQueue(pendingTransfers);
    if (!pendingTransfers.empty()) {
        _pendingTransfersMap[weakTexture] = pendingTransfers;
    }
}

// From the queue of textures to be promited
void GLTextureTransferEngineDefault::processPromotes() {
    // FIXME use max allocated memory per frame instead of promotion count
    static const size_t MAX_ALLOCATED_BYTES_PER_FRAME = GLVariableAllocationSupport::MAX_BUFFER_SIZE;
    static const size_t MAX_ALLOCATIONS_PER_FRAME = 8;
    const size_t boost = Texture::getTransferBoost() ? TRANSFER_BOOST_FACTOR : 1;   // entry 33: the loading screen's
    size_t allocatedBytes{ 0 };
    size_t allocations{ 0 };

    while (!_promoteQueue.empty()) {
        // Grab the first item off the demote queue
        PROFILE_RANGE(render_gpu_gl, __FUNCTION__);
        auto entry = _promoteQueue.top();
        _promoteQueue.pop();
        auto texture = entry.first.lock();
        if (!texture) {
            continue;
        }

        GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texture);
        GLVariableAllocationSupport* vartexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);
        auto originalSize = gltexture->size();
        vartexture->promote();
        // the loading boost (entry 33, Amendment 2): a texture goes to its full size before its uploads start. The
        // Mac's promote makes a new texture and copies the populated mips into it; the old order never copied, since
        // nothing was uploaded before everything was allocated, and alternating uploads with one-step promotes would
        // copy what the last upload turn had just put there, at the present thread's and the driver's expense.
        while (boost > 1 && vartexture->canPromote() &&
               gltexture->size() - originalSize < MAX_ALLOCATED_BYTES_PER_FRAME * boost) {
            vartexture->promote();
        }
        auto allocationDelta = gltexture->size() - originalSize;
        if (vartexture->canPromote()) {
            // Promote smallest first
            _promoteQueue.push({ texture, 1.0f / (float)gltexture->size() });
        }
        allocatedBytes += allocationDelta;
        if (++allocations >= MAX_ALLOCATIONS_PER_FRAME * boost) {
            break;
        }
        if (allocatedBytes >= MAX_ALLOCATED_BYTES_PER_FRAME * boost) {
            break;
        }
    }

    // Get the front of the work queue to perform work
    if (_promoteQueue.empty()) {
        // Force rebuild of work queue
        _memoryPressureState = MemoryPressureState::Idle;
    }
}

void GLTextureTransferEngineDefault::processDemotes(size_t reliefRequired, const std::vector<TexturePointer>& strongTextures) {
    // Demote largest first
    ImmediateWorkQueue demoteQueue;
    for (const auto& texture : strongTextures) {
        GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texture);
        GLVariableAllocationSupport* vargltexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);
        if (vargltexture->canDemote()) {
            demoteQueue.push({ texture, (float)gltexture->size() });
        }
    }

    size_t relieved = 0;
    while (!demoteQueue.empty() && relieved < reliefRequired) {
        {
            const auto& target = demoteQueue.top();
            const auto& texture = target.first;
            GLTexture* gltexture = Backend::getGPUObject<GLTexture>(*texture);
            auto oldSize = gltexture->size();
            GLVariableAllocationSupport* vargltexture = dynamic_cast<GLVariableAllocationSupport*>(gltexture);
            vargltexture->demote();
            auto newSize = gltexture->size();
            relieved += (oldSize - newSize);
        }
        demoteQueue.pop();
    }
}

// FIXME hack for stats display
QString getTextureMemoryPressureModeString() {
    switch (_memoryPressureState) {
        case MemoryPressureState::Undersubscribed:
            return "Undersubscribed";

        case MemoryPressureState::Transfer:
            return "Transfer";

        case MemoryPressureState::Idle:
            return "Idle";
    }
    Q_UNREACHABLE();
    return "Unknown";
}
