//
//  RenderableTerrainEntityItem.cpp
//  libraries/entities-renderer/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderableTerrainEntityItem.h"

#include <algorithm>
#include <atomic>

#include <cfloat>
#include <mutex>
#include <set>

#include <glm/gtx/transform.hpp>
#include <QCryptographicHash>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <AABox.h>
#include <AbstractViewStateInterface.h>
#include <DeferredLightingEffect.h>
#include <DependencyManager.h>
#include <EntityTree.h>
#include <GeometryUtil.h>
#include <GLMHelpers.h>
#include <PathUtils.h>
#include <ResourceManager.h>
#include <ResourceRequest.h>
#include <SimulationFlags.h>
#include <StencilMaskPass.h>
#include <ViewFrustum.h>
#include <gpu/Context.h>
#include <render/ShapePipeline.h>
#include <shaders/Shaders.h>
#include <terrain/TerrainFile.h>

#include "EntitiesRendererLogging.h"
#include "entities-renderer/ShaderConstants.h"

// The serial in a shape key ("terrain/<id>/<serial>") comes from one counter for the whole process: per entity object it
// restarted at 1 whenever the entity was made again (leaving and re-entering a domain), and the shape manager keeps
// released shapes for a while under their keys, so a changed shape could come back as the old one (10-21 review).
static uint64_t nextShapeSerial() {
    static std::atomic<uint64_t> serial { 0 };
    return ++serial;
}

using namespace render;
using namespace render::entities;

namespace {

// 20 bytes. The position stays a float: the meshes of neighbouring nodes meet exactly because both place a shared
// vertex at the same point (SurfaceNets.h), which a position quantized in each node's own voxel would break where
// levels meet. The normal is a 10-10-10-2 signed normalized integer (as the models' are), read as before.
struct TerrainVertex {
    glm::vec3 position;
    uint32_t normal;      // glm_packSnorm3x10_1x2
    uint32_t materials;   // the triangle's three materials in the low bytes, this corner (0-2) in the high byte;
                          // read as the colour attribute, a byte per channel
};

// The layer parameters as terrain.slf reads them (std140: vec4s and a mat4 only).
struct TerrainLayerParams {
    glm::vec4 color;    // rgb: the plain colour; a: 1 while a texture is bound
    glm::vec4 tiling;   // x: repeats per metre; yz: the offset, in repeats; w: opacity
    glm::vec4 tint;     // rgb: multiplies the texture; w: the normal map's strength
    glm::ivec4 flags;   // x: visible; y: the blend mode (terrain::LayerBlend); z: the normal array's slice holds the
                        // layer's normal map and it is on; w: the mask array's slice is drawn
};
// The parameters of a draw that fills a slice of the arrays (terrain_layer_maps.slf).
struct TerrainMapParams {
    glm::ivec4 which;   // x: 0 the normal slice, 1 the mask slice, 2 the colour slice; y, z, w: whether the first, second and third source is bound
    glm::vec4 plain;    // x: roughness, y: occlusion, z: height, written for a source that is not bound
};
const float PLAIN_ROUGHNESS = 0.9f;   // what a layer without a roughness map is, as the whole terrain was before maps
const float PLAIN_OCCLUSION = 1.0f;
const float PLAIN_HEIGHT = 0.5f;
struct TerrainParams {
    glm::mat4 worldToTerrain;
    TerrainLayerParams layers[TerrainLayerSet::LAYER_COUNT];   // by slot
    glm::ivec4 order[2];   // the slots drawn over the voxel material, bottom to top; -1 past the stack's end
};
// One chunk's own parameters (terrain.slf): where in its column's paint tile its voxels are read, and where in its
// column's top surface tile, with its voxels' height in the grid.
struct TerrainChunkParams {
    glm::vec4 paint;    // x: tile texture coordinates per voxel of the chunk; yz: the tile coordinates of its voxel (0, 0);
                        // w: 1 while the Height blend applies here, 0 on a coarse node (its tile averages the map)
    glm::vec4 top;      // x: top tile texture coordinates per voxel of the chunk; yz: the top tile coordinates of its voxel
                        // (0, 0); w: 1 while the paint lies on the top surface alone (a chunk of the grid's), 0 on a coarse node
    glm::vec4 vertical; // x: grid voxels per voxel of the chunk; y: the grid height of its voxel 0
};

// The plain colours of slots the stack has no layer for, and of layers named by the entity's terrainLayers property
// (which gives a texture and a tiling alone): grass, dirt, rock, sand, clay, snow, dark stone, moss.
const glm::vec3 LAYER_PALETTE[TerrainLayerSet::LAYER_COUNT] = {
    glm::vec3(0.33f, 0.52f, 0.22f), glm::vec3(0.42f, 0.30f, 0.18f), glm::vec3(0.50f, 0.50f, 0.50f), glm::vec3(0.78f, 0.70f, 0.50f),
    glm::vec3(0.62f, 0.32f, 0.22f), glm::vec3(0.92f, 0.93f, 0.95f), glm::vec3(0.28f, 0.28f, 0.30f), glm::vec3(0.24f, 0.40f, 0.20f)
};

// How far out every terrain draws full detail (RenderableTerrainEntityItem::setLevelOfDetail): main thread only.
float levelOfDetail = terrain::LodSettings().detail;

// TIVOLI_TERRAIN_DEBUG in the environment: the log gets what is drawn each time it changes, and each chunk item's
// first draw, for a bug hunt (the holes of 2026-10-18).
const bool terrainDebug = qEnvironmentVariableIsSet("TIVOLI_TERRAIN_DEBUG");
// the level of detail's own lines (entry 24's measure): one per cut shown, one per drawn-set update with buffers built
const bool lodDiag = qEnvironmentVariableIsSet("TIVOLI_TERRAIN_LOD_DIAG");

QString describeKey(const terrain::ChunkKey& key) {
    return QString("level %1 at %2 %3 %4").arg(key.level).arg(key.coords.x).arg(key.coords.y).arg(key.coords.z);
}

uint8_t CUSTOM_PIPELINE_NUMBER = 0;
std::map<std::pair<bool, bool>, ShapePipelinePointer> _pipelines;   // (forward, wireframe)
gpu::Stream::FormatPointer _vertexFormat;
gpu::TexturePointer _unpaintedTile;   // one texel of no weight at all: a chunk without a tile reads it
gpu::TexturePointer _noTopTile;       // one height under any ground: a chunk without a top tile reads it
gpu::PipelinePointer _mapPipeline;    // draws a layer's map into a slice of a set's arrays, shared by every set

gpu::PipelinePointer mapPipeline() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto program = gpu::Shader::createProgram(shader::entities_renderer::program::terrain_layer_maps);
        _mapPipeline = gpu::Pipeline::create(program, std::make_shared<gpu::State>());
    });
    return _mapPipeline;
}

// A top surface tile (TopSurface::fillTile): heights in grid voxels, two floats a texel (the top, the sheltered
// floor), gathered four columns at a time.
gpu::TexturePointer makeTopTileTexture(int size, const std::vector<float>& heights, const char* name) {
    const gpu::Element format(gpu::VEC2, gpu::FLOAT, gpu::XY);
    auto texture = gpu::Texture::createStrict(format, (uint16_t)size, (uint16_t)size, gpu::Texture::SINGLE_MIP,
                                              gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_LINEAR, gpu::Sampler::WRAP_CLAMP));
    texture->setSource(name);
    texture->setStoredMipFormat(format);
    texture->assignStoredMip(0, heights.size() * sizeof(float), (const gpu::Byte*)heights.data());
    return texture;
}

gpu::TexturePointer makeTileTexture(int size, const std::vector<uint8_t>& rgba, const char* name) {
    auto texture = gpu::Texture::createStrict(gpu::Element::COLOR_RGBA_32, (uint16_t)size, (uint16_t)size, gpu::Texture::SINGLE_MIP,
                                              gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_LINEAR, gpu::Sampler::WRAP_CLAMP));
    texture->setSource(name);
    texture->setStoredMipFormat(gpu::Element::COLOR_RGBA_32);
    texture->assignStoredMip(0, rgba.size(), rgba.data());
    return texture;
}

// The main pass asks for the chunks' pipeline through their custom shape key; shadow passes draw them with the
// standard shadow pipeline (renderShadowCasters picks it by the key's deformation bits, not the custom number).
ShapePipelinePointer terrainPipelineFactory(const ShapePlumber& plumber, const ShapeKey& key, RenderArgs* args) {
    if (_pipelines.empty()) {
        namespace program = shader::entities_renderer::program;
        for (int forward = 0; forward < 2; ++forward) {
            for (int wireframe = 0; wireframe < 2; ++wireframe) {
                auto state = std::make_shared<gpu::State>();
                state->setCullMode(gpu::State::CULL_BACK);
                state->setDepthTest(true, true, gpu::GREATER_EQUAL);
                PrepareStencil::testMaskDrawShape(*state);
                if (wireframe) {
                    state->setFillMode(gpu::State::FILL_LINE);
                }
                auto shaderProgram = gpu::Shader::createProgram(forward ? program::terrain_forward : program::terrain);
                // the forward variant binds the frame's lighting itself, as the lit pipelines' batch setter does
                // (RenderPipelines' lightBatchSetter); without one it saw what the pipeline before it left bound
                ShapePipeline::BatchSetter batchSetter = nullptr;
                if (forward) {
                    batchSetter = [](const ShapePipeline&, gpu::Batch& batch, RenderArgs* args) {
                        auto deferredLightingEffect = DependencyManager::get<DeferredLightingEffect>();
                        deferredLightingEffect->setupKeyLightBatch(args, batch);
                        deferredLightingEffect->setupFrameLightingBatch(batch, args);
                    };
                }
                _pipelines[std::make_pair(forward != 0, wireframe != 0)] =
                    std::make_shared<ShapePipeline>(gpu::Pipeline::create(shaderProgram, state), nullptr, batchSetter, nullptr);
            }
        }
    }
    return _pipelines[std::make_pair(args->_renderMethod == Args::RenderMethod::FORWARD, key.isWireframe())];
}

void initTerrainPipelines() {
    static std::once_flag once;
    std::call_once(once, [] {
        CUSTOM_PIPELINE_NUMBER = ShapePipeline::registerCustomShapePipelineFactory(terrainPipelineFactory);
        _vertexFormat = std::make_shared<gpu::Stream::Format>();
        _vertexFormat->setAttribute(gpu::Stream::POSITION, 0, gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::XYZ), offsetof(TerrainVertex, position));
        _vertexFormat->setAttribute(gpu::Stream::NORMAL, 0, gpu::Element(gpu::VEC4, gpu::NINT2_10_10_10, gpu::XYZW), offsetof(TerrainVertex, normal));
        _vertexFormat->setAttribute(gpu::Stream::COLOR, 0, gpu::Element(gpu::VEC4, gpu::NUINT8, gpu::RGBA), offsetof(TerrainVertex, materials));
        _unpaintedTile = makeTileTexture(1, std::vector<uint8_t>(4, 0), "terrain unpainted");
        _noTopTile = makeTopTileTexture(1, std::vector<float>(2, terrain::NO_GROUND_HEIGHT), "terrain no top surface");
    });
}

} // namespace

const float RenderableTerrainEntityItem::PAINT_TEXEL_METRES = 0.125f;
const int RenderableTerrainEntityItem::MAX_PAINT_TEXELS_PER_AXIS = 2048;

// The entity item

EntityItemPointer RenderableTerrainEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    std::shared_ptr<RenderableTerrainEntityItem> entity(new RenderableTerrainEntityItem(entityID), [](EntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

RenderableTerrainEntityItem::RenderableTerrainEntityItem(const EntityItemID& entityItemID) : TerrainEntityItem(entityItemID),
    _madeBuffers(std::make_shared<TerrainMadeBuffers>()) {
    // the level of detail counts what each cut shown brings only when the line that reports it is asked for
    _levelOfDetail.setCutShownCounting(qEnvironmentVariableIsSet("TIVOLI_TERRAIN_LOD_DIAG"));
    // every mesh made on the pool, the grid's chunks' and the level of detail's, comes with its vertex buffer made
    // there too (entry 24 step 3a); the buffers are queued here for the backend as they come (step 3b)
    std::shared_ptr<TerrainMadeBuffers> made = _madeBuffers;
    terrain::AsyncMesher::Finisher finisher = [made](const terrain::ChunkMesh& mesh) -> std::shared_ptr<void> {
        TerrainMeshBuffer::Pointer buffer = TerrainMeshBuffer::make(mesh);
        if (buffer) {
            std::lock_guard<std::mutex> lock(made->mutex);
            made->made.push_back(buffer);
        }
        return buffer;
    };
    _mesher.setFinisher(finisher);
    _levelOfDetail.setMeshFinisher(finisher);
}

int RenderableTerrainEntityItem::paintTexelsPerVoxel(float voxelMetres, const glm::ivec2& voxelFootprint) {
    int texels = glm::clamp((int)std::lround(voxelMetres / PAINT_TEXEL_METRES), 1, 8);
    const int longest = std::max(std::max(voxelFootprint.x, voxelFootprint.y), 1);
    while (texels > 1 && longest * texels > MAX_PAINT_TEXELS_PER_AXIS) {
        --texels;
    }
    return texels;
}

void RenderableTerrainEntityItem::makeDefaultGrid() {
    const glm::ivec3 voxelCount = getVoxelCount();
    terrain::VoxelGrid grid(voxelCount);
    grid.fillBelow(0.5f * voxelCount.y);
    replaceGrid(std::move(grid), terrain::PaintMap());
    if (_layers.isEmpty()) {
        _layers = legacyOrDefaultLayers();
        _layersFixed = false;
        ++_layersVersion;
    }
}

bool RenderableTerrainEntityItem::loadGrid(const QByteArray& file, QString& error) {
    terrain::VoxelGrid loaded;
    terrain::PaintMap paint;
    terrain::LayerStack layers;
    std::string why;
    if (!terrain::loadTerrain((const uint8_t*)file.constData(), (size_t)file.size(), loaded, &paint, &layers, &why)) {
        error = QString::fromStdString(why);
        return false;
    }
    const glm::ivec3 expected = getVoxelCount();
    if (expected != loaded.getVoxelCount()) {
        qCWarning(entitiesrenderer) << "Terrain" << getID() << "file holds" << loaded.getVoxelCount().x << "x" << loaded.getVoxelCount().y
                                    << "x" << loaded.getVoxelCount().z << "voxels but the entity's dimensions hold" << expected.x << "x"
                                    << expected.y << "x" << expected.z << "; drawing the file's";
    }
    replaceGrid(std::move(loaded), std::move(paint));
    // the file's layers; a file without them (before version 4) draws what the property names, or the defaults
    _layersFixed = !layers.isEmpty();
    _layers = _layersFixed ? std::move(layers) : legacyOrDefaultLayers();
    ++_layersVersion;
    return true;
}

void RenderableTerrainEntityItem::setLayers(const terrain::LayerStack& layers, bool fixed) {
    _layers = layers;
    _layersFixed = fixed;
    ++_layersVersion;
    somethingChangedNotification();
}

terrain::LayerStack RenderableTerrainEntityItem::legacyOrDefaultLayers() const {
    QString error;
    const std::vector<TerrainLayerSpec> specs = parseTerrainLayers(getTerrainLayers(), &error);
    if (!error.isEmpty()) {
        qCWarning(entitiesrenderer) << "Terrain" << getID() << "layers are not usable:" << error;
    }
    if (specs.empty()) {
        return terrain::LayerStack::defaults();
    }
    terrain::LayerStack stack;
    for (size_t i = 0; i < specs.size() && i < (size_t)terrain::MAX_LAYERS; ++i) {
        terrain::Layer layer;
        layer.slot = (uint8_t)i;
        layer.name = "Layer " + std::to_string(i + 1);
        layer.materialURL = specs[i].albedoURL.toStdString();
        layer.tiling = specs[i].tiling;
        layer.color = LAYER_PALETTE[i];
        stack.layers.push_back(layer);
    }
    stack.fillDefaultMaps();   // a default material named by its colour texture gets its maps
    return stack;
}

void RenderableTerrainEntityItem::clearGrid() {
    replaceGrid(terrain::VoxelGrid(), terrain::PaintMap());
}

void RenderableTerrainEntityItem::setGridAndPaint(terrain::VoxelGrid grid, terrain::PaintMap paint) {
    replaceGrid(std::move(grid), std::move(paint));
    somethingChangedNotification();
}

void RenderableTerrainEntityItem::setCaps(uint8_t caps) {
    if (caps == _grid.getCaps()) {
        return;
    }
    _grid.setCaps(caps);   // every chunk dirty, and both change logs say everything changed
    _levelOfDetail.gridReplaced();
    _shapeStale = true;
    somethingChangedNotification();
}

void RenderableTerrainEntityItem::setLevelOfDetail(float detail) {
    levelOfDetail = std::max(detail, 0.0f);
}

float RenderableTerrainEntityItem::getLevelOfDetail() {
    return levelOfDetail;
}

void RenderableTerrainEntityItem::replaceGrid(terrain::VoxelGrid grid, terrain::PaintMap paint) {
    _grid = std::move(grid);
    // what is drawn stays up until the meshes of the new grid are in
    if (_grid.isEmpty()) {
        _levelOfDetail.clear();
    } else {
        _levelOfDetail.gridReplaced();
    }
    // the map: the given one when it covers the grid, a fresh one at the texel size otherwise, none without a grid
    const glm::ivec2 footprint(_grid.getVoxelCount().x, _grid.getVoxelCount().z);
    if (_grid.isEmpty()) {
        _paint = terrain::PaintMap();
    } else if (!paint.isEmpty() && paint.getVoxelFootprint() == footprint) {
        _paint = std::move(paint);
    } else {
        const glm::vec3 voxelScale = getVoxelScale();
        const float voxelMetres = std::max((voxelScale.x + voxelScale.z) * 0.5f, 1.0e-4f);
        _paint = terrain::PaintMap(footprint, paintTexelsPerVoxel(voxelMetres, footprint));
    }
    _paint.markAllColumnsDirty();
    ++_paintVersion;
    _mesher.reset();   // meshes of the old grid still being built are dropped when they come
    _structureVersion = _grid.getStructureVersion();
    _staleMeshes = false;
    // a fresh grid has every chunk dirty, so the meshes within it are all replaced in time; those beyond it go now
    const terrain::ChunkKeySet& dirty = _grid.getDirtyChunks();
    bool erased = false;
    for (auto it = _chunkMeshes.begin(); it != _chunkMeshes.end();) {
        if (dirty.count(it->first)) {
            ++it;
        } else {
            it = _chunkMeshes.erase(it);
            erased = true;
        }
    }
    if (erased) {
        ++_meshVersion;
    }
    _shapeStale = true;
}

bool RenderableTerrainEntityItem::updateMeshes() {
    bool changed = false;
    std::vector<terrain::AsyncMesher::Result> results = _mesher.collect();
    takeMadeBuffers();   // before the meshes they belong to are taken: a buffer is queued no later than its mesh
    if (!results.empty()) {
        ++_meshVersion;
        for (terrain::AsyncMesher::Result& result : results) {
            if (result.mesh.isEmpty()) {
                _chunkMeshes.erase(result.key);
                continue;
            }
            ChunkMeshEntry& entry = _chunkMeshes[result.key];
            entry.mesh = std::move(result.mesh);
            entry.buffer = std::static_pointer_cast<TerrainMeshBuffer>(result.attachment);
            entry.version = _meshVersion;
        }
        changed = true;
    }
    // Chunks split or merged: a mesh whose chunk is gone stays up until the chunks now in its place all have
    // theirs (or none, for a chunk without a surface), so the ground does not blink while they are meshed.
    if (_grid.getStructureVersion() != _structureVersion) {
        _structureVersion = _grid.getStructureVersion();
        _staleMeshes = true;
    }
    if (_staleMeshes) {
        _staleMeshes = false;
        for (auto it = _chunkMeshes.begin(); it != _chunkMeshes.end();) {
            if (_grid.hasChunk(it->first)) {
                ++it;
                continue;
            }
            bool pending = false;
            const glm::ivec3 origin = it->first.getOrigin();
            for (const terrain::ChunkKey& key : _grid.chunksCovering(origin, origin + it->first.getSpan() - 1)) {
                if (_grid.getDirtyChunks().count(key) || _mesher.isInFlight(key)) {
                    pending = true;
                    break;
                }
            }
            if (pending) {
                _staleMeshes = true;
                ++it;
            } else {
                it = _chunkMeshes.erase(it);
                ++_meshVersion;
                changed = true;
            }
        }
    }
    _mesher.submit(_grid);
    return changed;
}

// The level of detail's sampling runs on this thread, in the frame: it may take this share of a frame, the frame
// measured as the median of the last periods between updates (the display's interval at a steady rate: long frames
// and the catch-ups after them fall either side of it), never less than the floor so the queue drains. At 72 Hz the
// share is the settings' own 4 ms and nothing changes; at 120 and 144 Hz a cut spreads over frames where one frame
// could not hold it (2.5 and 2.1 ms). The periods are taken once enough are in.
static const float LOD_SAMPLE_SHARE_OF_FRAME = 0.3f;
static const float LOD_SAMPLE_BUDGET_FLOOR_MS = 1.0f;
// The vertex buffers a new cut brings are given to the backend before the cut is shown (entry 24 step 3b), as one
// post a frame of the wanted backlog, whole, up to this many bytes (step 3d; the first buffer due always goes). The
// uploads land in the present thread's execute stage, and at 144 Hz in Port Amarelle that stage sits at its slow
// line already (half a refresh, 3.45 ms, against ~3.3 ms of its own), so what the governor's climb-back counts is
// the NUMBER of frames that upload, not their size: a cruise cut (3-8 MB) goes in one post, as main's first-draw
// upload did (step 3b's 2 MB pieces made it two or three slow executes a cut, and the lap of 11-06 19:13 never
// climbed back from 72); the boarding cut after a teleport (~48 MB) in 6 frames and the arrival's first cut (100-150
// MB) in 13-19, where the view has changed whole. The max is ~1.6-5.6 ms at the 0.2-0.7 ms per MB estimated so far
// (the execute column of the 3d legs measures it); 16 MB would slip five presents at 144 and drop the governor at
// every teleport (performance, 11-06). The arrival is the exception: for its first seconds the present thread
// carries the world's own first draws (executes of 30-90 ms, where the governor's drop is decided: three presents
// over two refreshes in a second), so the share stays small until the terrain has been drawn for a while, and the
// first cut comes node by node over a second or so, as the models come; a teleport later gets the whole share.
static const size_t LOD_UPLOAD_BYTES_MAX = 8 * 1024 * 1024;
static const size_t LOD_UPLOAD_BYTES_ARRIVAL = 2 * 1024 * 1024;
static const float LOD_UPLOAD_ARRIVAL_SECONDS = 10.0f;

bool RenderableTerrainEntityItem::updateLevelOfDetail(const glm::vec3& cameraWorld) {
    terrain::LodSettings settings = _levelOfDetail.getSettings();
    if (settings.detail != levelOfDetail) {
        settings.detail = levelOfDetail;
        _levelOfDetail.setSettings(settings);
    }
    const auto now = std::chrono::steady_clock::now();
    if (_lastLodUpdate != std::chrono::steady_clock::time_point()) {
        _framePeriodsMs[_framePeriodCount++ % _framePeriodsMs.size()] = std::chrono::duration<float, std::milli>(now - _lastLodUpdate).count();
    }
    _lastLodUpdate = now;
    if (_framePeriodCount >= _framePeriodsMs.size()) {
        std::array<float, 16> periods = _framePeriodsMs;
        std::sort(periods.begin(), periods.end());
        const float framePeriodMs = periods[periods.size() / 2];
        _levelOfDetail.setFrameBudgetMs(std::max(LOD_SAMPLE_BUDGET_FLOOR_MS, LOD_SAMPLE_SHARE_OF_FRAME * framePeriodMs));
    }
    const glm::vec3 camera(worldToVoxelMatrix() * glm::vec4(cameraWorld, 1.0f));
    // a chunk of the grid is drawn with its own mesh once that is there, or known to have no surface
    struct Refused {
        bool mesh { false };
        bool dirty { false };
        bool inFlight { false };
    } refused;   // the state of the last chunk refused: the one holding a new cut back, if any
    takeMadeBuffers();   // the level of detail's buffers, before its meshes are collected below
    // a buffer is ready to draw once a batch has referenced it (resident), or when it was made without one; one
    // found not resident is wanted (entry 24 step 3d): it goes to the backend ahead of what nothing waits for
    ++_wantSerial;
    auto bufferReady = [this](const TerrainMeshBuffer::Pointer& buffer) {
        if (!buffer || buffer->resident) {
            return true;
        }
        wantBuffer(buffer);
        return false;
    };
    const bool changed = _levelOfDetail.update(_grid, camera, getVoxelScale(), [&](const terrain::ChunkKey& key) {
        auto found = _chunkMeshes.find(key);
        const bool mesh = found != _chunkMeshes.end();
        const bool dirty = _grid.getDirtyChunks().count(key) > 0;
        const bool inFlight = _mesher.isInFlight(key);
        if (!mesh && (dirty || inFlight)) {
            refused = Refused { mesh, dirty, inFlight };
            return false;
        }
        return !mesh || bufferReady(found->second.buffer);
    }, [&](const std::shared_ptr<void>& attachment) {
        return bufferReady(std::static_pointer_cast<TerrainMeshBuffer>(attachment));
    });
    // what is drawn and still waits for its buffer (a new item of the first cut, or of a cut forced through a stall)
    // is wanted this update too; with the pending cut's marks above, every mark carries this update's serial, and the
    // marks of a cut abandoned by a later choice expire with it (entry 24 step 3d)
    for (const terrain::LevelOfDetail::Drawn& node : _levelOfDetail.getDrawn()) {
        if (node.isGridChunk) {
            auto found = _chunkMeshes.find(node.key);
            if (found != _chunkMeshes.end()) {
                wantBuffer(found->second.buffer);
            }
        } else {
            wantBuffer(std::static_pointer_cast<TerrainMeshBuffer>(node.attachment));
        }
    }
    // A node of a new cut that has had no mesh for seconds (terrain::LodSettings::stallNudgeMs, stallForceMs) is
    // asked for again, then shown without: say so, with the state that held it, so a stuck cut can be read off the log.
    const terrain::LevelOfDetail::Stall& stall = _levelOfDetail.getStall();
    if (stall.events != _stallEventsLogged) {
        _stallEventsLogged = stall.events;
        const QString state = stall.isGridChunk
            ? QString("a chunk of the grid; mesh %1, dirty %2, in flight %3").arg(refused.mesh).arg(refused.dirty).arg(refused.inFlight)
            : QString("a node of the level of detail; %1 meshes kept, %2 jobs out").arg(_levelOfDetail.getMeshCount()).arg(_levelOfDetail.getInFlightCount());
        qCWarning(entitiesrenderer) << "Terrain" << getID()
            << (stall.forced ? "shows a new cut without a node that has had no mesh for" : "asks again for the mesh of a node that has had none for")
            << (int)stall.ms << "ms: level" << stall.key.level << "at" << stall.key.coords.x << stall.key.coords.y << stall.key.coords.z
            << qPrintable(state);
    }
    // One line per cut shown, when asked for (TIVOLI_TERRAIN_LOD_DIAG=1): how long after its choice it came, in ms
    // and frames, and what it brings to the GPU (the vertex buffer is de-indexed: a TerrainVertex per index).
    const terrain::LevelOfDetail::CutShown& cut = _levelOfDetail.getCutShown();
    if (lodDiag && cut.serial != _cutShownLogged) {
        _cutShownLogged = cut.serial;
        qCDebug(entitiesrenderer) << "Terrain LOD cut" << cut.serial << "shown" << cut.msSinceChosen << "ms /" << cut.framesSinceChosen
            << "frames after its choice:" << cut.nodes << "nodes," << cut.newMeshes << "new meshes,"
            << (qulonglong)(cut.newIndices * sizeof(TerrainVertex)) << "bytes to upload; sample budget"
            << _levelOfDetail.getFrameBudgetMs() << "ms";
    }
    // One line per re-choice of the cut, once the next has come: the re-choice frame's own cost by part and the
    // costliest frame from it to the next re-choice (the level of detail keeps them while the line is asked for).
    const terrain::LevelOfDetail::ReChoice& reChoice = _levelOfDetail.getReChoiceDone();
    if (lodDiag && reChoice.serial != _reChoiceLogged) {
        _reChoiceLogged = reChoice.serial;
        const terrain::LevelOfDetail::FrameCost& own = reChoice.own;
        const terrain::LevelOfDetail::FrameCost& worst = reChoice.worst;
        qCDebug(entitiesrenderer) << "Terrain LOD re-choice" << reChoice.serial << ":" << reChoice.nodes << "nodes; own frame choice"
            << own.choiceMs << "requests" << own.requestsMs << "(" << own.requests << ") sends" << own.sendsMs << "(" << own.sent
            << ") collect" << own.collectMs << "(" << own.results << ") drawn" << own.drawnMs << "=" << own.totalMs()
            << "ms; worst frame +" << (qlonglong)(worst.frame - own.frame) << ": collect" << worst.collectMs << "(" << worst.results
            << ") drawn" << worst.drawnMs << "=" << worst.totalMs() << "ms";
    }
    return changed;
}

QString RenderableTerrainEntityItem::describeDrawn() const {
    std::map<int, int> byLevel;
    int meshes = 0;
    QStringList without;
    for (const terrain::LevelOfDetail::Drawn& node : _levelOfDetail.getDrawn()) {
        ++byLevel[node.key.level];
        if (node.isGridChunk) {
            if (_chunkMeshes.count(node.key)) {
                ++meshes;
            } else {
                without << QString("grid chunk %1 (dirty %2, in flight %3, has chunk %4)").arg(describeKey(node.key))
                    .arg(_grid.getDirtyChunks().count(node.key) > 0).arg(_mesher.isInFlight(node.key)).arg(_grid.hasChunk(node.key));
            }
        } else if (node.mesh) {
            ++meshes;
        } else {
            without << QString("node %1").arg(describeKey(node.key));
        }
    }
    QString levels;
    for (const auto& pair : byLevel) {
        levels += QString(" level %1: %2").arg(pair.first).arg(pair.second);
    }
    return QString("%1 nodes (%2), %3 with meshes; %4 chunk meshes kept, %5 dirty, %6 LOD meshes, %7 LOD jobs out, settled %8; without a mesh: %9")
        .arg(_levelOfDetail.getDrawn().size()).arg(levels.trimmed()).arg(meshes).arg(_chunkMeshes.size()).arg(_grid.getDirtyChunks().size())
        .arg(_levelOfDetail.getMeshCount()).arg(_levelOfDetail.getInFlightCount()).arg(_levelOfDetail.isSettled())
        .arg(without.isEmpty() ? QString("none") : without.join(", "));
}

void RenderableTerrainEntityItem::getDrawnChunks(std::vector<DrawnChunk>& drawn) const {
    // versions of the chunks' own meshes and of the level of detail's are counted apart: even and odd keep them apart
    drawn.clear();
    for (const terrain::LevelOfDetail::Drawn& node : _levelOfDetail.getDrawn()) {
        if (node.isGridChunk) {
            auto found = _chunkMeshes.find(node.key);
            if (found != _chunkMeshes.end()) {
                drawn.push_back({ node.key, &found->second.mesh, found->second.version * 2, found->second.buffer });
            }
        } else if (node.mesh) {
            drawn.push_back({ node.key, node.mesh, node.version * 2 + 1, std::static_pointer_cast<TerrainMeshBuffer>(node.attachment) });
        }
    }
}

void RenderableTerrainEntityItem::takeMadeBuffers() {
    std::vector<TerrainMeshBuffer::Pointer> made;
    {
        std::lock_guard<std::mutex> lock(_madeBuffers->mutex);
        made.swap(_madeBuffers->made);
    }
    for (TerrainMeshBuffer::Pointer& buffer : made) {
        buffer->uploadSerial = ++_nextUploadSerial;
        _uploads.push_back(std::move(buffer));
    }
}

void RenderableTerrainEntityItem::wantBuffer(const TerrainMeshBuffer::Pointer& buffer) {
    if (buffer && !buffer->resident) {
        buffer->wanted = _wantSerial;
    }
}

// A buffer the backend has (resident) leaves the queue, and so does a dead one: its mesh is gone (the level of
// detail evicted the entry or replaced its mesh, or the grid's chunk was meshed again), so the queue's reference is
// the last. Anywhere in the queue, not only at its head: what nobody waits for sits behind nothing.
void RenderableTerrainEntityItem::pruneUploads() {
    _uploads.erase(std::remove_if(_uploads.begin(), _uploads.end(), [](const TerrainMeshBuffer::Pointer& buffer) {
        return !buffer || buffer->resident || buffer.use_count() == 1;
    }), _uploads.end());
}

void RenderableTerrainEntityItem::getUploadsDue(size_t maxShare, std::vector<TerrainMeshBuffer::Pointer>& due, uint64_t& serial) {
    due.clear();
    serial = 0;
    pruneUploads();
    // the wanted backlog: what a cut or a drawn item waited for in the latest update and the backend has not got yet
    _uploadsWanted = 0;
    _uploadsWantedBytes = 0;
    for (const TerrainMeshBuffer::Pointer& buffer : _uploads) {
        if (buffer->wanted == _wantSerial) {
            ++_uploadsWanted;
            _uploadsWantedBytes += buffer->buffer->getSize();
        }
    }
    // the post on its way: the same buffers again, until the renderer says the backend has them
    if (_postOut) {
        for (const TerrainMeshBuffer::Pointer& buffer : _uploads) {
            if (buffer->posted == _postOut) {
                due.push_back(buffer);
            }
        }
        serial = _postOut;
        return;
    }
    // a new post: the wanted buffers in queue order, the backlog whole up to the share (the first always)
    const size_t share = maxShare;
    size_t bytes = 0;
    for (const TerrainMeshBuffer::Pointer& buffer : _uploads) {
        if (buffer->wanted != _wantSerial) {
            continue;
        }
        const size_t size = buffer->buffer->getSize();
        if (!due.empty() && bytes + size > share) {
            break;
        }
        due.push_back(buffer);
        bytes += size;
    }
    if (!due.empty()) {
        serial = ++_nextPostSerial;
        for (const TerrainMeshBuffer::Pointer& buffer : due) {
            buffer->posted = serial;
        }
        _postOut = serial;
    }
}

void RenderableTerrainEntityItem::uploadsFed(uint64_t serial) {
    if (!_postOut || serial < _postOut) {
        return;
    }
    bool any = false;
    for (const TerrainMeshBuffer::Pointer& buffer : _uploads) {
        if (buffer->posted == _postOut) {
            buffer->resident = true;
            any = true;
        }
    }
    _postOut = 0;
    pruneUploads();
    if (any) {
        ++_meshVersion;   // what may be drawn changed: a chunk item waits for its buffer to be resident
    }
}

void RenderableTerrainEntityItem::updateCollisionShape() {
    ShapeInfo::PointList points;
    ShapeInfo::TriangleIndices indices;
    const glm::mat4 voxelToLocal = voxelToLocalMatrix();
    for (const auto& pair : _chunkMeshes) {
        // the mesh is in the chunk's own voxels, from its origin; the grid's are the entity's
        const glm::vec3 origin(pair.first.getOrigin());
        const float perVoxel = 1.0f / (float)pair.first.getScale();
        const uint32_t base = (uint32_t)points.size();
        for (const glm::vec3& position : pair.second.mesh.positions) {
            points.push_back(glm::vec3(voxelToLocal * glm::vec4(position * perVoxel + origin, 1.0f)));
        }
        for (uint32_t index : pair.second.mesh.indices) {
            indices.push_back((int32_t)(base + index));
        }
    }

    _shapeSerial = nextShapeSerial();
    ShapeInfo info;
    if (points.size() >= 3 && indices.size() >= 3) {
        // the key names this build of the shape, so the shape manager makes a new one rather than hashing the points
        info.setParams(SHAPE_TYPE_STATIC_MESH, 0.5f * getScaledDimensions(),
                       QString("terrain/%1/%2").arg(getID().toString()).arg(_shapeSerial));
        info.setPointCollection({ points });
        info.getTriangleIndices() = std::move(indices);
        adjustShapeInfoByRegistration(info);
    } else {
        info.setParams(SHAPE_TYPE_NONE, 0.5f * getScaledDimensions());
    }
    withWriteLock([&] {
        _shapeInfo = info;
        _shapeReady = true;
        _shapeStale = false;
        if (!_collisionless) {
            _flags |= Simulation::DIRTY_SHAPE | Simulation::DIRTY_MASS;
        }
    });
    if (auto tree = getTree()) {
        tree->entityChanged(getThisPointer());
    }
}

glm::vec3 RenderableTerrainEntityItem::getVoxelScale() const {
    if (!_grid.isEmpty()) {
        return getScaledDimensions() / glm::vec3(_grid.getVoxelCount());
    }
    return glm::vec3(getVoxelSize());
}

glm::mat4 RenderableTerrainEntityItem::voxelToLocalMatrix() const {
    return glm::translate(-0.5f * getScaledDimensions()) * glm::scale(getVoxelScale());
}

glm::mat4 RenderableTerrainEntityItem::voxelToWorldMatrix() const {
    bool success = false;
    const Transform toCenter = getTransformToCenter(success);
    return toCenter.getMatrix() * voxelToLocalMatrix();
}

glm::mat4 RenderableTerrainEntityItem::worldToVoxelMatrix() const {
    return glm::inverse(voxelToWorldMatrix());
}

bool RenderableTerrainEntityItem::rayHitsGround(const glm::vec3& origin, const glm::vec3& direction, float& t, glm::vec3& normal) const {
    bool hit = false;
    float best = FLT_MAX;
    const glm::vec3 invDirection = 1.0f / direction;
    for (const auto& pair : _chunkMeshes) {
        // the chunk's mesh lies within two of its voxels outside the chunk on the low side and inside it elsewhere
        const glm::vec3 chunkOrigin(pair.first.getOrigin());
        const float perVoxel = 1.0f / (float)pair.first.getScale();
        const AABox box(chunkOrigin - 2.0f * perVoxel, glm::vec3(((float)terrain::CHUNK_SIZE + 3.0f) * perVoxel));
        if (!box.contains(origin)) {
            float boxDistance = 0.0f;
            BoxFace boxFace = UNKNOWN_FACE;
            glm::vec3 boxNormal;
            if (!box.findRayIntersection(origin, direction, invDirection, boxDistance, boxFace, boxNormal) || boxDistance > best) {
                continue;
            }
        }
        const terrain::ChunkMesh& mesh = pair.second.mesh;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const glm::vec3 v0 = mesh.positions[mesh.indices[i]] * perVoxel + chunkOrigin;
            const glm::vec3 v1 = mesh.positions[mesh.indices[i + 1]] * perVoxel + chunkOrigin;
            const glm::vec3 v2 = mesh.positions[mesh.indices[i + 2]] * perVoxel + chunkOrigin;
            float triangleDistance = 0.0f;
            // from either side: a dig is seen from inside the ground's box, and a cave's ceiling from below
            if (findRayTriangleIntersection(origin, direction, v0, v1, v2, triangleDistance, true)
                    && triangleDistance >= 0.0f && triangleDistance < best) {
                best = triangleDistance;
                normal = glm::cross(v1 - v0, v2 - v0);   // counter-clockwise seen from the air: out of the ground
                hit = true;
            }
        }
    }
    if (hit) {
        t = best;
        const float length = glm::length(normal);
        normal = length > 1.0e-12f ? normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return hit;
}

bool RenderableTerrainEntityItem::findDetailedRayIntersection(const glm::vec3& origin, const glm::vec3& direction, OctreeElementPointer& element,
                                                              float& distance, BoxFace& face, glm::vec3& surfaceNormal, QVariantMap& extraInfo,
                                                              bool precisionPicking) const {
    if (_chunkMeshes.empty()) {
        return false;   // no ground yet (or none left): nothing to land on
    }
    if (!precisionPicking) {
        return true;    // the box, which the element has already met
    }
    const glm::mat4 worldToVoxel = worldToVoxelMatrix();
    const glm::vec3 voxelOrigin(worldToVoxel * glm::vec4(origin, 1.0f));
    const glm::vec3 voxelDirection(worldToVoxel * glm::vec4(direction, 0.0f));
    float t = 0.0f;
    glm::vec3 voxelNormal;
    if (!rayHitsGround(voxelOrigin, voxelDirection, t, voxelNormal)) {
        return false;
    }
    distance = t;   // the parameter along the caller's direction, as the box test measures it
    // normals go through the inverse transpose of the voxel-to-world matrix, which is the transpose of its inverse
    const glm::vec3 worldNormal(glm::transpose(worldToVoxel) * glm::vec4(voxelNormal, 0.0f));
    const float length = glm::length(worldNormal);
    surfaceNormal = length > 1.0e-12f ? worldNormal / length : glm::vec3(0.0f, 1.0f, 0.0f);
    return true;
}

void RenderableTerrainEntityItem::setUnscaledDimensions(const glm::vec3& value) {
    const glm::vec3 before = getUnscaledDimensions();
    TerrainEntityItem::setUnscaledDimensions(value);
    if (before != getUnscaledDimensions()) {
        _shapeStale = true;   // the collision mesh is in local space and stretches with the box
    }
}

ShapeType RenderableTerrainEntityItem::getShapeType() const {
    return _collisionless ? SHAPE_TYPE_NONE : SHAPE_TYPE_STATIC_MESH;
}

bool RenderableTerrainEntityItem::shouldBePhysical() const {
    // EntityItem keeps static meshes out of physics and only models opt back in; the ground must be walked on
    return !isDead() && !isLocalEntity() && getShapeType() != SHAPE_TYPE_NONE;
}

bool RenderableTerrainEntityItem::isReadyToComputeShape() const {
    if (getShapeType() == SHAPE_TYPE_NONE) {
        return true;
    }
    return resultWithReadLock<bool>([&] { return _shapeReady; });
}

void RenderableTerrainEntityItem::computeShapeInfo(ShapeInfo& info) {
    if (getShapeType() == SHAPE_TYPE_NONE) {
        info.setParams(SHAPE_TYPE_NONE, 0.5f * getScaledDimensions());
        return;
    }
    withReadLock([&] { info = _shapeInfo; });
}

// The layer set

std::mutex& TerrainLayerSet::registryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::set<TerrainLayerSet*>& TerrainLayerSet::registry() {
    static std::set<TerrainLayerSet*> sets;
    return sets;
}

TerrainLayerSet::~TerrainLayerSet() {
    std::lock_guard<std::mutex> lock(registryMutex());
    registry().erase(this);
}

void TerrainLayerSet::drawAllMapsDue(RenderArgs* args) {
    // the lock is held through the draws: a set destroyed on the main thread meanwhile waits for them
    std::lock_guard<std::mutex> lock(registryMutex());
    for (TerrainLayerSet* set : registry()) {
        set->drawMapsDue(args);
    }
}

TerrainLayerSet::TerrainLayerSet(const terrain::LayerStack& stack, const Pointer& previous) {
    auto textureCache = DependencyManager::get<TextureCache>();
    auto fetch = [&](const std::string& address, image::TextureUsage::Type type) -> NetworkTexturePointer {
        if (address.empty()) {
            return nullptr;
        }
        QUrl url(QString::fromStdString(address));
        if (url.scheme() == QStringLiteral("qrc")) {
            url = PathUtils::resourcesUrl(url.path().mid(1));   // the client's own textures, wherever its resources are
        }
        return textureCache->getTexture(url, type);
    };
    for (const terrain::Layer& spec : stack.layers) {
        if (spec.slot >= LAYER_COUNT || _layers[spec.slot].present) {
            continue;
        }
        Layer& layer = _layers[spec.slot];
        layer.spec = spec;
        layer.present = true;
        layer.texture = fetch(spec.materialURL, image::TextureUsage::ALBEDO_TEXTURE);
        layer.normalMap = fetch(spec.normalURL, image::TextureUsage::NORMAL_TEXTURE);
        layer.roughnessMap = fetch(spec.roughnessURL, image::TextureUsage::ROUGHNESS_TEXTURE);
        layer.occlusionMap = fetch(spec.occlusionURL, image::TextureUsage::OCCLUSION_TEXTURE);
        layer.heightMap = fetch(spec.heightURL, image::TextureUsage::OCCLUSION_TEXTURE);   // one linear channel, read as an occlusion map is
        _order.push_back(spec.slot);
    }
    _params = std::make_shared<gpu::Buffer>(sizeof(TerrainParams), nullptr);
    if (previous) {
        std::lock_guard<std::mutex> lock(previous->_mapsMutex);
        _albedos = previous->_albedos;
        _normals = previous->_normals;
        _masks = previous->_masks;
        _albedoSize = previous->_albedoSize;
        _albedoTargets = previous->_albedoTargets;
        _normalTargets = previous->_normalTargets;
        _maskTargets = previous->_maskTargets;
        _albedoSlices = previous->_albedoSlices;
        _normalSlices = previous->_normalSlices;
        _maskSlices = previous->_maskSlices;
        // the slices drawn from a texture stand for the slots' textures until the first render finds otherwise
        for (int slot = 0; slot < LAYER_COUNT; ++slot) {
            _layers[slot].textured = _albedoSlices[slot].drawn && _albedoSlices[slot].sources[0] != nullptr;
        }
    }
    if (!_albedos || !_normals || !_masks) {
        makeArrays();
    }
    // registered last, whole: the render thread's DrawTerrainLayerMaps walks the registry at the start of every
    // frame and draws the slices due, and a set registered before its targets were made crashed it on arrival
    // (10-20: batch.setFramebuffer on a target vector still empty)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().insert(this);
    }
}

gpu::TexturePointer TerrainLayerSet::makeArray(const char* name, const gpu::Element& format, int size,
                                               std::vector<gpu::FramebufferPointer>& targets) {
    gpu::Sampler::Desc samplerDesc;
    samplerDesc._filter = gpu::Sampler::FILTER_ANISOTROPIC;
    samplerDesc._wrapModeU = gpu::Sampler::WRAP_REPEAT;
    samplerDesc._wrapModeV = gpu::Sampler::WRAP_REPEAT;
    samplerDesc._maxAnisotropy = 8;
    // drawn into, with mips for the ground further off
    auto array = gpu::Texture::createRenderBufferArray(format, (uint16_t)size, (uint16_t)size, LAYER_COUNT,
                                                       gpu::Texture::MAX_NUM_MIPS, gpu::Sampler(samplerDesc));
    array->setSource(name);
    targets.clear();
    for (int slice = 0; slice < LAYER_COUNT; ++slice) {
        auto framebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create(name));
        framebuffer->setRenderBuffer(0, array, slice);
        targets.push_back(framebuffer);
    }
    return array;
}

void TerrainLayerSet::makeArrays() {
    // linear 8 bits a channel for the normals and the masks
    _normals = makeArray("terrain layer normals", gpu::Element::COLOR_RGBA_32, MAP_SIZE, _normalTargets);
    _masks = makeArray("terrain layer masks", gpu::Element::COLOR_RGBA_32, MAP_SIZE, _maskTargets);
    _normalSlices = {};
    _maskSlices = {};
    makeAlbedoArray(ALBEDO_MIN_SIZE);
}

void TerrainLayerSet::makeAlbedoArray(int size) {
    // sRGB, like the textures
    _albedos = makeArray("terrain layer colours", gpu::Element::COLOR_SRGBA_32, size, _albedoTargets);
    _albedoSize = size;
    _albedoSlices = {};
    for (Layer& layer : _layers) {
        layer.textured = false;
    }
    _paramsDirty = true;
}

TerrainLayerSet::Stamps TerrainLayerSet::stampsOf(const Sources& sources) {
    Stamps stamps {};
    for (size_t i = 0; i < sources.size(); ++i) {
        stamps[i] = sources[i] ? sources[i]->getResidencyStamp() : 0;
    }
    return stamps;
}

TerrainLayerSet::Sources TerrainLayerSet::albedoSources(int slot) const {
    const Layer& layer = _layers[slot];
    return { layer.texture ? layer.texture->getGPUTexture() : nullptr, nullptr, nullptr };
}

TerrainLayerSet::Sources TerrainLayerSet::normalSources(int slot) const {
    const Layer& layer = _layers[slot];
    return { layer.normalMap ? layer.normalMap->getGPUTexture() : nullptr, nullptr, nullptr };
}

TerrainLayerSet::Sources TerrainLayerSet::maskSources(int slot) const {
    const Layer& layer = _layers[slot];
    return { layer.roughnessMap ? layer.roughnessMap->getGPUTexture() : nullptr,
             layer.occlusionMap ? layer.occlusionMap->getGPUTexture() : nullptr,
             layer.heightMap ? layer.heightMap->getGPUTexture() : nullptr };
}

void TerrainLayerSet::drawMapsDue(RenderArgs* args) {
    struct Due {
        int slot;
        Map map;
        Sources sources;
    };
    std::vector<Due> due;
    std::vector<gpu::TexturePointer> toMake;   // sources the backend has yet to make, bound so that it does
    {
        std::lock_guard<std::mutex> lock(_mapsMutex);
        // the colour array grows to the largest colour texture, within its bounds; its slices are then drawn again
        int wanted = _albedoSize;
        for (int slot = 0; slot < LAYER_COUNT; ++slot) {
            const auto texture = albedoSources(slot)[0];
            if (texture) {
                const auto dimensions = texture->getDimensions();
                const int side = (int)std::max(dimensions.x, dimensions.y);
                while (wanted < side && wanted < ALBEDO_MAX_SIZE) {
                    wanted *= 2;
                }
            }
        }
        if (wanted != _albedoSize) {
            makeAlbedoArray(wanted);
        }
        // a slice is drawn when it has not been, when its sources change, and when more of a source has reached the
        // GPU since (its residency stamp): a few draws per texture as its mips come in, smallest first. Not while a
        // source has yet to reach the GPU at all (no stamp yet): the GL 4.5 backend (Windows, Linux) makes two new
        // textures a frame and binds the others' fallbacks meanwhile (white, for a colour texture), and a slice drawn
        // from a fallback was never drawn again, since only a texture the backend has made gets a new stamp and
        // nothing else binds a layer's texture: Darkside's ground came out white on Windows (2026-10-28), its
        // twenty maps all asked for in its first frame. Such a source is bound instead, so the backend makes it, and
        // its slice is drawn once it has; the layer keeps its plain colour, or the slice what it had, until then.
        // (The macOS GL 4.1 backend makes every texture as it is first bound.)
        auto waiting = [](const Sources& sources) {
            for (const auto& source : sources) {
                if (source && source->getUsageType() == gpu::TextureUsageType::RESOURCE && source->getResidencyStamp() == 0) {
                    return true;
                }
            }
            return false;
        };
        auto check = [&](Map map, Slices& slices, int slot, const Sources& sources) {
            Slice& slice = slices[slot];
            const Stamps stamps = stampsOf(sources);
            if (!slice.drawn || slice.sources != sources || slice.stamps != stamps) {
                if (waiting(sources)) {
                    for (const auto& source : sources) {
                        if (source) {
                            toMake.push_back(source);
                        }
                    }
                    return;
                }
                slice.drawn = true;
                slice.sources = sources;
                slice.stamps = stamps;
                due.push_back({ slot, map, sources });
            }
        };
        for (int slot = 0; slot < LAYER_COUNT; ++slot) {
            check(Map::Albedo, _albedoSlices, slot, albedoSources(slot));
            check(Map::Normal, _normalSlices, slot, normalSources(slot));
            check(Map::Mask, _maskSlices, slot, maskSources(slot));
            const bool textured = _albedoSlices[slot].sources[0] != nullptr;   // the plain colour until the texture has arrived
            if (textured != _layers[slot].textured) {
                _layers[slot].textured = textured;
                _paramsDirty = true;
            }
        }
    }
    if ((due.empty() && toMake.empty()) || !args || !args->_context) {
        return;
    }
    if (!due.empty()) {
        _paramsDirty = true;   // the flags say which slices are drawn
    }

    auto white = DependencyManager::get<TextureCache>()->getWhiteTexture();
    // a batch of its own, appended to the frame now: at the start of the frame (DrawTerrainLayerMaps), before any
    // pass has bound its framebuffer, since the slice's stays bound when this batch ends
    gpu::doInBatch("TerrainLayerSet::drawMapsDue", args->_context, [&](gpu::Batch& batch) {
        // binding a texture is what has the backend make it
        for (const auto& texture : toMake) {
            batch.setResourceTexture(0, texture);
        }
        if (due.empty()) {
            batch.setResourceTexture(0, nullptr);
            return;
        }
        batch.enableStereo(false);
        batch.setPipeline(mapPipeline());
        bool drawn[3] = { false, false, false };
        for (const Due& d : due) {
            TerrainMapParams params;
            params.which = glm::ivec4((int)d.map, d.sources[0] ? 1 : 0, d.sources[1] ? 1 : 0, d.sources[2] ? 1 : 0);
            params.plain = glm::vec4(PLAIN_ROUGHNESS, PLAIN_OCCLUSION, PLAIN_HEIGHT, 0.0f);
            // a buffer per draw: the batch reads its buffers when it runs, so one shared would hold the last draw's
            auto buffer = std::make_shared<gpu::Buffer>(sizeof(TerrainMapParams), (const gpu::Byte*)&params);
            batch.setUniformBuffer(ENTITIES_BUFFER_TERRAIN_MAPS, buffer, 0, sizeof(TerrainMapParams));
            const int size = d.map == Map::Albedo ? _albedoSize : MAP_SIZE;
            batch.setViewportTransform(glm::ivec4(0, 0, size, size));
            batch.setFramebuffer(d.map == Map::Albedo ? _albedoTargets[d.slot]
                                                      : d.map == Map::Mask ? _maskTargets[d.slot] : _normalTargets[d.slot]);
            for (int i = 0; i < 3; ++i) {
                batch.setResourceTexture(i, d.sources[i] ? d.sources[i] : white);
            }
            batch.draw(gpu::TRIANGLE_STRIP, 4);
            drawn[(int)d.map] = true;
        }
        for (int i = 0; i < 3; ++i) {
            batch.setResourceTexture(i, nullptr);
        }
        if (drawn[(int)Map::Albedo]) {
            batch.generateTextureMips(_albedos);
        }
        if (drawn[(int)Map::Normal]) {
            batch.generateTextureMips(_normals);
        }
        if (drawn[(int)Map::Mask]) {
            batch.generateTextureMips(_masks);
        }
    });
}

void TerrainLayerSet::setWorldToTerrain(const glm::mat4& worldToTerrain) {
    if (_worldToTerrain != worldToTerrain) {
        _worldToTerrain = worldToTerrain;
        _paramsDirty = true;
    }
}

void TerrainLayerSet::updateParams() {
    TerrainParams params;
    params.worldToTerrain = _worldToTerrain;
    for (int i = 0; i < LAYER_COUNT; ++i) {
        const Layer& layer = _layers[i];
        const terrain::Layer& spec = layer.spec;
        const float repeats = 1.0f / std::max(spec.tiling, 0.01f);
        params.layers[i].color = glm::vec4(layer.present ? spec.color : LAYER_PALETTE[i], layer.textured ? 1.0f : 0.0f);
        params.layers[i].tiling = glm::vec4(repeats, spec.offset.x * repeats, spec.offset.y * repeats, glm::clamp(spec.opacity, 0.0f, 1.0f));
        params.layers[i].tint = glm::vec4(layer.present ? spec.tint : glm::vec3(1.0f), spec.normalStrength);
        const bool normalMapped = spec.useNormal && _normalSlices[i].drawn && _normalSlices[i].sources[0] != nullptr;
        params.layers[i].flags = glm::ivec4(spec.visible ? 1 : 0, (int)spec.blend, normalMapped ? 1 : 0, _maskSlices[i].drawn ? 1 : 0);
    }
    for (int i = 0; i < 2 * 4; ++i) {
        params.order[i / 4][i % 4] = i < (int)_order.size() ? (int)_order[i] : -1;
    }
    _params->setSubData(0, params);
    _paramsDirty = false;
}

void TerrainLayerSet::bind(gpu::Batch& batch) {
    batch.setResourceTexture(ENTITIES_TEXTURE_TERRAIN_LAYERS, _albedos);
    batch.setResourceTexture(ENTITIES_TEXTURE_TERRAIN_NORMALS, _normals);
    batch.setResourceTexture(ENTITIES_TEXTURE_TERRAIN_MASKS, _masks);
    if (_paramsDirty) {
        updateParams();
    }
    batch.setUniformBuffer(ENTITIES_BUFFER_TERRAIN_LAYERS, _params, 0, sizeof(TerrainParams));
}

// The paint tile

TerrainPaintTile::Pointer TerrainPaintTile::make(const terrain::PaintMap& paint, int level, const glm::ivec2& column) {
    if (paint.isEmpty()) {
        return nullptr;
    }
    std::vector<uint8_t> layers0to3, layers4to7;
    paint.fillTile(level, column, layers0to3, layers4to7);
    auto tile = std::make_shared<TerrainPaintTile>();
    tile->layers0to3 = makeTileTexture(paint.getTileSize(), layers0to3, "terrain paint 0-3");
    tile->layers4to7 = makeTileTexture(paint.getTileSize(), layers4to7, "terrain paint 4-7");
    return tile;
}

// The chunk payload

gpu::BufferPointer TerrainChunkPayload::makeVertexBuffer(const terrain::ChunkMesh& mesh) {
    // de-indexed: a triangle's three vertices share its three materials and differ in their corner
    std::vector<TerrainVertex> vertices(mesh.indices.size());
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t materials = (uint32_t)mesh.materials[mesh.indices[i]]
                                 | ((uint32_t)mesh.materials[mesh.indices[i + 1]] << 8)
                                 | ((uint32_t)mesh.materials[mesh.indices[i + 2]] << 16);
        for (uint32_t corner = 0; corner < 3; ++corner) {
            const uint32_t index = mesh.indices[i + corner];
            TerrainVertex& vertex = vertices[i + corner];
            vertex.position = mesh.positions[index];
            vertex.normal = glm_packSnorm3x10_1x2(glm::vec4(mesh.normals[index], 0.0f));
            vertex.materials = materials | (corner << 24);
        }
    }
    return std::make_shared<gpu::Buffer>(vertices.size() * sizeof(TerrainVertex), (const gpu::Byte*)vertices.data());
}

TerrainMeshBuffer::Pointer TerrainMeshBuffer::make(const terrain::ChunkMesh& mesh) {
    if (mesh.isEmpty()) {
        return nullptr;
    }
    auto made = std::make_shared<TerrainMeshBuffer>();
    made->buffer = TerrainChunkPayload::makeVertexBuffer(mesh);
    made->vertexCount = TerrainChunkPayload::getVertexCount(mesh);
    return made;
}

void TerrainChunkPayload::setBuffers(const gpu::BufferPointer& vertices, uint32_t vertexCount) {
    _vertexBuffer = vertices;
    _vertexCount = vertexCount;
    _debugDrawn = false;
}

void TerrainChunkPayload::setWorldToTerrain(const glm::mat4& worldToTerrain) {
    if (_layers) {
        _layers->setWorldToTerrain(worldToTerrain);
    }
}

void TerrainChunkPayload::setPlacement(const Transform& chunkToWorld) {
    _transform = chunkToWorld;
    // The mesh's vertices lie within two voxels outside the chunk on the low side (SurfaceNets.h) and inside it elsewhere.
    AABox bound(glm::vec3(-2.0f), glm::vec3((float)terrain::CHUNK_SIZE + 2.0f));
    bound.transform(chunkToWorld);
    _bound = bound;
    _debugDrawn = false;
}

void TerrainChunkPayload::setChunk(const terrain::ChunkKey& chunk) {
    _debugKey = chunk;
    // A tile is CHUNK_SIZE voxels of its level with the apron around them (PaintMap::fillTile), tile coordinates 0
    // to 1 across. A node at level 0 or coarser has a tile of its own column; a finer chunk's voxels are a fraction
    // of its level-0 column's and start part way in.
    const float perTileVoxel = 1.0f / (float)(terrain::CHUNK_SIZE + 2 * terrain::PAINT_TILE_APRON);
    const glm::ivec3 inColumn = chunk.level > 0 ? terrain::VoxelGrid::localOf(chunk.getOrigin()) : glm::ivec3(0);
    // The Height blend decides between layers by their heights at an edge 0.2 tall; a coarse node's tile is an
    // average of the map's texels, and the edge would draw their lattice, so there the layers mix plainly instead.
    TerrainChunkParams params;
    params.paint = glm::vec4(perTileVoxel * std::min(chunk.getVoxelSize(), 1.0f),
                             (float)(terrain::PAINT_TILE_APRON + inColumn.x) * perTileVoxel,
                             (float)(terrain::PAINT_TILE_APRON + inColumn.z) * perTileVoxel, chunk.level < 0 ? 0.0f : 1.0f);
    // The top surface's tile has a texel a grid voxel, each the height at the grid's column through its middle
    // (TopSurface::fillTile), so a point reads its tile half a texel on, between the columns as heightAt() does. A
    // coarse node draws the top alone: it paints wherever its ground faces up, as before.
    const float perTopTexel = 1.0f / (float)terrain::TopSurface::getTileSize();
    params.top = glm::vec4(perTopTexel * std::min(chunk.getVoxelSize(), 1.0f),
                           ((float)(terrain::PAINT_TILE_APRON + inColumn.x) + 0.5f) * perTopTexel,
                           ((float)(terrain::PAINT_TILE_APRON + inColumn.z) + 0.5f) * perTopTexel, chunk.level < 0 ? 0.0f : 1.0f);
    params.vertical = glm::vec4(std::min(chunk.getVoxelSize(), 1.0f), (float)chunk.getOrigin().y, 0.0f, 0.0f);
    if (!_chunkParams) {
        _chunkParams = std::make_shared<gpu::Buffer>(sizeof(TerrainChunkParams), (const gpu::Byte*)&params);
    } else {
        _chunkParams->setSubData(0, params);
    }
}

ShapeKey TerrainChunkPayload::getShapeKey() const {
    ShapeKey::Builder builder = ShapeKey::Builder().withCustom(CUSTOM_PIPELINE_NUMBER);
    if (_wireframe) {
        builder.withWireframe();
    }
    return builder.build();
}

void TerrainChunkPayload::render(RenderArgs* args) {
    if (!_vertexBuffer || _vertexCount == 0 || !_layers || !_chunkParams) {
        if (terrainDebug && !_debugDrawn) {
            _debugDrawn = true;
            qCDebug(entitiesrenderer) << "Terrain chunk item" << describeKey(_debugKey) << "cannot draw: buffer" << (_vertexBuffer != nullptr)
                                      << "vertices" << _vertexCount << "layers" << (_layers != nullptr) << "params" << (_chunkParams != nullptr);
        }
        return;
    }
    if (terrainDebug && !_debugDrawn && args->_renderMode == RenderArgs::DEFAULT_RENDER_MODE) {
        _debugDrawn = true;
        const glm::vec3 low = _bound.getMinimumPoint();
        const glm::vec3 high = _bound.getMaximumPoint();
        qCDebug(entitiesrenderer) << "Terrain chunk item" << describeKey(_debugKey) << "draws" << _vertexCount / 3 << "triangles; bound"
                                  << low.x << low.y << low.z << "to" << high.x << high.y << high.z << "; at"
                                  << _transform.getTranslation().x << _transform.getTranslation().y << _transform.getTranslation().z
                                  << "scale" << _transform.getScale().x << _transform.getScale().y << _transform.getScale().z;
    }
    // the layer maps due this frame were drawn at its start (DrawTerrainLayerMaps), never from here: a batch slipped
    // in before this pass left the slice's framebuffer bound for it, and the frame came out black (2026-10-19)
    gpu::Batch& batch = *args->_batch;
    batch.setModelTransform(_transform);
    batch.setInputFormat(_vertexFormat);
    batch.setInputBuffer(0, _vertexBuffer, 0, sizeof(TerrainVertex));
    _layers->bind(batch);   // the shadow pass binds them too, and ignores them
    batch.setUniformBuffer(ENTITIES_BUFFER_TERRAIN_CHUNK, _chunkParams, 0, sizeof(TerrainChunkParams));
    batch.setResourceTexture(ENTITIES_TEXTURE_TERRAIN_PAINT0, _paintTile ? _paintTile->layers0to3 : _unpaintedTile);
    batch.setResourceTexture(ENTITIES_TEXTURE_TERRAIN_PAINT1, _paintTile ? _paintTile->layers4to7 : _unpaintedTile);
    batch.setResourceTexture(ENTITIES_TEXTURE_TERRAIN_TOP, _topTile ? _topTile : _noTopTile);
    batch.draw(gpu::TRIANGLES, _vertexCount, 0);
}

void TerrainUploadPayload::render(RenderArgs* args) {
    if (_buffers.empty() || !args || !args->_batch) {
        return;
    }
    // referenced, not drawn: the backend uploads every buffer a batch holds as the batch executes (the transfer
    // pass), and a chunk item that draws the buffer later finds it there
    gpu::Batch& batch = *args->_batch;
    for (const gpu::BufferPointer& buffer : _buffers) {
        batch.setInputBuffer(0, buffer, 0, sizeof(TerrainVertex));
    }
    uint64_t fed = _fed->load(std::memory_order_relaxed);
    while (fed < _serial && !_fed->compare_exchange_weak(fed, _serial, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

namespace render {
template <> const ItemKey payloadGetKey(const TerrainChunkPayload::Pointer& payload) {
    return payload ? payload->getKey() : ItemKey::Builder::opaqueShape().build();
}
template <> const Item::Bound payloadGetBound(const TerrainChunkPayload::Pointer& payload) {
    return payload ? payload->getBound() : Item::Bound();
}
template <> const ShapeKey shapeGetShapeKey(const TerrainChunkPayload::Pointer& payload) {
    return payload ? payload->getShapeKey() : ShapeKey::Builder::invalid();
}
template <> void payloadRender(const TerrainChunkPayload::Pointer& payload, RenderArgs* args) {
    if (payload) {
        payload->render(args);
    }
}
template <> const ItemKey payloadGetKey(const TerrainUploadPayload::Pointer& payload) {
    return payload ? payload->getKey() : ItemKey::Builder::opaqueShape().build();
}
template <> const Item::Bound payloadGetBound(const TerrainUploadPayload::Pointer& payload) {
    return payload ? payload->getBound() : Item::Bound();
}
template <> const ShapeKey shapeGetShapeKey(const TerrainUploadPayload::Pointer& payload) {
    return ShapeKey::Builder().withOwnPipeline();   // it binds no pipeline and draws nothing: its render() runs as it is
}
template <> void payloadRender(const TerrainUploadPayload::Pointer& payload, RenderArgs* args) {
    if (payload) {
        payload->render(args);
    }
}
}

// The renderer

TerrainEntityRenderer::TerrainEntityRenderer(const EntityItemPointer& entity) : Parent(entity) {
    initTerrainPipelines();
}

TerrainEntityRenderer::~TerrainEntityRenderer() {
    dropRequest();
}

ItemKey TerrainEntityRenderer::getKey() {
    // this item only groups the chunks, which are the shapes
    ItemKey::Builder builder = ItemKey::Builder().withTypeMeta().withTagBits(getTagMask()).withLayer(getHifiRenderLayer());
    if (!_visible) {
        builder.withInvisible();
    }
    return builder;
}

uint32_t TerrainEntityRenderer::metaFetchMetaSubItems(ItemIDs& subItems) const {
    std::lock_guard<std::mutex> lock(_subItemsMutex);
    subItems.insert(subItems.end(), _subItems.begin(), _subItems.end());
    return (uint32_t)_subItems.size();
}

void TerrainEntityRenderer::handOverSubItems() {
    ItemIDs ids;
    ids.reserve(_chunkItems.size());
    for (const auto& pair : _chunkItems) {
        ids.push_back(pair.second.id);
    }
    std::lock_guard<std::mutex> lock(_subItemsMutex);
    _pendingSubItems = std::move(ids);
    _subItemsPending = true;
}

void TerrainEntityRenderer::doRenderUpdateAsynchronousTyped(const TypedEntityPointer& entity) {
    // On the render thread, as this frame's transaction goes through: its new chunk items are in the scene by now
    // (a transaction's resets come before its updates).
    std::lock_guard<std::mutex> lock(_subItemsMutex);
    if (_subItemsPending) {
        _subItems.swap(_pendingSubItems);
        _subItemsPending = false;
    }
}

void TerrainEntityRenderer::removeFromScene(const ScenePointer& scene, Transaction& transaction) {
    for (const auto& pair : _chunkItems) {
        transaction.removeItem(pair.second.id);
    }
    _chunkItems.clear();
    if (_uploadItem != Item::INVALID_ITEM_ID) {
        transaction.removeItem(_uploadItem);
        _uploadItem = Item::INVALID_ITEM_ID;
    }
    {
        std::lock_guard<std::mutex> lock(_subItemsMutex);
        _pendingSubItems.clear();
        _subItems.clear();
        _subItemsPending = false;
    }
    Parent::removeFromScene(scene, transaction);
}

bool TerrainEntityRenderer::needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const {
    if (!entity->hasGridSource() || entity->getTerrainURL() != entity->getGridURL() || entity->getTerrainVersion() != entity->getGridVersion()) {
        return true;
    }
    if (!entity->meshesAreSettled() || entity->collisionShapeIsStale()) {
        return true;
    }
    if (entity->getDrawnVersion() != _drawnVersion) {
        return true;
    }
    if (entity->getPaintVersion() != _paintVersion) {
        return true;
    }
    if (entity->getVoxelScale() != _lastVoxelScale) {
        return true;
    }
    if (!_layerSetMade || entity->getLayersVersion() != _lastLayersVersion) {
        return true;
    }
    if (!entity->layersAreFixed() && entity->getTerrainLayers() != _lastLegacyLayersJSON) {
        return true;
    }
    if ((_primitiveMode == PrimitiveMode::LINES) != _lastWireframe) {
        return true;
    }
    return false;
}

void TerrainEntityRenderer::postUploads(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) {
    std::vector<TerrainMeshBuffer::Pointer> due;
    uint64_t serial = 0;
    const bool arriving = _firstDrawn == std::chrono::steady_clock::time_point() ||
        std::chrono::duration<float>(std::chrono::steady_clock::now() - _firstDrawn).count() < LOD_UPLOAD_ARRIVAL_SECONDS;
    entity->getUploadsDue(arriving ? LOD_UPLOAD_BYTES_ARRIVAL : LOD_UPLOAD_BYTES_MAX, due, serial);
    if (serial == _uploadsPosted) {
        return;   // the same share as last time (not through yet), or nothing due
    }
    _uploadsPosted = serial;
    std::vector<gpu::BufferPointer> buffers;
    buffers.reserve(due.size());
    for (const TerrainMeshBuffer::Pointer& buffer : due) {
        buffers.push_back(buffer->buffer);
    }
    ItemKey::Builder keyBuilder = ItemKey::Builder::opaqueShape().withTagBits(getTagMask()).withLayer(getHifiRenderLayer()).withShadowCaster();
    if (!_visible) {
        keyBuilder.withInvisible();
    }
    const ItemKey key = keyBuilder.build();
    const Item::Bound bound = getBound();
    if (_uploadItem == Item::INVALID_ITEM_ID) {
        auto payload = std::make_shared<TerrainUploadPayload>(_uploadsFed);
        payload->setBuffers(std::move(buffers), serial);
        payload->setKey(key);
        payload->setBound(bound);
        _uploadItem = scene->allocateID();
        transaction.resetItem(_uploadItem, std::make_shared<Payload<TerrainUploadPayload>>(payload));
        return;
    }
    transaction.updateItem<TerrainUploadPayload>(_uploadItem, [buffers, serial, key, bound](TerrainUploadPayload& payload) {
        payload.setBuffers(buffers, serial);
        payload.setKey(key);
        payload.setBound(bound);
    });
}

void TerrainEntityRenderer::doRenderUpdateSynchronousTyped(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) {
    // The voxels: the file the entity points at, or flat ground while it points at none.
    const QString url = entity->getTerrainURL();
    const uint32_t version = entity->getTerrainVersion();
    if (!entity->hasGridSource() || url != entity->getGridURL() || version != entity->getGridVersion()) {
        if (url.isEmpty()) {
            dropRequest();
            entity->makeDefaultGrid();
            entity->setGridSource(url, version);
        } else if (!_request || _requestURL != url || _requestVersion != version) {
            fetchTerrain(url, version);
        }
    }

    // The meshes, which come back from the mesher's threads over a few frames, and then the collision shape, once
    // they have all come: a grid without ground (none, or cleared) settles at once and gets no shape.
    entity->updateMeshes();
    if (entity->collisionShapeIsStale() && entity->meshesAreSettled()) {
        entity->updateCollisionShape();
    }

    // What to draw for the camera: the chunks' own meshes near it, coarser nodes' further away. The camera moves
    // without a word to the entity, so the tree renderer, which only comes back when asked, is asked every frame.
    if (auto viewState = AbstractViewStateInterface::instance()) {
        ViewFrustum view;
        viewState->copyCurrentViewFrustum(view);
        entity->updateLevelOfDetail(view.getPosition());
    }
    emit requestRenderUpdate();

    // The layers: what the voxel materials look like, and how the paint map draws over them. A terrain whose file
    // holds no layers follows the entity's terrainLayers property. A changed stack makes a new set; the textures
    // come in on their own time and the set binds them as they do.
    bool layersChanged = false;
    if (!entity->layersAreFixed()) {
        const QString legacy = entity->getTerrainLayers();
        if (!_layerSetMade || legacy != _lastLegacyLayersJSON) {
            _lastLegacyLayersJSON = legacy;
            entity->setLayers(entity->legacyOrDefaultLayers(), false);
        }
    }
    if (!_layerSetMade || entity->getLayersVersion() != _lastLayersVersion) {
        _layerSet = std::make_shared<TerrainLayerSet>(entity->getLayers(), _layerSet);
        _lastLayersVersion = entity->getLayersVersion();
        _layerSetMade = true;
        layersChanged = true;
    }

    // The paint map: the columns it marks dirty lose their tiles, and so do the coarser nodes' tiles over them; the
    // items below get anew the tiles they lost.
    terrain::PaintMap& paint = entity->getPaint();
    const terrain::ColumnSet dirtyColumns = paint.getDirtyColumns();
    paint.clearDirtyColumns();
    _paintVersion = entity->getPaintVersion();
    const glm::ivec2 columnCount = paint.getColumnCount();
    std::set<glm::ivec3, terrain::IVec3Less> lostTiles;
    bool allTilesLost = false;
    if (paint.isEmpty() || (!dirtyColumns.empty() && (int)dirtyColumns.size() >= columnCount.x * columnCount.y)) {
        _paintTiles.clear();   // a new map (a resample may have changed its columns): every tile is remade
        allTilesLost = true;
    } else if (!dirtyColumns.empty()) {
        for (auto it = _paintTiles.begin(); it != _paintTiles.end();) {
            const glm::ivec3& tile = it->first;
            bool lost = false;
            if (tile.z == 0) {
                lost = dirtyColumns.count(glm::ivec2(tile.x, tile.y)) > 0;
            } else {
                // a coarser node's tile covers 2^-level level-0 columns a side, and its apron reaches past them
                const int columns = 1 << -tile.z;
                const int margin = (terrain::PAINT_TILE_APRON * columns + terrain::CHUNK_SIZE - 1) / terrain::CHUNK_SIZE;
                const glm::ivec2 low = glm::ivec2(tile.x, tile.y) * columns - margin;
                const glm::ivec2 high = (glm::ivec2(tile.x, tile.y) + 1) * columns - 1 + margin;
                for (const glm::ivec2& column : dirtyColumns) {
                    if (glm::all(glm::greaterThanEqual(column, low)) && glm::all(glm::lessThanEqual(column, high))) {
                        lost = true;
                        break;
                    }
                }
            }
            if (lost) {
                lostTiles.insert(tile);
                it = _paintTiles.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The ground the sky sees: its heights follow the grid's edits, and the columns whose heights moved lose their
    // tiles, as the paint map's dirty columns do.
    entity->updateTopSurface();
    terrain::TopSurface& top = entity->getTopSurface();
    terrain::ColumnSet lostTopTiles;
    bool allTopTilesLost = false;
    if (top.allColumnsDirty() || top.isEmpty()) {
        allTopTilesLost = top.allColumnsDirty() || !_topTiles.empty();
        top.clearAllColumnsDirty();
        top.clearDirtyColumns();
        _topTiles.clear();
    } else if (!top.getDirtyColumns().empty()) {
        for (const glm::ivec2& column : top.getDirtyColumns()) {
            if (_topTiles.erase(column) > 0) {
                lostTopTiles.insert(column);
            }
        }
        top.clearDirtyColumns();
    }

    // The chunk items, one for each mesh drawn: nothing to do on most frames.
    const Transform& model = getModelTransform();
    const glm::vec3 dimensions = entity->getScaledDimensions();
    const glm::vec3 voxelScale = entity->getVoxelScale();
    const bool wireframe = _primitiveMode == PrimitiveMode::LINES;
    const ItemKey key = chunkKey();
    const bool placementChanged = model != _lastTransform || dimensions != _lastDimensions || voxelScale != _lastVoxelScale;
    const bool keyChanged = key != _lastChunkKey || wireframe != _lastWireframe;
    // The vertex buffers on their way to the backend (entry 24 step 3b), every frame: what the upload item's batch
    // has referenced is resident now (which may change the drawn version, so this comes before it is read), and the
    // next share of the queue goes to the item.
    entity->uploadsFed(_uploadsFed->load(std::memory_order_acquire));
    postUploads(scene, transaction, entity);
    const uint64_t drawnVersion = entity->getDrawnVersion();
    if (_drawnOnce && drawnVersion == _drawnVersion && !placementChanged && !keyChanged && !layersChanged && lostTiles.empty() &&
            !allTilesLost && lostTopTiles.empty() && !allTopTilesLost) {
        return;
    }
    _drawnOnce = true;
    _drawnVersion = drawnVersion;
    _lastTransform = model;
    _lastDimensions = dimensions;
    _lastVoxelScale = voxelScale;
    _lastWireframe = wireframe;
    _lastChunkKey = key;
    const glm::mat4 worldToTerrain = glm::inverse(model.getMatrix());
    const TerrainLayerSet::Pointer layerSet = _layerSet;

    // the section's own time and the buffers it takes ready or, failing one, builds on this thread, for the level
    // of detail's diag line (a mesh made on the pool comes with its buffer, entry 24 step 3a: built here should read 0)
    const auto drawnStart = std::chrono::steady_clock::now();
    int buffersTaken = 0;
    int buffersBuilt = 0;
    int itemsWaiting = 0;   // new items not made yet: their buffer is still on its way to the backend
    double buildMs = 0.0;
    uint64_t buildBytes = 0;
    auto buildVertexBuffer = [&](const terrain::ChunkMesh& mesh) {
        const auto start = std::chrono::steady_clock::now();
        gpu::BufferPointer buffer = TerrainChunkPayload::makeVertexBuffer(mesh);
        buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++buffersBuilt;
        buildBytes += buffer->getSize();
        return buffer;
    };
    // the chunk's buffer, ready from the pool, or built here when the mesh came without one
    auto takeVertexBuffer = [&](const RenderableTerrainEntityItem::DrawnChunk& chunk, uint32_t& vertexCount) {
        if (chunk.buffer) {
            ++buffersTaken;
            vertexCount = chunk.buffer->vertexCount;
            return chunk.buffer->buffer;
        }
        vertexCount = TerrainChunkPayload::getVertexCount(*chunk.mesh);
        return buildVertexBuffer(*chunk.mesh);
    };
    std::vector<RenderableTerrainEntityItem::DrawnChunk> drawn;
    entity->getDrawnChunks(drawn);
    std::set<terrain::ChunkKey> drawnKeys;
    for (const RenderableTerrainEntityItem::DrawnChunk& chunk : drawn) {
        drawnKeys.insert(chunk.key);
    }
    int itemsRemoved = 0;
    int itemsMade = 0;
    int itemsUpdated = 0;
    QStringList removedKeys;
    for (auto it = _chunkItems.begin(); it != _chunkItems.end();) {
        if (drawnKeys.count(it->first)) {
            ++it;
        } else {
            transaction.removeItem(it->second.id);
            if (terrainDebug) {
                removedKeys << describeKey(it->first);
            }
            it = _chunkItems.erase(it);
            ++itemsRemoved;
        }
    }
    for (const RenderableTerrainEntityItem::DrawnChunk& chunk : drawn) {
        const Transform placement = chunkToWorld(model, dimensions, voxelScale, chunk.key);
        const glm::ivec3 tileKey = paintTileKey(chunk.key);
        const TerrainPaintTile::Pointer paintTile = paintTileFor(paint, tileKey);
        const gpu::TexturePointer topTile = topTileFor(top, chunk.key);
        auto found = _chunkItems.find(chunk.key);
        if (found == _chunkItems.end()) {
            // A new item waits for its buffer to be resident (step 3b: the first cut, and a cut forced through a
            // stall, come node by node as the uploads land; a cut shown in the usual way has every buffer resident
            // already). An item that exists takes a new mesh at once: a chunk under the brush must not blink.
            if (chunk.buffer && !chunk.buffer->resident) {
                ++itemsWaiting;   // marked wanted by the entity's update (the drawn set's pass), not here
                continue;
            }
            uint32_t vertexCount = 0;
            const gpu::BufferPointer vertices = takeVertexBuffer(chunk, vertexCount);
            auto payload = std::make_shared<TerrainChunkPayload>();
            payload->setBuffers(vertices, vertexCount);
            payload->setPlacement(placement);
            payload->setChunk(chunk.key);
            payload->setKey(key);
            payload->setWireframe(wireframe);
            payload->setLayers(layerSet);
            payload->setWorldToTerrain(worldToTerrain);
            payload->setPaintTile(paintTile);
            payload->setTopTile(topTile);
            ChunkItem item;
            item.id = scene->allocateID();
            item.meshVersion = chunk.version;
            transaction.resetItem(item.id, std::make_shared<Payload<TerrainChunkPayload>>(payload));
            _chunkItems[chunk.key] = item;
            ++itemsMade;
            if (terrainDebug) {
                qCDebug(entitiesrenderer) << "Terrain" << entity->getID() << "item" << item.id << "made for" << describeKey(chunk.key)
                                          << "with" << TerrainChunkPayload::getVertexCount(*chunk.mesh) / 3 << "triangles, mesh version" << chunk.version;
            }
            continue;
        }
        const bool meshChanged = found->second.meshVersion != chunk.version;
        const bool paintChanged = allTilesLost || lostTiles.count(tileKey) > 0;
        const bool topChanged = allTopTilesLost ||
            (chunk.key.level >= 0 && lostTopTiles.count(glm::ivec2(chunk.key.getSlot().x, chunk.key.getSlot().z)) > 0);
        if (!meshChanged && !placementChanged && !keyChanged && !layersChanged && !paintChanged && !topChanged) {
            continue;
        }
        found->second.meshVersion = chunk.version;
        ++itemsUpdated;
        // The buffer comes ready from the pool with the mesh (or is built here, failing one) and is handed over: the
        // update runs later on the render thread.
        uint32_t vertexCount = TerrainChunkPayload::getVertexCount(*chunk.mesh);
        gpu::BufferPointer vertices = meshChanged ? takeVertexBuffer(chunk, vertexCount) : nullptr;
        if (terrainDebug && meshChanged) {
            qCDebug(entitiesrenderer) << "Terrain" << entity->getID() << "item" << found->second.id << "of" << describeKey(chunk.key)
                                      << "gets a new mesh of" << vertexCount / 3 << "triangles, mesh version" << chunk.version;
        }
        transaction.updateItem<TerrainChunkPayload>(found->second.id,
                [vertices, vertexCount, placement, key, wireframe, layerSet, worldToTerrain, paintTile, topTile](TerrainChunkPayload& payload) {
            if (vertices) {
                payload.setBuffers(vertices, vertexCount);
            }
            payload.setPlacement(placement);
            payload.setKey(key);
            payload.setWireframe(wireframe);
            payload.setLayers(layerSet);
            payload.setWorldToTerrain(worldToTerrain);
            payload.setPaintTile(paintTile);
            payload.setTopTile(topTile);
        });
    }
    handOverSubItems();
    if (itemsMade && _firstDrawn == std::chrono::steady_clock::time_point()) {
        _firstDrawn = std::chrono::steady_clock::now();   // the arrival's clock for the upload share
    }
    if (lodDiag && (buffersTaken || buffersBuilt || itemsMade || itemsRemoved || itemsUpdated || itemsWaiting)) {
        // what this frame's drawn-set update cost on this thread (entry 24: before step 3a the buffer build was the
        // main-thread part of a cut shown; now the buffers come ready and "built" should read 0; "waiting" are new
        // items whose buffer is still on its way to the backend, step 3b)
        qCDebug(entitiesrenderer) << "Terrain LOD drawn" << drawnVersion << ":" << buffersTaken << "buffers taken ready," << buffersBuilt
                                  << "built here in" << buildMs << "ms," << (qulonglong)buildBytes << "bytes;" << itemsMade << "made,"
                                  << itemsUpdated << "updated," << itemsRemoved << "removed," << itemsWaiting << "waiting; uploads pending"
                                  << (qulonglong)entity->getUploadsWanted() << "wanted (" << (qulonglong)entity->getUploadsWantedBytes() << "bytes ),"
                                  << (qulonglong)(entity->getUploadsPending() - entity->getUploadsWanted()) << "unwanted; section"
                                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawnStart).count() << "ms";
    }
    if (terrainDebug && (itemsMade || itemsRemoved || itemsUpdated)) {
        qCDebug(entitiesrenderer) << "Terrain" << entity->getID() << "drawn version" << drawnVersion << ":" << drawn.size() << "meshes to draw,"
                                  << _chunkItems.size() << "items;" << itemsMade << "made," << itemsRemoved << "removed (" << qPrintable(removedKeys.join(", "))
                                  << ")," << itemsUpdated << "updated; placement changed" << placementChanged << "key" << keyChanged << "layers" << layersChanged
                                  << "tiles lost" << lostTiles.size() << "all" << allTilesLost << "top lost" << lostTopTiles.size() << "all" << allTopTilesLost
                                  << ";" << qPrintable(entity->describeDrawn());
    }
}

glm::ivec3 TerrainEntityRenderer::paintTileKey(const terrain::ChunkKey& chunk) {
    // a coarser node's own column at its level; a chunk of the grid's, its level-0 column
    const glm::ivec3 column = chunk.level < 0 ? chunk.coords : chunk.getSlot();
    return glm::ivec3(column.x, column.z, std::min(chunk.level, 0));
}

TerrainPaintTile::Pointer TerrainEntityRenderer::paintTileFor(const terrain::PaintMap& paint, const glm::ivec3& tileKey) {
    if (paint.isEmpty()) {
        return nullptr;
    }
    auto found = _paintTiles.find(tileKey);
    if (found != _paintTiles.end()) {
        return found->second;
    }
    TerrainPaintTile::Pointer tile = TerrainPaintTile::make(paint, tileKey.z, glm::ivec2(tileKey.x, tileKey.y));
    _paintTiles[tileKey] = tile;
    return tile;
}

gpu::TexturePointer TerrainEntityRenderer::topTileFor(const terrain::TopSurface& top, const terrain::ChunkKey& chunk) {
    if (top.isEmpty() || chunk.level < 0) {
        return nullptr;
    }
    const glm::ivec3 slot = chunk.getSlot();
    const glm::ivec2 column(slot.x, slot.z);
    auto found = _topTiles.find(column);
    if (found != _topTiles.end()) {
        return found->second;
    }
    std::vector<float> heights;
    top.fillTile(column, heights);
    gpu::TexturePointer tile = makeTopTileTexture(terrain::TopSurface::getTileSize(), heights, "terrain top surface");
    _topTiles[column] = tile;
    return tile;
}

void TerrainEntityRenderer::fetchTerrain(const QString& url, uint32_t version) {
    dropRequest();
    if (url != _requestURL || version != _requestVersion) {
        _fetchFailures = 0;
    }
    _requestURL = url;
    _requestVersion = version;
    // a save writes the file at the same URL, so the version goes into the request: caches on the way (the client's
    // own, a CDN's) see a new address, and the request itself asks the network rather than the local cache
    QUrl fetchURL(url);
    QUrlQuery query(fetchURL);
    query.addQueryItem("v", QString::number(version));
    fetchURL.setQuery(query);
    _request = DependencyManager::get<ResourceManager>()->createResourceRequest(this, fetchURL, true, -1, "TerrainEntityRenderer::fetchTerrain");
    if (!_request) {
        qCWarning(entitiesrenderer) << "Terrain file" << url << "cannot be fetched: no request for that kind of URL";
        if (auto entity = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(_entity)) {
            entity->clearGrid();
            entity->setGridSource(url, version);
        }
        return;
    }
    _request->setCacheEnabled(false);
    connect(_request, &ResourceRequest::finished, this, [this, url, version] {
        ResourceRequest* request = _request;
        _request = nullptr;
        if (!request) {
            return;
        }
        request->deleteLater();
        auto entity = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(_entity);
        if (!entity) {
            return;
        }
        bool loaded = false;
        if (request->getResult() != ResourceRequest::Success) {
            qCWarning(entitiesrenderer) << "Terrain file" << url << "could not be fetched:" << request->getResultString();
        } else {
            QString error;
            const QByteArray data = request->getData();
            loaded = entity->loadGrid(data, error);
            if (!loaded) {
                // what the loader was given, to tell a short read from a short file: its size and the SHA-256 of all
                // of it (the asset's name for an ATP file) and of its first KB
                qCWarning(entitiesrenderer) << "Terrain file" << url << "is not usable:" << error << "; given" << data.size()
                                            << "bytes, sha256"
                                            << QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()
                                            << ", of the first 1 KB"
                                            << QCryptographicHash::hash(data.left(1024), QCryptographicHash::Sha256).toHex();
            }
        }
        if (loaded) {
            _fetchFailures = 0;
        } else {
            // no ground until a later fetch brings the file whole (one first visit in five to the LAN copy of
            // Darkside got a file cut short at its first chunk, reported a success, and had no ground at all for
            // the visit, 2026-10-28)
            entity->clearGrid();
            fetchTerrainLater(url, version);
        }
        entity->setGridSource(url, version);
        entity->gridEdited();
    });
    _request->send();
}

void TerrainEntityRenderer::fetchTerrainLater(const QString& url, uint32_t version) {
    const int RETRY_FIRST_MS = 1000;
    const int RETRY_LONGEST_MS = 30000;
    const int delay = std::min(RETRY_FIRST_MS << std::min(_fetchFailures, 5), RETRY_LONGEST_MS);
    _fetchFailures++;
    qCWarning(entitiesrenderer) << "Terrain file" << url << "is fetched again in" << delay / 1000 << "s";
    QTimer::singleShot(delay, this, [this, url, version] {
        auto entity = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(_entity);
        // not if the entity has moved on to another file, or a fetch is already on its way
        if (entity && !_request && entity->getTerrainURL() == url && entity->getTerrainVersion() == version) {
            fetchTerrain(url, version);
        }
    });
}

void TerrainEntityRenderer::dropRequest() {
    if (_request) {
        _request->disconnect(this);
        _request->deleteLater();
        _request = nullptr;
    }
}

ItemKey TerrainEntityRenderer::chunkKey() const {
    ItemKey::Builder builder = ItemKey::Builder::opaqueShape().withTagBits(getTagMask()).withLayer(getHifiRenderLayer());
    if (_canCastShadow) {
        builder.withShadowCaster();
    }
    if (_cullWithParent) {
        builder.withSubMetaCulled();
    }
    if (!_visible) {
        builder.withInvisible();
    }
    return builder.build();
}

Transform TerrainEntityRenderer::chunkToWorld(const Transform& model, const glm::vec3& dimensions, const glm::vec3& voxelScale, const terrain::ChunkKey& chunk) {
    // The entity's model transform is its box's centre; grid voxel (0, 0, 0) is the box's low corner. The mesh is
    // in the chunk's (or coarser node's) own voxels, 1 / 2^level of a grid voxel, from its origin.
    Transform transform = model;
    transform.postTranslate(-0.5f * dimensions);
    transform.postScale(voxelScale);
    transform.postTranslate(glm::vec3(chunk.getOrigin()));
    transform.postScale(glm::vec3(chunk.getVoxelSize()));
    return transform;
}
