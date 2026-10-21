//
//  RenderLocalShadowTask.cpp
//  render-utils/src/
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderLocalShadowTask.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>

#include <QStringList>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>

#include <gpu/Context.h>
#include <shaders/Shaders.h>

#include <ViewFrustum.h>

#include <render/CullTask.h>
#include <render/SortTask.h>
#include <render/DrawTask.h>
#include <render/Scene.h>
#include <render/TransitionStage.h>

#include "FadeEffect.h"
#include "RenderUtilsLogging.h"

using namespace render;

extern void initZPassPipelines(ShapePlumber& plumber, gpu::StatePointer state, const render::ShapePipeline::BatchSetter& batchSetter, const render::ShapePipeline::ItemSetter& itemSetter);

const int LocalShadows::ATLAS_SIZE = 4096;
const int LocalShadows::SPOT_TILE_SIZE = 1024;
const int LocalShadows::SPOT_TILE_COUNT = 8;
const int LocalShadows::FACE_TILE_SIZE = 512;
const int LocalShadows::FACE_COUNT = 6;
const int LocalShadows::POINT_LIGHT_COUNT = 4;
const int LocalShadows::TILE_COUNT = LocalShadows::SPOT_TILE_COUNT + LocalShadows::POINT_LIGHT_COUNT * LocalShadows::FACE_COUNT;
const int LocalShadows::LIGHT_COUNT = 256;
const int LocalShadows::COOKIE_WIDTH = 1024;
const int LocalShadows::COOKIE_HEIGHT = 512;
const int LocalShadows::COOKIE_LAYERS = 8;
const int LocalShadows::COOKIE_LIGHT_COUNT = 16;

namespace {
    // the atlas: rows of spot tiles from the top, then rows of the point lights' faces
    const int SPOT_TILES_PER_ROW = LocalShadows::ATLAS_SIZE / LocalShadows::SPOT_TILE_SIZE;
    const int FACE_TILES_PER_ROW = LocalShadows::ATLAS_SIZE / LocalShadows::FACE_TILE_SIZE;
    const int FACE_ROWS_TOP = ((LocalShadows::SPOT_TILE_COUNT + SPOT_TILES_PER_ROW - 1) / SPOT_TILES_PER_ROW) * LocalShadows::SPOT_TILE_SIZE;
    const int FACE_ROWS = (LocalShadows::POINT_LIGHT_COUNT * LocalShadows::FACE_COUNT + FACE_TILES_PER_ROW - 1) / FACE_TILES_PER_ROW;
    static_assert(FACE_ROWS_TOP + FACE_ROWS * LocalShadows::FACE_TILE_SIZE <= LocalShadows::ATLAS_SIZE, "the tiles fit in the atlas");

    // the table's layout, as LocalShadow.slh reads it: an entry for each light, then each tile's entry, then a cookie
    // number for each light, then each cookie's entry
    const int LIGHT_TEXELS = LocalShadows::LIGHT_COUNT / 4;   // an entry for each light, four to a texel
    const int TILE_TEXELS = 6;                                 // per tile: a matrix, the tile's bounds, its parameters
    const int COOKIE_MAP_TEXEL = LIGHT_TEXELS + LocalShadows::TILE_COUNT * TILE_TEXELS;
    const int COOKIE_TEXEL = COOKIE_MAP_TEXEL + LIGHT_TEXELS;
    const int COOKIE_TEXELS = 5;                               // per cookie: a matrix, then its layer and kind
    const int TABLE_TEXELS = COOKIE_TEXEL + LocalShadows::COOKIE_LIGHT_COUNT * COOKIE_TEXELS;

    // the lookup moves off the surface by about this many shadow map texels, along its normal and toward the light,
    // so that it doesn't shadow itself
    const float NORMAL_OFFSET_TEXELS = 1.5f;
    const float LIGHT_OFFSET_TEXELS = 1.0f;
    // texels kept clear inside a tile's edges, so that filtering never reads the next tile
    const float TILE_MARGIN_TEXELS = 1.0f;

    // a wider cone would spread its shadow map too thin, and a square map can't take in 180 degrees at all
    const float MAX_SPOT_ANGLE = glm::radians(75.0f);
    // a cookie spreads over the cone's whole width, but nothing projects to 180 degrees either
    const float MAX_COOKIE_SPOT_ANGLE = glm::radians(85.0f);
    // the pictures in use are copied in again this often, for the detail a picture gains as it finishes loading
    const uint64_t COOKIE_REFRESH_FRAMES = 30;
    // a caster unchanged for this many scene frames counts as static (an avatar, or an animated model, changes every
    // frame and never does); one that changes stays dynamic this long after, then joins the static map
    const uint64_t STATIC_AFTER_FRAMES = 60;
    // a tile drawn this many frames in a row is busy (something in its light's view keeps moving); one kept this many
    // frames in a row is calm again
    const int BUSY_AFTER_FRAMES = 30;
    const int CALM_AFTER_FRAMES = 60;
    // scene frames between the shadow diagnostic's lines (TIVOLI_SHADOW_DIAG)
    const uint64_t SHADOW_DIAG_FRAMES = 240;
    const float FIELD_OF_VIEW_MARGIN = glm::radians(2.0f);   // so the edge of the cone is filtered like its middle
    const float MIN_NEAR_CLIP = 0.05f;
    const float NEAR_CLIP_SCALE = 0.01f;

    // A point light's face takes in its 90 degrees and a guard angle of this many texels beyond them, so that a lookup
    // near the edge of the face, with its taps spread and its offsets off the surface, still lands in this face's
    // map rather than on its border (as Unity's URP widens its point-light shadow slices by a fov bias)
    const float FACE_GUARD_TEXELS = 4.0f;
    const float FACE_FIELD_OF_VIEW = 2.0f * atanf(1.0f + 2.0f * FACE_GUARD_TEXELS / LocalShadows::FACE_TILE_SIZE);

    // the faces' axes, in the order the shaders pick them by (LocalShadow.slh: localShadowFace), and an up for each
    const glm::vec3 FACE_DIRECTIONS[] = {
        glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
    const glm::vec3 FACE_UPS[] = {
        glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };

    // the orientation that looks along the face's axis (a view looks down its -Z)
    const glm::quat& faceOrientation(int face) {
        static const std::array<glm::quat, 6> ORIENTATIONS = [] {
            std::array<glm::quat, 6> orientations;
            for (int i = 0; i < 6; ++i) {
                orientations[i] = glm::quat_cast(glm::inverse(glm::lookAt(glm::vec3(0.0f), FACE_DIRECTIONS[i], FACE_UPS[i])));
            }
            return orientations;
        }();
        return ORIENTATIONS[face];
    }

    // a light that had tiles last frame keeps them unless another outranks it by this much, so that two lights that
    // matter about as much don't take turns at casting
    const float KEEP_TILE_BONUS = 1.25f;
    // what the lights may shine on is fetched once, through a view taking in all of them, at this resolution
    const int COARSE_QUERY_RESOLUTION = 2048;

    // from the projection's [-1, 1] cube to [0, 1] texture coordinates and the reversed-Z depth the atlas stores (1 at
    // the near plane, 0 at the far one)
    const glm::mat4 BIAS_MATRIX {
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, -0.5f, 0.0f,
        0.5f, 0.5f, 0.5f, 1.0f };

    // from the projection's [-1, 1] square to a picture's texture coordinates; a picture's first row (t = 0) is its
    // top, so up from the light is up in the picture: it lands as it looks to someone standing behind the light
    const glm::mat4 COOKIE_BIAS_MATRIX {
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.5f, 0.0f,
        0.5f, 0.5f, 0.5f, 1.0f };

    // from [0, 1] texture coordinates over a whole shadow map to its tile's place in the atlas
    glm::mat4 tileMatrix(const glm::ivec4& viewport) {
        float scale = (float)viewport.z / LocalShadows::ATLAS_SIZE;
        glm::vec2 offset = glm::vec2(viewport.x, viewport.y) / (float)LocalShadows::ATLAS_SIZE;
        return glm::mat4(
            scale, 0.0f, 0.0f, 0.0f,
            0.0f, scale, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            offset.x, offset.y, 0.0f, 1.0f);
    }

    // A light whose shadow map's texels would be wider than this where the camera is gets no map, as a light beyond
    // Unity's shadow distance casts none: its shadows near the camera would be a blur, and every caster in its reach
    // would be drawn for them. For a point light's 512-texel face that is about 50 m from the camera (URP's default
    // shadow distance); a spot light's 1024-texel tile goes further, the narrower its cone the further. A light
    // marked Important casts at any distance
    const float MAX_SHADOW_TEXEL_AT_CAMERA = 0.2f;

    // how wide a texel of the light's shadow map is at the camera, for a map of the given field of view and size
    float shadowTexelAtCamera(const graphics::Light& light, const glm::vec3& viewPosition, float fieldOfView, int tileSize) {
        return 2.0f * tanf(0.5f * fieldOfView) * glm::distance(viewPosition, light.getPosition()) / tileSize;
    }

    // how much a light matters to the view: how big its reach looks from the camera, which grows as the camera
    // nears it and stays large inside it, times how bright it is
    float importance(const graphics::Light& light, const glm::vec3& viewPosition) {
        float range = light.getCutoffRadius();
        if (range <= 0.0f) {
            return 0.0f;
        }
        float gap = std::max(glm::distance(viewPosition, light.getPosition()) - range, 0.1f * range);
        float size = range / gap;
        return size * size * light.getIntensity() * std::max(glm::compMax(light.getColor()), 0.01f);
    }

    // a view down onto everything in the box, for culling what is in it
    void lookDownOn(const AABox& box, ViewFrustum& frustum) {
        glm::vec3 halfSize = 0.5f * box.getScale();
        frustum.setPosition(box.calcCenter() + glm::vec3(0.0f, 0.0f, halfSize.z + 1.0f));
        frustum.setOrientation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));   // looking down -Z
        frustum.setProjection(glm::ortho(-halfSize.x, halfSize.x, -halfSize.y, halfSize.y, 0.5f, 2.0f * halfSize.z + 1.5f));
        frustum.calculate();
    }

    // the box around everything the light reaches: as far as its light goes (its cutoff radius), which may be well
    // inside the entity's size, its maximum radius
    AABox reachOf(const graphics::Light& light) {
        float reach = light.getCutoffRadius();
        return AABox(light.getPosition() - glm::vec3(reach), 2.0f * reach);
    }
}

LocalShadows::LocalShadows() {
    auto depthFormat = gpu::Element(gpu::SCALAR, gpu::FLOAT, gpu::DEPTH);
    gpu::Sampler::Desc samplerDesc;
    samplerDesc._borderColor = glm::vec4(0.0f); // reversed-Z: the far plane is depth 0, so outside the map is lit
    samplerDesc._wrapModeU = gpu::Sampler::WRAP_BORDER;
    samplerDesc._wrapModeV = gpu::Sampler::WRAP_BORDER;
    samplerDesc._filter = gpu::Sampler::FILTER_MIN_MAG_LINEAR;
    samplerDesc._comparisonFunc = gpu::GREATER; // lit where the fragment is nearer the light than the map's depth
    _atlas = gpu::TexturePointer(gpu::Texture::createRenderBuffer(depthFormat, ATLAS_SIZE, ATLAS_SIZE,
        gpu::Texture::SINGLE_MIP, gpu::Sampler(samplerDesc)));
    _framebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create("Local shadow atlas"));
    _framebuffer->setDepthBuffer(_atlas, depthFormat);

    _tiles.resize(TILE_COUNT);
    _cookieLayers.resize(COOKIE_LAYERS);
    _tableData.resize(TABLE_TEXELS, glm::vec4(0.0f));
    _table = std::make_shared<gpu::Buffer>();
    uploadTable();
}

const gpu::BufferPointer& LocalShadows::getEmptyTable() {
    static const gpu::BufferPointer EMPTY_TABLE = [] {
        std::vector<glm::vec4> texels(TABLE_TEXELS, glm::vec4(0.0f));
        return std::make_shared<gpu::Buffer>(texels.size() * sizeof(glm::vec4), (const gpu::Byte*)texels.data());
    }();
    return EMPTY_TABLE;
}

glm::ivec4 LocalShadows::getTileViewport(int tile) const {
    if (tile < SPOT_TILE_COUNT) {
        return glm::ivec4((tile % SPOT_TILES_PER_ROW) * SPOT_TILE_SIZE, (tile / SPOT_TILES_PER_ROW) * SPOT_TILE_SIZE,
            SPOT_TILE_SIZE, SPOT_TILE_SIZE);
    }
    int face = tile - SPOT_TILE_COUNT;
    return glm::ivec4((face % FACE_TILES_PER_ROW) * FACE_TILE_SIZE, FACE_ROWS_TOP + (face / FACE_TILES_PER_ROW) * FACE_TILE_SIZE,
        FACE_TILE_SIZE, FACE_TILE_SIZE);
}

void LocalShadows::faceFrustum(const graphics::Light& light, int face, ViewFrustum& frustum) {
    float nearClip = std::max(MIN_NEAR_CLIP, light.getCutoffRadius() * NEAR_CLIP_SCALE);
    float farClip = std::max(light.getCutoffRadius(), 2.0f * nearClip);
    frustum.setPosition(light.getPosition());
    frustum.setOrientation(faceOrientation(face));
    frustum.setProjection(glm::perspective(FACE_FIELD_OF_VIEW, 1.0f, nearClip, farClip));
    frustum.calculate();
}

bool LocalShadows::hasLights() const {
    return std::any_of(_tiles.begin(), _tiles.end(), [](const Tile& tile) { return (bool)tile.light; });
}

void LocalShadows::clear() {
    for (auto& tile : _tiles) {
        tile.light.reset();
        tile.lightIndex = LightStage::INVALID_INDEX;
    }
    _cookieCount = 0;
    if (++_frame % COOKIE_REFRESH_FRAMES == 0) {
        for (auto& layer : _cookieLayers) {
            layer.due = true;
        }
    }
    std::fill(_tableData.begin(), _tableData.end(), glm::vec4(0.0f));
}

void LocalShadows::setSpotTile(int tile, const graphics::LightPointer& light, LightStage::Index lightIndex, const ViewFrustum& frustum) {
    if (tile < 0 || tile >= SPOT_TILE_COUNT || lightIndex < 0 || lightIndex >= LIGHT_COUNT) {
        return;
    }
    fillTile(tile, light, lightIndex, frustum);
    // a spot light's entry: its tile plus one
    _tableData[lightIndex / 4][lightIndex % 4] = (float)(tile + 1);
}

void LocalShadows::setPointFaces(int point, const graphics::LightPointer& light, LightStage::Index lightIndex, const ViewFrustum* faces) {
    if (point < 0 || point >= POINT_LIGHT_COUNT || lightIndex < 0 || lightIndex >= LIGHT_COUNT) {
        return;
    }
    int first = firstFaceTile(point);
    for (int face = 0; face < FACE_COUNT; ++face) {
        fillTile(first + face, light, lightIndex, faces[face]);
    }
    // a point light's entry: its first face's tile plus one, negated, to tell it from a spot light's
    _tableData[lightIndex / 4][lightIndex % 4] = -(float)(first + 1);
}

void LocalShadows::fillTile(int tile, const graphics::LightPointer& light, LightStage::Index lightIndex, const ViewFrustum& frustum) {
    auto& entry = _tiles[tile];
    entry.light = light;
    entry.lightIndex = lightIndex;
    if (!entry.frustum) {
        entry.frustum = std::make_shared<ViewFrustum>();
    }
    *entry.frustum = frustum;

    glm::ivec4 viewport = getTileViewport(tile);
    glm::vec2 low = (glm::vec2(viewport.x, viewport.y) + TILE_MARGIN_TEXELS) / (float)ATLAS_SIZE;
    glm::vec2 high = (glm::vec2(viewport.x + viewport.z, viewport.y + viewport.w) - TILE_MARGIN_TEXELS) / (float)ATLAS_SIZE;
    glm::mat4 worldToTile = tileMatrix(viewport) * BIAS_MATRIX * frustum.getProjection() * glm::inverse(frustum.getView());
    float texelSizePerMeter = 2.0f * tanf(0.5f * glm::radians(frustum.getFieldOfView())) / viewport.z;   // the frustum's field of view is in degrees

    int texel = LIGHT_TEXELS + tile * TILE_TEXELS;
    for (int column = 0; column < 4; ++column) {
        _tableData[texel + column] = worldToTile[column];
    }
    _tableData[texel + 4] = glm::vec4(low, high);
    _tableData[texel + 5] = glm::vec4(texelSizePerMeter, NORMAL_OFFSET_TEXELS, LIGHT_OFFSET_TEXELS, 1.0f / ATLAS_SIZE);
}

const LocalShadows::TilePlan* LocalShadows::planTile(int tile, const render::ShapeBounds& casters, const render::Scene& scene) {
    if (tile < 0 || tile >= getTileCount() || !_tiles[tile].light || !_tiles[tile].frustum) {
        return nullptr;
    }
    auto& entry = _tiles[tile];
    const uint64_t frame = scene.getFrame();
    const bool split = _caching && _staticCaching;
    if (split) {
        ensureStaticAtlas();
    }

    // the casters in the light's view this frame, the static ones (unchanged for a while) apart from the dynamic
    // (changed lately, or in a transition such as fading in, which changes them every frame without a word to the
    // scene); and whether one of either kind has changed since its map was drawn
    _plan.staticCasters.clear();
    _plan.dynamicCasters.clear();
    _staticIds.clear();
    _casterIds.clear();
    bool staticChanged = false;
    bool staticChangedSinceDrawn = false;
    bool dynamicChanged = false;
    int staticCount = 0;
    // TIVOLI_SHADOW_DIAG in the environment: every few seconds a line per tile, with the first of its dynamic casters
    // (where and how big, how long since they changed), to tell what keeps a light's map busy
    static const bool SHADOW_DIAG = getenv("TIVOLI_SHADOW_DIAG") != nullptr;
    const bool diagFrame = SHADOW_DIAG && frame % SHADOW_DIAG_FRAMES == 0;
    QStringList diagCasters;
    for (const auto& shapes : casters) {
        for (const auto& bound : shapes.second) {
            const auto& item = scene.getItem(bound.id);
            bool transitioning = !render::TransitionStage::isIndexInvalid(item.getTransitionId());
            bool dynamic = !split || transitioning || item.getChangeFrame() + STATIC_AFTER_FRAMES > frame;
            if (dynamic) {
                _plan.dynamicCasters[shapes.first].push_back(bound);
                _casterIds.push_back(bound.id);
                dynamicChanged = dynamicChanged || transitioning || item.getChangeFrame() > entry.drawnFrame;
                if (diagFrame && split && diagCasters.size() < 4) {
                    glm::vec3 center = bound.bound.calcCenter();
                    diagCasters << QString("(%1, %2, %3) %4 m, changed %5 frames ago%6").arg(center.x, 0, 'f', 1)
                        .arg(center.y, 0, 'f', 1).arg(center.z, 0, 'f', 1).arg(glm::compMax(bound.bound.getScale()), 0, 'f', 1)
                        .arg(frame - item.getChangeFrame()).arg(transitioning ? ", fading" : "");
                }
            } else {
                _plan.staticCasters[shapes.first].push_back(bound);
                _staticIds.push_back(bound.id);
                staticChanged = staticChanged || item.getChangeFrame() > entry.staticFrame;
                staticChangedSinceDrawn = staticChangedSinceDrawn || item.getChangeFrame() > entry.drawnFrame;
                ++staticCount;
            }
        }
    }
    std::sort(_casterIds.begin(), _casterIds.end());
    std::sort(_staticIds.begin(), _staticIds.end());
    const int dynamicCount = (int)_casterIds.size();

    const glm::mat4& view = entry.frustum->getView();
    const glm::mat4& projection = entry.frustum->getProjection();

    // A busy tile, drawn every frame anyway because something in its light's view keeps moving, draws its static
    // casters with the others when copying their map in would cost more draws than they take; their map is left
    // as it was, and drawn again when the tile is layered once more
    const bool layered = split && !(entry.busy && staticCount <= _blitCostInDraws);
    if (split && !layered) {
        for (const auto& shapes : _plan.staticCasters) {
            auto& into = _plan.dynamicCasters[shapes.first];
            into.insert(into.end(), shapes.second.begin(), shapes.second.end());
        }
        _casterIds.insert(_casterIds.end(), _staticIds.begin(), _staticIds.end());
        std::sort(_casterIds.begin(), _casterIds.end());
        dynamicChanged = dynamicChanged || staticChangedSinceDrawn;
        entry.staticDrawn = false;
    }

    // the static map: drawn again when the view moved or the static casters differ from those in it
    _plan.redrawStatic = layered && (!entry.staticDrawn || staticChanged || view != entry.staticView
        || projection != entry.staticProjection || _staticIds != entry.staticCasters);
    _plan.copyStatic = layered;
    if (_plan.redrawStatic) {
        entry.staticDrawn = true;
        entry.staticView = view;
        entry.staticProjection = projection;
        entry.staticCasters.swap(_staticIds);
        entry.staticFrame = frame;
    }

    // the working map: kept while the view, the static map under it and the casters drawn into it are as drawn
    uint64_t staticFrame = layered ? entry.staticFrame : 0;
    bool keep = _caching && entry.drawn && !dynamicChanged && layered == entry.drawnLayered && view == entry.drawnView
        && projection == entry.drawnProjection && _casterIds == entry.drawnCasters && staticFrame == entry.drawnStaticFrame;
    // busy after it has been drawn every frame for a while, calm again after it has been kept for a while
    if (keep) {
        entry.keptStreak++;
        entry.drawnStreak = 0;
    } else {
        entry.drawnStreak++;
        entry.keptStreak = 0;
    }
    entry.busy = entry.busy ? entry.keptStreak < CALM_AFTER_FRAMES : entry.drawnStreak >= BUSY_AFTER_FRAMES;
    if (diagFrame) {
        qCDebug(renderutils).noquote() << "SHADOW-DIAG tile" << tile << "light" << entry.lightIndex
            << (keep ? "kept" : "drawn") << (layered ? "layered" : "whole") << (entry.busy ? "busy" : "calm")
            << "static" << staticCount << "dynamic" << dynamicCount << (diagCasters.isEmpty() ? QString() : ":")
            << diagCasters.join("; ");
    }
    if (keep) {
        return nullptr;
    }
    entry.drawn = true;
    entry.drawnLayered = layered;
    entry.drawnView = view;
    entry.drawnProjection = projection;
    entry.drawnCasters.swap(_casterIds);
    entry.drawnFrame = frame;
    entry.drawnStaticFrame = staticFrame;
    return &_plan;
}

void LocalShadows::ensureStaticAtlas() {
    if (_staticAtlas) {
        return;
    }
    // the working atlas's twin: the same tiles, in the same format, so that a tile blits across point for point;
    // the shaders never read it
    auto depthFormat = gpu::Element(gpu::SCALAR, gpu::FLOAT, gpu::DEPTH);
    gpu::Sampler::Desc samplerDesc;
    samplerDesc._filter = gpu::Sampler::FILTER_MIN_MAG_POINT;
    samplerDesc._wrapModeU = gpu::Sampler::WRAP_CLAMP;
    samplerDesc._wrapModeV = gpu::Sampler::WRAP_CLAMP;
    _staticAtlas = gpu::TexturePointer(gpu::Texture::createRenderBuffer(depthFormat, ATLAS_SIZE, ATLAS_SIZE,
        gpu::Texture::SINGLE_MIP, gpu::Sampler(samplerDesc)));
    _staticFramebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create("Local shadow static atlas"));
    _staticFramebuffer->setDepthBuffer(_staticAtlas, depthFormat);
}

void LocalShadows::copyStaticMap(gpu::Batch& batch, const glm::ivec4& viewport) {
    if (!_staticFramebuffer) {
        return;
    }
    glm::ivec4 rect(viewport.x, viewport.y, viewport.x + viewport.z, viewport.y + viewport.w);
    batch.blit(_staticFramebuffer, rect, _framebuffer, rect);
}

bool LocalShadows::setSpotCookie(LightStage::Index lightIndex, const gpu::TexturePointer& picture, const ViewFrustum& frustum) {
    return addCookie(lightIndex, picture, COOKIE_BIAS_MATRIX * frustum.getProjection() * glm::inverse(frustum.getView()), false);
}

bool LocalShadows::setPointCookie(LightStage::Index lightIndex, const gpu::TexturePointer& picture, const glm::mat4& worldToLight) {
    return addCookie(lightIndex, picture, worldToLight, true);
}

bool LocalShadows::addCookie(LightStage::Index lightIndex, const gpu::TexturePointer& picture, const glm::mat4& matrix, bool panorama) {
    if (!picture || _cookieCount >= COOKIE_LIGHT_COUNT || lightIndex < 0 || lightIndex >= LIGHT_COUNT) {
        return false;
    }
    int layer = cookieLayer(picture);
    if (layer < 0) {
        return false;
    }
    int cookie = _cookieCount++;
    _tableData[COOKIE_MAP_TEXEL + lightIndex / 4][lightIndex % 4] = (float)(cookie + 1);

    int texel = COOKIE_TEXEL + cookie * COOKIE_TEXELS;
    for (int column = 0; column < 4; ++column) {
        _tableData[texel + column] = matrix[column];
    }
    _tableData[texel + 4] = glm::vec4((float)layer, panorama ? 1.0f : 0.0f, 0.0f, 0.0f);
    return true;
}

int LocalShadows::cookieLayer(const gpu::TexturePointer& picture) {
    // the layer the picture is in already; else an empty one, or the one unused longest, but not one another light
    // uses this frame
    int chosen = -1;
    for (int i = 0; i < (int)_cookieLayers.size(); ++i) {
        auto& layer = _cookieLayers[i];
        auto held = layer.picture.lock();
        if (held == picture) {
            layer.lastUsed = _frame;
            return i;
        }
        if (held && layer.lastUsed == _frame) {
            continue;
        }
        if (chosen < 0) {
            chosen = i;
            continue;
        }
        bool empty = !held;
        bool chosenEmpty = _cookieLayers[chosen].picture.expired();
        if ((empty && !chosenEmpty) || (empty == chosenEmpty && layer.lastUsed < _cookieLayers[chosen].lastUsed)) {
            chosen = i;
        }
    }
    if (chosen < 0) {
        return -1;
    }

    if (!_cookies) {
        // made when the first light has a cookie: sRGB like the pictures, with mips for seeing cookies from afar. It
        // repeats across, for the seam behind a point light where its panorama's two ends meet; a spot light's cone
        // fits inside its picture, so the repeat never shows in one
        gpu::Sampler::Desc samplerDesc;
        samplerDesc._filter = gpu::Sampler::FILTER_ANISOTROPIC;
        samplerDesc._wrapModeU = gpu::Sampler::WRAP_REPEAT;
        samplerDesc._wrapModeV = gpu::Sampler::WRAP_CLAMP;
        samplerDesc._maxAnisotropy = 8;
        _cookies = gpu::Texture::createRenderBufferArray(gpu::Element::COLOR_SRGBA_32, COOKIE_WIDTH, COOKIE_HEIGHT,
            COOKIE_LAYERS, gpu::Texture::MAX_NUM_MIPS, gpu::Sampler(samplerDesc));
        for (int i = 0; i < COOKIE_LAYERS; ++i) {
            auto framebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create("Light cookie"));
            framebuffer->setRenderBuffer(0, _cookies, i);
            _cookieFramebuffers.push_back(framebuffer);
        }
        _cookiePipeline = gpu::Pipeline::create(gpu::Shader::createProgram(shader::gpu::program::drawUnitQuatTextureOpaque),
            std::make_shared<gpu::State>());
    }
    auto& layer = _cookieLayers[chosen];
    layer.picture = picture;
    layer.lastUsed = _frame;
    layer.due = true;
    return chosen;
}

bool LocalShadows::cookiesDue() const {
    return std::any_of(_cookieLayers.begin(), _cookieLayers.end(), [this](const CookieLayer& layer) {
        return layer.due && layer.lastUsed == _frame && !layer.picture.expired();
    });
}

void LocalShadows::drawCookies(gpu::Batch& batch) {
    bool drawn = false;
    for (int i = 0; i < (int)_cookieLayers.size(); ++i) {
        auto& layer = _cookieLayers[i];
        auto picture = layer.picture.lock();
        if (!layer.due || layer.lastUsed != _frame || !picture) {
            continue;   // a layer not in use now is copied when it is next used
        }
        layer.due = false;
        if (!drawn) {
            batch.enableStereo(false);
            batch.setPipeline(_cookiePipeline);
            drawn = true;
        }
        // the whole picture, scaled to the layer
        batch.setFramebuffer(_cookieFramebuffers[i]);
        batch.setViewportTransform(glm::ivec4(0, 0, COOKIE_WIDTH, COOKIE_HEIGHT));
        batch.setResourceTexture(0, picture);
        batch.draw(gpu::TRIANGLE_STRIP, 4);
    }
    if (drawn) {
        batch.setResourceTexture(0, nullptr);
        batch.generateTextureMips(_cookies);
    }
}

void LocalShadows::uploadTable() {
    _table->setData(_tableData.size() * sizeof(glm::vec4), (const gpu::Byte*)_tableData.data());
}

void RenderLocalShadowTask::build(JobModel& task, const render::Varying& input, render::Varying& output, uint8_t tagBits, uint8_t tagMask) {
    ShapePlumberPointer shapePlumber = std::make_shared<ShapePlumber>();
    {
        auto state = std::make_shared<gpu::State>();
        state->setCullMode(gpu::State::CULL_BACK);
        state->setDepthTest(true, true, gpu::GREATER_EQUAL);

        auto fadeEffect = DependencyManager::get<FadeEffect>();
        initZPassPipelines(*shapePlumber, state, fadeEffect->getBatchSetter(), fadeEffect->getItemUniformSetter());
    }

    // translucent items cast too (solid, or cut out by the opacity mask), as for the sun: see RenderShadowTask
    const ItemFilter casterReceiverFilter = ItemFilter::Builder::visibleWorldItems().withoutLayered()
        .withTagBits(tagBits, tagMask).build();
    const auto setupOutput = task.addJob<RenderLocalShadowSetup>("LocalShadowSetup", input);
    const auto queryResolution = setupOutput.getN<RenderLocalShadowSetup::Output>(1);
    const auto localShadows = setupOutput.getN<RenderLocalShadowSetup::Output>(2);

    // what any of the lights might shine on, fetched once through a view that takes them all in; every item however
    // small, so that the casters in a light's view don't come and go with the level of detail's size scale, which
    // would redraw its cached map
    const auto fetchInput = FetchSpatialTree::Inputs(casterReceiverFilter, queryResolution).asVarying();
    const auto selection = task.addJob<FetchSpatialTree>("FetchLocalShadowTree", fetchInput, false);
    const auto selectionInputs = CullSpatialSelection::Inputs(selection, casterReceiverFilter).asVarying();
    const auto items = task.addJob<CullSpatialSelection>("FilterLocalShadowSelection", selectionInputs, nullptr, true, RenderDetails::SHADOW);
    const auto sortedPipelines = task.addJob<PipelineSortShapes>("PipelineSortLocalShadow", items);
    const auto sortedShapes = task.addJob<DepthSortShapes>("DepthSortLocalShadow", sortedPipelines, true);

    // a view's turn: of the shapes, the casters its light sees from it, drawn into its tile; the view is taken from
    // the whole light's casters when it is one face of a point light
    char jobName[64];
    auto addTileJobs = [&](int tile, const render::Varying& shapes) {
        snprintf(jobName, sizeof(jobName), "LocalShadowSlotSetup%d", tile);
        const auto slotOutput = task.addJob<RenderLocalShadowSlotSetup>(jobName, localShadows, tile, casterReceiverFilter);
        const auto slotFilter = slotOutput.getN<RenderLocalShadowSlotSetup::Output>(0);
        const auto slotLight = slotOutput.getN<RenderLocalShadowSlotSetup::Output>(1);
        const auto slotCullFunctor = slotOutput.getN<RenderLocalShadowSlotSetup::Output>(2);

        snprintf(jobName, sizeof(jobName), "CullLocalShadowCasters%d", tile);
        const auto cullInputs = CullShadowBounds::Inputs(shapes, slotFilter, render::Varying(ViewFrustumPointer()),
            slotLight, slotCullFunctor).asVarying();
        const auto casters = task.addJob<CullShadowBounds>(jobName, cullInputs);

        snprintf(jobName, sizeof(jobName), "RenderLocalShadowMap%d", tile);
        const auto renderInputs = RenderLocalShadowMap::Inputs(casters.getN<CullShadowBounds::Outputs>(0), localShadows).asVarying();
        task.addJob<RenderLocalShadowMap>(jobName, renderInputs, shapePlumber, tile);

        snprintf(jobName, sizeof(jobName), "LocalShadowSlotTeardown%d", tile);
        task.addJob<RenderShadowCascadeTeardown>(jobName, slotFilter);
    };

    for (int tile = 0; tile < LocalShadows::SPOT_TILE_COUNT; ++tile) {
        addTileJobs(tile, sortedShapes);
    }
    for (int point = 0; point < LocalShadows::POINT_LIGHT_COUNT; ++point) {
        // the point light's casters, those within its reach, culled once for its six faces
        int first = LocalShadows::firstFaceTile(point);
        snprintf(jobName, sizeof(jobName), "LocalShadowPointSetup%d", point);
        const auto pointOutput = task.addJob<RenderLocalShadowSlotSetup>(jobName, localShadows, first, casterReceiverFilter, true);
        const auto pointFilter = pointOutput.getN<RenderLocalShadowSlotSetup::Output>(0);
        const auto pointLight = pointOutput.getN<RenderLocalShadowSlotSetup::Output>(1);
        const auto pointCullFunctor = pointOutput.getN<RenderLocalShadowSlotSetup::Output>(2);

        snprintf(jobName, sizeof(jobName), "CullLocalShadowPointCasters%d", point);
        const auto cullInputs = CullShadowBounds::Inputs(sortedShapes, pointFilter, render::Varying(ViewFrustumPointer()),
            pointLight, pointCullFunctor).asVarying();
        const auto pointCasters = task.addJob<CullShadowBounds>(jobName, cullInputs).getN<CullShadowBounds::Outputs>(0);

        for (int face = 0; face < LocalShadows::FACE_COUNT; ++face) {
            addTileJobs(first + face, pointCasters);
        }
        snprintf(jobName, sizeof(jobName), "LocalShadowPointTeardown%d", point);
        task.addJob<RenderShadowCascadeTeardown>(jobName, pointFilter);
    }
    task.addJob<RenderLocalShadowTeardown>("LocalShadowTeardown", setupOutput);
}

RenderLocalShadowSetup::RenderLocalShadowSetup() :
    _coarseFrustum{ std::make_shared<ViewFrustum>() } {
}

void RenderLocalShadowSetup::run(const render::RenderContextPointer& renderContext, const Input& input, Output& output) {
    RenderArgs* args = renderContext->args;
    const auto& lightFrame = input.get0();
    const auto& lightingModel = input.get1();
    const auto& shadowFrame = input.get2();
    auto lightStage = renderContext->_scene->getStage<LightStage>();

    if (!_localLights) {
        // this view does without: no tiles, no atlas made; the fetch gets a view onto nothing
        if (shadowFrame) {
            shadowFrame->_localShadows = nullptr;
        }
        _coarseFrustum->setPosition(glm::vec3(0.0f, -1.0e6f, 0.0f));
        _coarseFrustum->setProjection(glm::perspective(glm::radians(1.0f), 1.0f, 0.1f, 0.2f));
        _coarseFrustum->calculate();
        output.edit0() = args->_renderMode;
        output.edit1() = glm::ivec2(COARSE_QUERY_RESOLUTION);
        output.edit2() = LocalShadowsPointer();
        args->pushViewFrustum(*_coarseFrustum);
        args->_renderMode = RenderArgs::SHADOW_RENDER_MODE;   // the teardown pops the frustum in this mode
        return;
    }
    if (!_localShadows) {
        _localShadows = std::make_shared<LocalShadows>();
    }
    _localShadows->setCaching(_caching);
    _localShadows->setStaticCaching(_staticCaching);
    _localShadows->setBlitCostInDraws(_blitCostInDraws);
    // which light had which tile last frame (a point light all six of its faces), and how many of those tiles were
    // busy, and drawn over a copy of their static map
    std::vector<LightStage::Index> previousTiles;
    int busyTiles = 0;
    int layeredTiles = 0;
    for (int tile = 0; tile < _localShadows->getTileCount(); ++tile) {
        const auto& previous = _localShadows->getTile(tile);
        previousTiles.push_back(previous.light ? previous.lightIndex : LightStage::INVALID_INDEX);
        if (previous.light) {
            busyTiles += previous.busy ? 1 : 0;
            layeredTiles += previous.drawnLayered ? 1 : 0;
        }
    }
    std::static_pointer_cast<Config>(renderContext->jobConfig)->setTileCounts(busyTiles, layeredTiles);
    auto previousTile = [&](LightStage::Index index) {
        auto found = std::find(previousTiles.begin(), previousTiles.end(), index);
        return found == previousTiles.end() ? -1 : (int)(found - previousTiles.begin());
    };
    _localShadows->clear();
    if (shadowFrame) {
        shadowFrame->_localShadows = nullptr;
    }

    // the lights that reach into view: those that cast shadows are candidates for tiles, those whose cookie has loaded
    // get it; the ones that matter most first, and those marked important before all the others
    struct Candidate {
        LightStage::Index index;
        graphics::LightPointer light;
        bool important;
        float rank;
        gpu::TexturePointer cookie;
        bool point;
    };
    std::vector<Candidate> spots;
    std::vector<Candidate> points;
    std::vector<Candidate> cookieLights;
    if (shadowFrame && lightFrame && lightStage && lightingModel) {
        const ViewFrustum& view = args->getViewFrustum();
        bool shadows = lightingModel->isShadowEnabled();
        // a light casts when its Shadows field is On or Important (or every light does, for comparing), and gets a
        // map when that map would be sharp enough at the camera or the light is Important
        auto casts = [&](const graphics::Light& light) {
            return light.getCastShadows() && (light.getShadowsOn() || _everyLightCasts);
        };
        auto sharpEnough = [&](const graphics::Light& light, float fieldOfView, int tileSize) {
            return light.getShadowsImportant() ||
                shadowTexelAtCamera(light, view.getPosition(), fieldOfView, tileSize) <= MAX_SHADOW_TEXEL_AT_CAMERA;
        };
        if (lightingModel->isSpotLightEnabled()) {
            for (auto index : lightFrame->_spotLights) {
                if (index < 0 || index >= LocalShadows::LIGHT_COUNT) {
                    continue;   // beyond the light array the shaders hold
                }
                auto light = lightStage->getLight(index);
                if (!light || !view.sphereIntersectsKeyhole(light->getPosition(), light->getCutoffRadius())) {
                    continue;
                }
                float rank = importance(*light, view.getPosition());
                auto cookieSource = light->getCookie();
                gpu::TexturePointer cookie = cookieSource ? cookieSource->getGPUTexture() : gpu::TexturePointer();
                if (cookie && light->getSpotAngle() <= MAX_COOKIE_SPOT_ANGLE) {
                    cookieLights.push_back({ index, light, false, rank, cookie, false });
                }
                if (shadows && casts(*light) && light->getSpotAngle() <= MAX_SPOT_ANGLE &&
                        sharpEnough(*light, 2.0f * light->getSpotAngle() + FIELD_OF_VIEW_MARGIN, LocalShadows::SPOT_TILE_SIZE)) {
                    bool hadTile = previousTile(index) >= 0;
                    spots.push_back({ index, light, light->getShadowsImportant(), hadTile ? rank * KEEP_TILE_BONUS : rank, nullptr, false });
                }
            }
        }
        if (lightingModel->isPointLightEnabled()) {
            for (auto index : lightFrame->_pointLights) {
                if (index < 0 || index >= LocalShadows::LIGHT_COUNT) {
                    continue;
                }
                auto light = lightStage->getLight(index);
                if (!light || !view.sphereIntersectsKeyhole(light->getPosition(), light->getCutoffRadius())) {
                    continue;
                }
                float rank = importance(*light, view.getPosition());
                auto cookieSource = light->getCookie();
                gpu::TexturePointer cookie = cookieSource ? cookieSource->getGPUTexture() : gpu::TexturePointer();
                if (cookie) {
                    cookieLights.push_back({ index, light, false, rank, cookie, true });
                }
                if (shadows && casts(*light) && sharpEnough(*light, FACE_FIELD_OF_VIEW, LocalShadows::FACE_TILE_SIZE)) {
                    bool hadTiles = previousTile(index) >= 0;
                    points.push_back({ index, light, light->getShadowsImportant(), hadTiles ? rank * KEEP_TILE_BONUS : rank, nullptr, true });
                }
            }
        }
    }
    auto outranks = [](const Candidate& a, const Candidate& b) {
        return a.important != b.important ? a.important : a.rank > b.rank;
    };
    std::sort(spots.begin(), spots.end(), outranks);
    std::sort(points.begin(), points.end(), outranks);
    std::sort(cookieLights.begin(), cookieLights.end(), outranks);
    if (spots.size() > (size_t)LocalShadows::SPOT_TILE_COUNT) {
        spots.resize(LocalShadows::SPOT_TILE_COUNT);
    }
    if (points.size() > (size_t)LocalShadows::POINT_LIGHT_COUNT) {
        points.resize(LocalShadows::POINT_LIGHT_COUNT);
    }

    // a spot light's cookie is projected from the light down its -Z axis, exactly as wide as the cone, so that the
    // picture fills the square the cone fits in; a point light's hangs around the light in the light's own frame. A
    // light whose picture finds no layer this frame shines without it
    ViewFrustum frustum;
    for (const auto& cookieLight : cookieLights) {
        const auto& light = cookieLight.light;
        if (cookieLight.point) {
            glm::mat4 worldToLight = glm::mat4_cast(glm::inverse(light->getOrientation())) * glm::translate(glm::mat4(1.0f), -light->getPosition());
            _localShadows->setPointCookie(cookieLight.index, cookieLight.cookie, worldToLight);
            continue;
        }
        float farClip = std::max(light->getMaximumRadius(), 2.0f * MIN_NEAR_CLIP);
        frustum.setPosition(light->getPosition());
        frustum.setOrientation(light->getOrientation());
        frustum.setProjection(glm::perspective(2.0f * light->getSpotAngle(), 1.0f, MIN_NEAR_CLIP, farClip));
        frustum.calculate();
        _localShadows->setSpotCookie(cookieLight.index, cookieLight.cookie, frustum);
    }
    if (_localShadows->cookiesDue()) {
        gpu::doInBatch("RenderLocalShadowSetup::drawCookies", args->_context, [&](gpu::Batch& batch) {
            _localShadows->drawCookies(batch);
        });
    }

    if (spots.empty() && points.empty()) {
        _localShadows->uploadTable();
        if (_localShadows->hasCookies()) {
            shadowFrame->_localShadows = _localShadows;
        }
        renderContext->taskFlow.abortTask();
        return;
    }

    // lights keep the place they had (a spot light its tile, a point light its six faces); the others take the free
    // ones. `placeOfTile` says which place of this kind a tile from last frame belongs to, -1 for one of the other kind
    auto assignPlaces = [&](const std::vector<Candidate>& lights, int placeCount, const std::function<int(int)>& placeOfTile) {
        std::vector<int> places(lights.size(), -1);
        std::vector<bool> taken(placeCount, false);
        for (size_t i = 0; i < lights.size(); ++i) {
            int tile = previousTile(lights[i].index);
            int place = tile >= 0 ? placeOfTile(tile) : -1;
            if (place >= 0 && !taken[place]) {
                places[i] = place;
                taken[place] = true;
            }
        }
        for (size_t i = 0; i < lights.size(); ++i) {
            if (places[i] < 0) {
                places[i] = (int)(std::find(taken.begin(), taken.end(), false) - taken.begin());
                taken[places[i]] = true;
            }
        }
        return places;
    };
    auto spotTiles = assignPlaces(spots, LocalShadows::SPOT_TILE_COUNT,
        [](int tile) { return tile < LocalShadows::SPOT_TILE_COUNT ? tile : -1; });
    auto pointPlaces = assignPlaces(points, LocalShadows::POINT_LIGHT_COUNT,
        [](int tile) { return tile >= LocalShadows::SPOT_TILE_COUNT ? (tile - LocalShadows::SPOT_TILE_COUNT) / LocalShadows::FACE_COUNT : -1; });

    // each spot light's view: from the light down its -Z axis, wide enough for its cone, as far as it reaches
    AABox reach;
    for (size_t i = 0; i < spots.size(); ++i) {
        const auto& light = spots[i].light;
        float nearClip = std::max(MIN_NEAR_CLIP, light->getCutoffRadius() * NEAR_CLIP_SCALE);
        float farClip = std::max(light->getCutoffRadius(), 2.0f * nearClip);
        frustum.setPosition(light->getPosition());
        frustum.setOrientation(light->getOrientation());
        frustum.setProjection(glm::perspective(2.0f * light->getSpotAngle() + FIELD_OF_VIEW_MARGIN, 1.0f, nearClip, farClip));
        frustum.calculate();
        _localShadows->setSpotTile(spotTiles[i], light, spots[i].index, frustum);
        reach += reachOf(*light);
    }
    // each point light's six views: out from the light along the world's axes
    std::vector<ViewFrustum> faces(LocalShadows::FACE_COUNT);
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& light = points[i].light;
        for (int face = 0; face < LocalShadows::FACE_COUNT; ++face) {
            LocalShadows::faceFrustum(*light, face, faces[face]);
        }
        _localShadows->setPointFaces(pointPlaces[i], light, points[i].index, faces.data());
        reach += reachOf(*light);
    }
    _localShadows->uploadTable();
    shadowFrame->_localShadows = _localShadows;

    // a view down onto everything the lights reach, for fetching what they might shine on once for all of them
    lookDownOn(reach, *_coarseFrustum);

    output.edit0() = args->_renderMode;
    output.edit1() = glm::ivec2(COARSE_QUERY_RESOLUTION);
    output.edit2() = _localShadows;

    args->pushViewFrustum(*_coarseFrustum);
    args->_renderMode = RenderArgs::SHADOW_RENDER_MODE;
}

RenderLocalShadowSlotSetup::RenderLocalShadowSlotSetup(int tile, render::ItemFilter filter, bool wholeLight) :
    _tile{ tile }, _filter{ filter }, _wholeLight{ wholeLight } {
    if (wholeLight) {
        _reach = std::make_shared<ViewFrustum>();
    }
}

void RenderLocalShadowSlotSetup::run(const render::RenderContextPointer& renderContext, const Input& input, Output& output) {
    const auto& localShadows = input;
    if (!localShadows || _tile >= localShadows->getTileCount() || !localShadows->getTile(_tile).light) {
        output.edit0() = ItemFilter::Builder::nothing();
        output.edit1() = graphics::LightPointer();
        output.edit2() = RenderShadowTask::CullFunctor();
        return;
    }
    const auto& tile = localShadows->getTile(_tile);
    if (_wholeLight) {
        // everything within the light's reach, for its faces to cull from
        lookDownOn(reachOf(*tile.light), *_reach);
        renderContext->args->pushViewFrustum(*_reach);
    } else {
        renderContext->args->pushViewFrustum(*tile.frustum);
    }
    output.edit0() = _filter;
    output.edit1() = tile.light;
    output.edit2() = RenderShadowTask::CullFunctor();   // every caster, however small
}

void RenderLocalShadowMap::run(const render::RenderContextPointer& renderContext, const Inputs& inputs) {
    const auto& casters = inputs.get0();
    const auto& localShadows = inputs.get1();
    if (!localShadows || _tile >= localShadows->getTileCount() || !localShadows->getTile(_tile).light) {
        return;
    }
    RenderArgs* args = renderContext->args;
    args->_details._localShadowMaps++;
    const auto* plan = localShadows->planTile(_tile, casters, *renderContext->_scene);
    if (!plan) {
        return;   // the map in the tile still shows what the light sees
    }
    args->_details._localShadowMapsDrawn++;
    if (plan->redrawStatic) {
        args->_details._localShadowStaticMapsDrawn++;
    }

    glm::ivec4 viewport = localShadows->getTileViewport(_tile);
    glm::mat4 projection;
    Transform view;
    args->getViewFrustum().evalProjectionMatrix(projection);
    args->getViewFrustum().evalViewTransform(view);

    // the static casters' map first, when they or the view have changed: in a batch of its own, because shapes are
    // drawn instanced through the batch's named calls, which a batch runs at its end, with whatever framebuffer is
    // bound then
    if (plan->redrawStatic) {
        gpu::doInBatch("RenderLocalShadowMap::static", args->_context, [&](gpu::Batch& batch) {
            args->_batch = &batch;
            batch.enableStereo(false);
            batch.setFramebuffer(localShadows->getStaticFramebuffer());
            batch.setViewportTransform(viewport);
            batch.setStateScissorRect(viewport);
            batch.clearDepthFramebuffer(0.0f, true); // depth 0 is the far plane (reversed-Z)
            batch.setProjectionTransform(projection);
            batch.setViewTransform(view, false);
            renderShadowCasters(renderContext, _shapePlumber, plan->staticCasters);
            args->_batch = nullptr;
        });
    }

    // the working map: a copy of the static map, or cleared, with the dynamic casters (every caster, without static
    // caching) drawn over it
    gpu::doInBatch("RenderLocalShadowMap::run", args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;
        batch.enableStereo(false);
        batch.setFramebuffer(localShadows->getFramebuffer());
        batch.setViewportTransform(viewport);
        batch.setStateScissorRect(viewport);
        if (plan->copyStatic) {
            localShadows->copyStaticMap(batch, viewport);
        } else {
            batch.clearDepthFramebuffer(0.0f, true); // depth 0 is the far plane (reversed-Z)
        }
        if (!plan->dynamicCasters.empty()) {
            batch.setProjectionTransform(projection);
            batch.setViewTransform(view, false);
            renderShadowCasters(renderContext, _shapePlumber, plan->dynamicCasters);
        }
        args->_batch = nullptr;
    });
}

void RenderLocalShadowTeardown::run(const render::RenderContextPointer& renderContext, const Input& input) {
    RenderArgs* args = renderContext->args;
    if (args->_renderMode == RenderArgs::SHADOW_RENDER_MODE) {
        args->popViewFrustum();
    }
    args->_renderMode = input.get0();
}
