//
//  LightClusters.cpp
//
//  Created by Sam Gateau on 9/7/2016 (a frustum grid); Unity URP's Forward+ layout since 10/2026.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "LightClusters.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <gpu/Context.h>
#include <shaders/Shaders.h>
#include <graphics/ShaderConstants.h>

#include "RenderUtilsLogging.h"
#include "render-utils/ShaderConstants.h"
#include "DeferredLightingEffect.h"
#include "StencilMaskPass.h"


namespace ru {
    using render_utils::slot::texture::Texture;
    using render_utils::slot::buffer::Buffer;
}

namespace gr {
    using graphics::slot::texture::Texture;
    using graphics::slot::buffer::Buffer;
}

static_assert(LIGHT_CLUSTER_MAX_PROBES == ReflectionProbeStage::MAX_LAYERS, "the frame's probe list holds the array's layers");

namespace {
    constexpr int MAX_LIGHTS = LIGHT_CLUSTER_MAX_LIGHTS;
    constexpr int MAX_PROBES = LIGHT_CLUSTER_MAX_PROBES;
    constexpr int TILE_WORDS = LIGHT_CLUSTER_TILE_WORDS;
    constexpr int BIN_WORDS = LIGHT_CLUSTER_BIN_WORDS;
    constexpr int MIN_TILE_SIZE = 16;      // pixels; doubled until every eye's tiles fit their block
    constexpr int MAX_TILE_SIZE = 4096;
    constexpr int MAX_BINS = 1024;
    constexpr int BLOCK_SIZE_FOR_256_LIGHTS = 65536;   // bytes: URP's desktop cap needs 64 KB blocks, else its OpenGL 32
    constexpr int LIGHTS_WITH_SMALL_BLOCKS = 32;
    constexpr uint32_t EMPTY_BIN_HEADER = 0x0000FFFFu;   // first light 0xFFFF (none), last light 0

    // the ndc coordinate a direction from the eye projects to, on one axis: side is its x (or y), depth its distance
    // forward (positive), p00 the projection's [0][0] (or [1][1]) and p20 its [2][0] (or [2][1], the asymmetry)
    float projectDirection(float side, float depth, float p00, float p20) {
        return p00 * (side / depth) - p20;
    }

    // The screen bounds in ndc, along one axis, of a sphere seen from the eye: from the two tangents from the eye to
    // the sphere's circle in that axis' plane (its x, or y, and its depth; depthZ is the eye-space z, negative in
    // front). The whole screen when the eye is inside the circle, and to the screen's edge on a side whose tangent
    // points backward (the sphere then reaches past the edge)
    glm::vec2 sphereNdcBounds(float side, float depthZ, float radius, float p00, float p20) {
        const float NDC_MIN = -1.0f;
        const float NDC_MAX = 1.0f;
        const float len2 = side * side + depthZ * depthZ;
        if (len2 <= radius * radius) {
            return glm::vec2(NDC_MIN, NDC_MAX);
        }
        const float len = std::sqrt(len2);
        const glm::vec2 dir(side / len, depthZ / len);
        const float cosT = std::sqrt(len2 - radius * radius) / len;
        const float sinT = radius / len;
        // the direction to the centre turned by the tangent angle each way: toward the lower (left) side, and the upper
        const glm::vec2 lower(dir.x * cosT + dir.y * sinT, -dir.x * sinT + dir.y * cosT);
        const glm::vec2 upper(dir.x * cosT - dir.y * sinT, dir.x * sinT + dir.y * cosT);
        glm::vec2 bounds;
        bounds.x = (lower.y >= 0.0f) ? NDC_MIN : projectDirection(lower.x, -lower.y, p00, p20);
        bounds.y = (upper.y >= 0.0f) ? NDC_MAX : projectDirection(upper.x, -upper.y, p00, p20);
        return bounds;
    }

    // The plane through the eye along a tile boundary, at ndc x = a (columns: p00 = projection[0][0], p20 =
    // projection[2][0]) or ndc y = b (rows: the [1][1] and [2][1]); its normal points to the greater ndc (right, or up)
    glm::vec4 columnPlane(const glm::mat4& projection, float a) {
        return glm::vec4(glm::normalize(glm::vec3(projection[0][0], 0.0f, projection[2][0] + a)), 0.0f);
    }
    glm::vec4 rowPlane(const glm::mat4& projection, float b) {
        return glm::vec4(glm::normalize(glm::vec3(0.0f, projection[1][1], projection[2][1] + b)), 0.0f);
    }

    float distanceToPlane(const glm::vec3& point, const glm::vec4& plane) {
        return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
    }

    // The circle where a plane cuts a sphere (centre xyz, radius w); false when the sphere does not reach the plane
    bool reduceSphereToPlane(const glm::vec4& sphere, const glm::vec4& plane, glm::vec4& reducedSphere) {
        const float distance = distanceToPlane(glm::vec3(sphere), plane);
        if (std::abs(distance) <= sphere.w) {
            reducedSphere = glm::vec4(sphere.x - distance * plane.x, sphere.y - distance * plane.y, sphere.z - distance * plane.z,
                                      std::sqrt(std::max(0.0f, sphere.w * sphere.w - distance * distance)));
            return true;
        }
        return false;
    }
}

int LightClusters::getMaxLights() {
    const int blockSize = gpu::Context::getMaxUniformBlockSize();
    // before the GPU has said (no context yet), the Macs' answer
    if (blockSize <= 0 || blockSize >= BLOCK_SIZE_FOR_256_LIGHTS) {
        return MAX_LIGHTS;
    }
    return LIGHTS_WITH_SMALL_BLOCKS;
}

LightClusters::LightClusters() :
    _tiles(TILE_WORDS, 0u),
    _bins(BIN_WORDS, 0u) {
    // empty: no light, so the shaders read nothing else
    auto& params = _paramsBuffer.edit();
    memset(&params, 0, sizeof(params));
    _tilesBuffer = gpu::BufferView(std::make_shared<gpu::Buffer>(_tiles.size() * sizeof(uint32_t), (const gpu::Byte*)_tiles.data()));
    _binsBuffer = gpu::BufferView(std::make_shared<gpu::Buffer>(_bins.size() * sizeof(uint32_t), (const gpu::Byte*)_bins.data()));
}

void LightClusters::updateLightFrame(const LightStage::FramePointer& lightFrame, bool points, bool spots) {
    _visibleLightIndices.clear();
    if (points) {
        _visibleLightIndices.insert(_visibleLightIndices.end(), lightFrame->_pointLights.begin(), lightFrame->_pointLights.end());
    }
    if (spots) {
        _visibleLightIndices.insert(_visibleLightIndices.end(), lightFrame->_spotLights.begin(), lightFrame->_spotLights.end());
    }
}

const gpu::TexturePointer& LightClusters::getReflectionProbeArray() const {
    if (_probeStage && _probeStage->getArray()) {
        return _probeStage->getArray();
    }
    return ReflectionProbeStage::getEmptyArray();
}

LightClusters::Stats LightClusters::updateClusters(const RenderArgs* args) {
    Stats stats;
    _numLights = 0;
    _numProbes = 0;

    const bool stereo = args->isStereo();
    const int numEyes = stereo ? 2 : 1;
    const glm::ivec4 viewport = args->_viewport;

    // the eyes' viewports in framebuffer pixels: a stereo frame draws its two eyes side by side in the one viewport
    glm::ivec4 eyeViewports[2] = { viewport, viewport };
    if (stereo) {
        eyeViewports[0].z = viewport.z / 2;
        eyeViewports[1] = glm::ivec4(viewport.x + eyeViewports[0].z, viewport.y, eyeViewports[0].z, viewport.w);
    }
    const glm::ivec2 eyeSize(std::max(1, eyeViewports[0].z), std::max(1, eyeViewports[0].w));

    // world to the view's eye space; each eye's own space and projection in stereo
    Transform view;
    _frustum.evalViewTransform(view);
    const glm::mat4 worldToEye = view.getInverseMatrix();
    glm::mat4 eyeProjections[2];
    glm::mat4 eyeViews[2];
    if (stereo && args->_context) {
        args->_context->getStereoProjections(eyeProjections);
        args->_context->getStereoViews(eyeViews);
    } else {
        eyeProjections[0] = _frustum.getProjection();
        eyeViews[0] = glm::mat4(1.0f);
    }
    const float nearClip = std::max(_frustum.getNearClip(), 0.001f);
    const float farClip = std::max(_frustum.getFarClip(), nearClip * 2.0f);

    // a candidate's screen bounds in each eye, from its sphere; false when off every eye's screen
    auto projectCandidate = [&](Candidate& candidate) {
        bool visible = false;
        for (int eye = 0; eye < numEyes; eye++) {
            const glm::vec3 eyeCenter = glm::vec3(eyeViews[eye] * (worldToEye * glm::vec4(candidate.center, 1.0f)));
            const auto& projection = eyeProjections[eye];
            const glm::vec2 xBounds = sphereNdcBounds(eyeCenter.x, eyeCenter.z, candidate.radius, projection[0][0], projection[2][0]);
            const glm::vec2 yBounds = sphereNdcBounds(eyeCenter.y, eyeCenter.z, candidate.radius, projection[1][1], projection[2][1]);
            candidate.eyeCenter[eye] = eyeCenter;
            candidate.ndcBounds[eye] = glm::vec4(xBounds, yBounds);
            candidate.visible[eye] = xBounds.x <= 1.0f && xBounds.y >= -1.0f && yBounds.x <= 1.0f && yBounds.y >= -1.0f;
            visible = visible || candidate.visible[eye];
        }
        return visible;
    };

    // 1. Each candidate's bounds: a sphere (a spot's cone's enclosing sphere), its depth range (a spot's cone's own)
    // and its screen bounds in each eye; one out of the depth range, or off every eye's screen, is left out
    _candidates.clear();
    for (auto lightId : _visibleLightIndices) {
        auto light = _lightStage->getLight(lightId);
        if (!light) {
            continue;
        }
        stats.numInputLights++;

        Candidate candidate;
        candidate.id = lightId;
        const glm::vec3 position = light->getPosition();
        // the reach of the light's energy, not the entity's size: a light whose entity is far larger than its falloff lit
        // nothing in most of the tiles and bins it was put in, yet every fragment there ran its shadow and shading
        const float range = light->getCutoffRadius();
        const glm::vec3 eyePosition = glm::vec3(worldToEye * glm::vec4(position, 1.0f));
        const float depth = -eyePosition.z;
        candidate.center = position;
        candidate.radius = range;
        candidate.minZ = depth - range;
        candidate.maxZ = depth + range;

        if (light->isSpot()) {
            const glm::vec3 direction = glm::normalize(light->getDirection());
            const float cosAngle = glm::clamp(light->getSpotAngleCosSin().x, 0.0f, 1.0f);
            // the cone's enclosing sphere: under 45 degrees the sphere through its apex and the rim of its cap, wider
            // the sphere through the rim centred on the axis (a 90 degree cone's is the range sphere itself)
            const float COS_45_DEGREES = 0.70710678f;
            if (cosAngle > COS_45_DEGREES) {
                candidate.radius = range / (2.0f * cosAngle);
                candidate.center = position + direction * candidate.radius;
            } else {
                candidate.center = position + direction * (range * cosAngle);
                candidate.radius = range * std::sqrt(std::max(0.0f, 1.0f - cosAngle * cosAngle));
            }
            // its depth range: the apex, and its cap by the angle between the cone's axis and the view direction
            const glm::vec3 eyeDirection = glm::normalize(glm::vec3(worldToEye * glm::vec4(direction, 0.0f)));
            const float axisAngle = std::acos(glm::clamp(-eyeDirection.z, -1.0f, 1.0f));
            const float coneAngle = std::acos(cosAngle);
            const float capMax = depth + range * std::cos(std::max(0.0f, axisAngle - coneAngle));
            const float capMin = depth + range * std::cos(std::min(glm::pi<float>(), axisAngle + coneAngle));
            candidate.minZ = std::min(depth, capMin);
            candidate.maxZ = std::max(depth, capMax);
        }
        if (candidate.maxZ <= nearClip || candidate.minZ >= farClip) {
            continue;
        }
        if (!projectCandidate(candidate)) {
            continue;
        }
        _candidates.push_back(candidate);
    }

    // 1b. The reflection probes with a layer in the array: the sphere round each one's box, the same tests
    _probeCandidates.clear();
    if (_probeStage && _probeStage->getArray()) {
        const int numAllocated = _probeStage->getNumAllocatedProbes();
        for (int i = 0; i < numAllocated; i++) {
            if (!_probeStage->checkProbeId(i)) {
                continue;
            }
            auto probe = _probeStage->getProbe(i);
            const int layer = _probeStage->getLayer(i);
            if (!probe || layer < 0) {
                continue;
            }
            stats.numInputProbes++;

            Candidate candidate;
            candidate.probe = probe;
            candidate.layer = layer;
            candidate.center = probe->getBoxCenter();
            candidate.radius = probe->getBoxRadius();
            const float depth = -(worldToEye * glm::vec4(candidate.center, 1.0f)).z;
            candidate.minZ = depth - candidate.radius;
            candidate.maxZ = depth + candidate.radius;
            if (candidate.maxZ <= nearClip || candidate.minZ >= farClip) {
                continue;
            }
            if (!projectCandidate(candidate)) {
                continue;
            }
            _probeCandidates.push_back(candidate);
        }
        // by importance, the most important first, then the smallest first: the order a fragment blends them in,
        // each taking the weight the ones before left (ReflectionProbe.slh); the least important go over the cap
        std::sort(_probeCandidates.begin(), _probeCandidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.probe->importance != b.probe->importance) {
                return a.probe->importance > b.probe->importance;
            }
            return a.probe->getBoxVolume() < b.probe->getBoxVolume();
        });
        if ((int)_probeCandidates.size() > MAX_PROBES) {
            _probeCandidates.resize(MAX_PROBES);
        }
    }
    const int numProbes = (int)_probeCandidates.size();
    _numProbes = numProbes;
    stats.numProbes = numProbes;

    // 2. The list: sorted by where each light's depth range starts, so a bin's lights lie in a short run of it (which
    // its header names), and no longer than the cap: the farthest-starting lights go
    std::sort(_candidates.begin(), _candidates.end(), [](const Candidate& a, const Candidate& b) { return a.minZ < b.minZ; });
    const int maxLights = std::min(getMaxLights(), MAX_LIGHTS);
    if ((int)_candidates.size() > maxLights) {
        _candidates.resize(maxLights);
    }
    const int numLights = (int)_candidates.size();
    _numLights = numLights;
    stats.numLights = numLights;

    // 3. The layout: the words a tile or bin holds (one bit a light, then from the next whole word one a probe), the
    // tile size (doubled from 16 px until every eye's tiles fit their block), the bins (as many as fit theirs, up to
    // 1024, logarithmic from the near clip to the far: each holds the same ratio of depths)
    const int probeBase = (numProbes > 0) ? ((numLights + 31) / 32) * 32 : 0;
    const int wordsPerEntry = std::max(1, (std::max(numLights, probeBase + numProbes) + 31) / 32);
    const int blockSize = gpu::Context::getMaxUniformBlockSize();
    const int tileWordBudget = (blockSize > 0) ? std::min(TILE_WORDS, blockSize / 4) : TILE_WORDS;
    int tileSize = MIN_TILE_SIZE;
    glm::ivec2 tileCount;
    for (;; tileSize *= 2) {
        tileCount = (eyeSize + tileSize - 1) / tileSize;
        if (tileCount.x * tileCount.y * numEyes * wordsPerEntry <= tileWordBudget || tileSize >= MAX_TILE_SIZE) {
            break;
        }
    }
    const int tilesPerEye = tileCount.x * tileCount.y;
    const int binStride = wordsPerEntry + 2;   // two headers: the lights' and the probes'
    const int binCount = std::max(1, std::min(MAX_BINS, BIN_WORDS / binStride));
    const float binScale = (float)binCount / std::log2(farClip / nearClip);
    const float binOffset = -std::log2(nearClip) * binScale;
    auto binOf = [&](float depth) {
        return glm::clamp((int)std::floor(std::log2(std::max(depth, nearClip)) * binScale + binOffset), 0, binCount - 1);
    };
    stats.tileSize = tileSize;
    stats.tileCountX = tileCount.x;
    stats.tileCountY = tileCount.y;
    stats.binCount = binCount;
    stats.wordsPerEntry = wordsPerEntry;

    // 4. The bins: a light's (or probe's) bit in every bin its depth range crosses, and the bin's header widened to it
    const int usedBinWords = binCount * binStride;
    std::fill(_bins.begin(), _bins.begin() + usedBinWords, 0u);
    for (int bin = 0; bin < binCount; bin++) {
        _bins[bin * binStride] = EMPTY_BIN_HEADER;
        _bins[bin * binStride + 1] = EMPTY_BIN_HEADER;
    }
    auto binCandidate = [&](const Candidate& candidate, int entry, int headerWord) {
        const uint32_t bit = 1u << (entry & 31);
        const int word = 2 + (entry >> 5);
        const int lastBin = binOf(candidate.maxZ);
        for (int bin = binOf(candidate.minZ); bin <= lastBin; bin++) {
            uint32_t& header = _bins[bin * binStride + headerWord];
            header = std::min(header & 0xFFFFu, (uint32_t)entry) | (std::max(header >> 16, (uint32_t)entry) << 16);
            _bins[bin * binStride + word] |= bit;
        }
    };
    for (int i = 0; i < numLights; i++) {
        binCandidate(_candidates[i], i, 0);
    }
    for (int i = 0; i < numProbes; i++) {
        binCandidate(_probeCandidates[i], probeBase + i, 1);
    }

    // 5. The tiles, eye by eye: a light's rows and columns from its screen bounds, and within them, row by row, the
    // columns its sphere reaches once cut down to the row's nearer boundary plane (the slice of the sphere a row sees
    // is never wider on screen than that circle)
    const int usedTileWords = tilesPerEye * numEyes * wordsPerEntry;
    std::fill(_tiles.begin(), _tiles.begin() + usedTileWords, 0u);
    for (int eye = 0; eye < numEyes; eye++) {
        const auto& projection = eyeProjections[eye];
        const float p11 = projection[1][1];
        const float p21 = projection[2][1];

        // the planes through the eye along the tiles' boundaries, by their ndc
        _columnPlanes.resize(tileCount.x + 1);
        _rowPlanes.resize(tileCount.y + 1);
        for (int k = 0; k <= tileCount.x; k++) {
            _columnPlanes[k] = columnPlane(projection, 2.0f * (float)(k * tileSize) / (float)eyeSize.x - 1.0f);
        }
        for (int j = 0; j <= tileCount.y; j++) {
            _rowPlanes[j] = rowPlane(projection, 2.0f * (float)(j * tileSize) / (float)eyeSize.y - 1.0f);
        }
        auto tileOfNdc = [&](float ndc, int axis) {
            return (int)std::floor((ndc + 1.0f) * 0.5f * (float)eyeSize[axis] / (float)tileSize);
        };

        auto scanTiles = [&](const Candidate& candidate, int entry) {
            if (!candidate.visible[eye]) {
                return;
            }
            const auto& bounds = candidate.ndcBounds[eye];
            const int xMin = glm::clamp(tileOfNdc(bounds.x, 0), 0, tileCount.x - 1);
            const int xMax = glm::clamp(tileOfNdc(bounds.y, 0), 0, tileCount.x - 1);
            const int yMin = glm::clamp(tileOfNdc(bounds.z, 1), 0, tileCount.y - 1);
            const int yMax = glm::clamp(tileOfNdc(bounds.w, 1), 0, tileCount.y - 1);

            // the centre's row (a centre at or behind the eye taken as if in front, as the frustum grid did)
            const glm::vec3 center = candidate.eyeCenter[eye];
            const float centerDepth = std::max(std::abs(center.z), 1.0e-4f);
            const int centerY = glm::clamp(tileOfNdc(projectDirection(center.y, centerDepth, p11, p21), 1), yMin, yMax);
            const glm::vec4 sphere(center, candidate.radius);
            const uint32_t bit = 1u << (entry & 31);
            const int word = entry >> 5;

            for (int y = yMin; y <= yMax; y++) {
                glm::vec4 rowSphere = sphere;
                if (y != centerY) {
                    const glm::vec4 plane = (y < centerY) ? _rowPlanes[y + 1] : -_rowPlanes[y];
                    if (!reduceSphereToPlane(sphere, plane, rowSphere)) {
                        continue;
                    }
                }
                const glm::vec3 rowCenter(rowSphere);
                const float rowRadius = rowSphere.w;
                // the first column the circle is not entirely right of (its right boundary), the last it is not
                // entirely left of (its left boundary)
                int x0 = xMin;
                for (; x0 < xMax; x0++) {
                    if (distanceToPlane(rowCenter, _columnPlanes[x0 + 1]) - rowRadius <= 0.0f) {
                        break;
                    }
                }
                int x1 = xMax;
                for (; x1 > x0; x1--) {
                    if (distanceToPlane(rowCenter, _columnPlanes[x1]) + rowRadius >= 0.0f) {
                        break;
                    }
                }
                uint32_t* row = _tiles.data() + (eye * tilesPerEye + y * tileCount.x) * wordsPerEntry + word;
                for (int x = x0; x <= x1; x++) {
                    row[x * wordsPerEntry] |= bit;
                }
                stats.numTileBits += x1 - x0 + 1;
            }
        };
        for (int i = 0; i < numLights; i++) {
            scanTiles(_candidates[i], i);
        }
        for (int i = 0; i < numProbes; i++) {
            scanTiles(_probeCandidates[i], probeBase + i);
        }
    }

    // 6. To the GPU: the parameters with the list, and the words in use of the tiles and the bins
    auto& params = _paramsBuffer.edit();
    params.tileInfo = glm::vec4(1.0f / (float)tileSize, 1.0f / (float)tileSize, (float)tileCount.x, (float)tileCount.y);
    params.binInfo = glm::vec4(binScale, binOffset, (float)(binCount - 1), 0.0f);
    params.counts = glm::ivec4(numLights, wordsPerEntry, tilesPerEye, stereo ? 1 : 0);
    params.probeCounts = glm::ivec4(probeBase, numProbes, _probeStage ? _probeStage->getArrayNumMips() : 0, 0);
    params.viewports[0] = glm::vec4(eyeViewports[0]);
    params.viewports[1] = glm::vec4(eyeViewports[1]);
    params.worldToEye = worldToEye;
    for (int i = 0; i < numLights; i++) {
        params.lightIndices[i >> 2][i & 3] = _candidates[i].id;
    }
    for (int i = 0; i < numProbes; i++) {
        const auto& probe = *_probeCandidates[i].probe;
        auto& entry = params.probes[i];
        // the box in the probe's frame, the probe at the origin; world to that frame = turn back, after moving back
        const glm::vec3 halfSize = 0.5f * glm::max(probe.boxSize, glm::vec3(0.0f));
        entry.position = glm::vec4(probe.position, (float)_probeCandidates[i].layer);
        entry.boxMin = glm::vec4(probe.boxOffset - halfSize, std::max(probe.blendDistance, 1.0e-4f));
        entry.boxMax = glm::vec4(probe.boxOffset + halfSize, probe.boxProjection ? 1.0f : 0.0f);
        entry.params = glm::vec4(probe.intensity, 0.0f, 0.0f, 0.0f);
        entry.worldToProbe = glm::toMat4(glm::inverse(probe.orientation)) * glm::translate(glm::mat4(1.0f), -probe.position);
    }
    _tilesBuffer._buffer->setSubData(0, usedTileWords * sizeof(uint32_t), (const gpu::Byte*)_tiles.data());
    _binsBuffer._buffer->setSubData(0, usedBinWords * sizeof(uint32_t), (const gpu::Byte*)_bins.data());

    return stats;
}



LightClusteringPass::LightClusteringPass() {
    _lightClusters = std::make_shared<LightClusters>();
}


void LightClusteringPass::configure(const Config& config) {
    _freeze = config.freeze;
}

void LightClusteringPass::run(const render::RenderContextPointer& renderContext, const Input& inputs, Output& output) {
    auto args = renderContext->args;

    auto lightingModel = inputs.get1();
    auto lightFrame = inputs.get2();

    auto lightStage = renderContext->_scene->getStage<LightStage>();
    assert(lightStage);
    _lightClusters->updateLightStage(lightStage);

    auto config = std::static_pointer_cast<Config>(renderContext->jobConfig);
    config->numSceneLights = lightStage->getNumLights();
    config->numFreeSceneLights = lightStage->getNumFreeLights();
    config->numAllocatedSceneLights = lightStage->getNumAllocatedLights();

    // frozen, the last build stands while the view moves on
    if (!_freeze) {
        // the scene's reflection probes and their cube map array, brought up to date first
        auto probeStage = renderContext->_scene->getStage<ReflectionProbeStage>();
        if (probeStage) {
            probeStage->updateArray();
        }
        _lightClusters->updateFrustum(args->getViewFrustum());
        _lightClusters->updateLightFrame(lightFrame, lightingModel->isPointLightEnabled(), lightingModel->isSpotLightEnabled());
        _lightClusters->updateReflectionProbes(probeStage);
        const auto stats = _lightClusters->updateClusters(args);

        config->numInputLights = stats.numInputLights;
        config->numClusteredLights = stats.numLights;
        config->numClusteredLightReferences = stats.numTileBits;
        config->numInputProbes = stats.numInputProbes;
        config->numClusteredProbes = stats.numProbes;
        config->maxLights = LightClusters::getMaxLights();
        config->tileSize = stats.tileSize;
        config->tileCountX = stats.tileCountX;
        config->tileCountY = stats.tileCountY;
        config->binCount = stats.binCount;
        config->wordsPerEntry = stats.wordsPerEntry;
    }

    output = _lightClusters;
}

DebugLightClusters::DebugLightClusters() {

}


void DebugLightClusters::configure(const Config& config) {
    doDrawClusterFromDepth = config.doDrawClusterFromDepth;
    doDrawContent = config.doDrawContent;
}

const gpu::PipelinePointer DebugLightClusters::getDrawClusterFromDepthPipeline() {
    if (!_drawClusterFromDepth) {
        gpu::ShaderPointer program = gpu::Shader::createProgram(shader::render_utils::program::lightClusters_drawClusterFromDepth);
        auto state = std::make_shared<gpu::State>();
        
        // Blend on transparent
        state->setBlendFunction(true, gpu::State::SRC_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::INV_SRC_ALPHA);

        // Good to go add the brand new pipeline
        _drawClusterFromDepth = gpu::Pipeline::create(program, state);
    }
    return _drawClusterFromDepth;
}

const gpu::PipelinePointer DebugLightClusters::getDrawClusterContentPipeline() {
    if (!_drawClusterContent) {
        gpu::ShaderPointer program = gpu::Shader::createProgram(shader::render_utils::program::lightClusters_drawClusterContent);
        auto state = std::make_shared<gpu::State>();
        
        // Blend on transparent
        state->setBlendFunction(true, gpu::State::SRC_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::INV_SRC_ALPHA);

        // Good to go add the brand new pipeline
        _drawClusterContent = gpu::Pipeline::create(program, state);
    }
    return _drawClusterContent;
}


void DebugLightClusters::run(const render::RenderContextPointer& renderContext, const Inputs& inputs) {
    if (!(doDrawClusterFromDepth || doDrawContent)) {
        return;
    }

    auto deferredTransform = inputs.get0();
    auto linearDepthTarget = inputs.get2();
    auto lightClusters = inputs.get3();
    if (!lightClusters || !lightClusters->_lightStage || !linearDepthTarget) {
        return;
    }

    auto args = renderContext->args;

    gpu::doInBatch("DebugLightClusters::run", args->_context, [&](gpu::Batch& batch) {
        batch.enableStereo(false);
        batch.setViewportTransform(args->_viewport);
        batch.setStateScissorRect(args->_viewport);

        // the light array and the culling, as the local-light pass binds them
        batch.setUniformBuffer(gr::Buffer::Light, lightClusters->_lightStage->getLightArrayBuffer());
        batch.setUniformBuffer(ru::Buffer::LightClusterParams, lightClusters->_paramsBuffer);
        batch.setUniformBuffer(ru::Buffer::LightClusterTiles, lightClusters->_tilesBuffer);
        batch.setUniformBuffer(ru::Buffer::LightClusterBins, lightClusters->_binsBuffer);
        batch.setUniformBuffer(ru::Buffer::DeferredFrameTransform, deferredTransform->getFrameTransformBuffer());
        batch.setResourceTexture(ru::Texture::DeferredLinearZEye, linearDepthTarget->getLinearDepthTexture());

        if (doDrawClusterFromDepth) {
            batch.setPipeline(getDrawClusterFromDepthPipeline());
            batch.draw(gpu::TRIANGLE_STRIP, 4, 0);
        }
        if (doDrawContent) {
            batch.setPipeline(getDrawClusterContentPipeline());
            batch.draw(gpu::TRIANGLE_STRIP, 4, 0);
        }

        batch.setResourceTexture(ru::Texture::DeferredLinearZEye, nullptr);
        batch.setUniformBuffer(ru::Buffer::DeferredFrameTransform, nullptr);
        batch.setUniformBuffer(gr::Buffer::Light, nullptr);
        batch.setUniformBuffer(ru::Buffer::LightClusterParams, nullptr);
        batch.setUniformBuffer(ru::Buffer::LightClusterTiles, nullptr);
        batch.setUniformBuffer(ru::Buffer::LightClusterBins, nullptr);
    });
}
