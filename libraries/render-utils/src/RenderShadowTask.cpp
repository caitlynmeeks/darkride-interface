//
//  RenderShadowTask.cpp
//  render-utils/src/
//
//  Created by Zach Pomerantz on 1/7/2016.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: RenderShadowSetup::run (the far cascades' turns and holds), RenderShadowCascadeSetup::run (a waiting
// cascade culled by the frustum it holds) and RenderShadowMap::run (the cache check; a waiting cascade redrawn only
// when one of its casters moved or left).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.

#include "RenderShadowTask.h"

#include <gpu/Context.h>

#include <algorithm>

#include <ViewFrustum.h>

#include <render/CullTask.h>
#include <render/SortTask.h>
#include <render/DrawTask.h>
#include <render/Scene.h>
#include <render/TransitionStage.h>

#include "DeferredLightingEffect.h"
#include "FramebufferCache.h"

#include "RenderUtilsLogging.h"

#include "RenderCommonTask.h"
#include "AssembleLightingStageTask.h"

#include "FadeEffect.h"

// These values are used for culling the objects rendered in the shadow map
// but are readjusted afterwards
#define SHADOW_FRUSTUM_NEAR 1.0f
#define SHADOW_FRUSTUM_FAR  500.0f

using namespace render;

extern void initZPassPipelines(ShapePlumber& plumber, gpu::StatePointer state, const render::ShapePipeline::BatchSetter& batchSetter, const render::ShapePipeline::ItemSetter& itemSetter);

void RenderShadowTask::configure(const Config& configuration) {
    //DependencyManager::get<DeferredLightingEffect>()->setShadowMapEnabled(configuration.isEnabled());
    // This is a task, so must still propogate configure() to its Jobs
    //    Task::configure(configuration);
}

void RenderShadowTask::build(JobModel& task, const render::Varying& input, render::Varying& output, render::CullFunctor cameraCullFunctor, uint8_t tagBits, uint8_t tagMask) {
    // Prepare the ShapePipeline
    ShapePlumberPointer shapePlumber = std::make_shared<ShapePlumber>();
    {
        auto state = std::make_shared<gpu::State>();
        state->setCullMode(gpu::State::CULL_BACK);
        state->setDepthTest(true, true, gpu::GREATER_EQUAL);

        auto fadeEffect = DependencyManager::get<FadeEffect>();
        initZPassPipelines(*shapePlumber, state, fadeEffect->getBatchSetter(), fadeEffect->getItemUniformSetter());
    }
    const auto setupOutput = task.addJob<RenderShadowSetup>("ShadowSetup", input);
    const auto queryResolution = setupOutput.getN<RenderShadowSetup::Output>(1);
    const auto shadowFrame = setupOutput.getN<RenderShadowSetup::Output>(3);
    const auto currentKeyLight = setupOutput.getN<RenderShadowSetup::Output>(4);
    // Fetch and cull the items from the scene

    // Translucent items cast too, as in Unity's URP: solid, or cut out by the opacity mask when the material has one
    // (the depth-only shaders read the mask); CullShadowBounds lets only the ones with a material cast
    static const auto shadowCasterReceiverFilter = ItemFilter::Builder::visibleWorldItems().withoutLayered().withTagBits(tagBits, tagMask);

    const auto fetchInput = FetchSpatialTree::Inputs(shadowCasterReceiverFilter, queryResolution).asVarying();
    const auto shadowSelection = task.addJob<FetchSpatialTree>("FetchShadowTree", fetchInput);
    const auto selectionInputs = CullSpatialSelection::Inputs(shadowSelection, shadowCasterReceiverFilter).asVarying();
    const auto shadowItems = task.addJob<CullSpatialSelection>("FilterShadowSelection", selectionInputs, nullptr, true, RenderDetails::SHADOW);

    // Cull objects that are not visible in camera view. Hopefully the cull functor only performs LOD culling, not
    // frustum culling or this will make shadow casters out of the camera frustum disappear.
    const auto cameraFrustum = setupOutput.getN<RenderShadowSetup::Output>(2);
    const auto applyFunctorInputs = ApplyCullFunctorOnItemBounds::Inputs(shadowItems, cameraFrustum).asVarying();
    const auto culledShadowItems = task.addJob<ApplyCullFunctorOnItemBounds>("ShadowCullCamera", applyFunctorInputs, cameraCullFunctor);

    // Sort
    const auto sortedPipelines = task.addJob<PipelineSortShapes>("PipelineSortShadow", culledShadowItems);
    const auto sortedShapes = task.addJob<DepthSortShapes>("DepthSortShadow", sortedPipelines, true);

    render::Varying cascadeFrustums[SHADOW_CASCADE_MAX_COUNT] = {
        ViewFrustumPointer()
#if SHADOW_CASCADE_MAX_COUNT>1
        ,ViewFrustumPointer(),
        ViewFrustumPointer(),
        ViewFrustumPointer()
#endif
    };

    CascadeBoxes cascadeSceneBBoxes;

    for (auto i = 0; i < SHADOW_CASCADE_MAX_COUNT; i++) {
        char jobName[64];
        sprintf(jobName, "ShadowCascadeSetup%d", i);
        const auto cascadeSetupOutput = task.addJob<RenderShadowCascadeSetup>(jobName, shadowFrame, i, shadowCasterReceiverFilter);
        const auto shadowFilter = cascadeSetupOutput.getN<RenderShadowCascadeSetup::Outputs>(0);
        auto antiFrustum = render::Varying(ViewFrustumPointer());
        cascadeFrustums[i] = cascadeSetupOutput.getN<RenderShadowCascadeSetup::Outputs>(1);
        if (i > 1) {
            antiFrustum = cascadeFrustums[i - 2];
        }

        const auto cullInputs = CullShadowBounds::Inputs(sortedShapes, shadowFilter, antiFrustum, currentKeyLight, cascadeSetupOutput.getN<RenderShadowCascadeSetup::Outputs>(2)).asVarying();
        sprintf(jobName, "CullShadowCascade%d", i);
        const auto culledShadowItemsAndBounds = task.addJob<CullShadowBounds>(jobName, cullInputs);

        // GPU jobs: Render to shadow map
        sprintf(jobName, "RenderShadowMap%d", i);
        const auto shadowInputs = RenderShadowMap::Inputs(culledShadowItemsAndBounds.getN<CullShadowBounds::Outputs>(0),
            culledShadowItemsAndBounds.getN<CullShadowBounds::Outputs>(1), shadowFrame).asVarying();
        task.addJob<RenderShadowMap>(jobName, shadowInputs, shapePlumber, i);
        sprintf(jobName, "ShadowCascadeTeardown%d", i);
        task.addJob<RenderShadowCascadeTeardown>(jobName, shadowFilter);

        cascadeSceneBBoxes[i] = culledShadowItemsAndBounds.getN<CullShadowBounds::Outputs>(1);
    }
    task.addJob<RenderShadowTeardown>("ShadowTeardown", setupOutput);


    output = Output(cascadeSceneBBoxes, setupOutput.getN<RenderShadowSetup::Output>(3));

}

static void computeNearFar(const Triangle& triangle, const Plane shadowClipPlanes[4], float& near, float& far) {
    static const int MAX_TRIANGLE_COUNT = 16;
    Triangle clippedTriangles[MAX_TRIANGLE_COUNT];
    auto clippedTriangleCount = clipTriangleWithPlanes(triangle, shadowClipPlanes, 4, clippedTriangles, MAX_TRIANGLE_COUNT);

    for (auto i = 0; i < clippedTriangleCount; i++) {
        const auto& clippedTriangle = clippedTriangles[i];

        near = glm::min(near, -clippedTriangle.v0.z);
        near = glm::min(near, -clippedTriangle.v1.z);
        near = glm::min(near, -clippedTriangle.v2.z);

        far = glm::max(far, -clippedTriangle.v0.z);
        far = glm::max(far, -clippedTriangle.v1.z);
        far = glm::max(far, -clippedTriangle.v2.z);
    }
}

static void computeNearFar(const glm::vec3 sceneBoundVertices[8], const Plane shadowClipPlanes[4], float& near, float& far) {
    // This code is inspired from Microsoft's CascadedShadowMaps11 sample which is under MIT licence.
    // See https://code.msdn.microsoft.com/windowsdesktop/Direct3D-Shadow-Win32-2d72a4f2/sourcecode?fileId=121915&pathId=1645833187
    // Basically it decomposes the object bounding box in triangles and clips each triangle with the shadow
    // frustum planes. Finally it computes the minimum and maximum depth of the clipped triangle vertices 
    // in shadow space to extract the near and far distances of the shadow frustum.
    static const std::array<int[4], 6> boxQuadVertexIndices = { {
        { TOP_LEFT_FAR, BOTTOM_LEFT_FAR, BOTTOM_RIGHT_FAR, TOP_RIGHT_FAR },
        { TOP_LEFT_NEAR, BOTTOM_LEFT_NEAR, BOTTOM_RIGHT_NEAR, TOP_RIGHT_NEAR },
        { TOP_RIGHT_FAR, BOTTOM_RIGHT_FAR, BOTTOM_RIGHT_NEAR, TOP_RIGHT_NEAR },
        { TOP_LEFT_FAR, BOTTOM_LEFT_FAR, BOTTOM_LEFT_NEAR, TOP_LEFT_NEAR },
        { BOTTOM_LEFT_FAR, BOTTOM_RIGHT_FAR, BOTTOM_RIGHT_NEAR, BOTTOM_LEFT_NEAR },
        { TOP_LEFT_FAR, TOP_RIGHT_FAR, TOP_RIGHT_NEAR, TOP_LEFT_NEAR }
    }  };
    Triangle triangle;

    for (auto quadVertexIndices : boxQuadVertexIndices) {
        triangle.v0 = sceneBoundVertices[quadVertexIndices[0]];
        triangle.v1 = sceneBoundVertices[quadVertexIndices[1]];
        triangle.v2 = sceneBoundVertices[quadVertexIndices[2]];
        computeNearFar(triangle, shadowClipPlanes, near, far);
        triangle.v1 = sceneBoundVertices[quadVertexIndices[3]];
        computeNearFar(triangle, shadowClipPlanes, near, far);
    }
}

static void adjustNearFar(const AABox& inShapeBounds, ViewFrustum& shadowFrustum) {
    if (!inShapeBounds.isNull()) {
        const Transform shadowView{ shadowFrustum.getView() };
        const Transform shadowViewInverse{ shadowView.getInverseMatrix() };

        glm::vec3 sceneBoundVertices[8];
        // Keep only the left, right, top and bottom shadow frustum planes as we wish to determine
        // the near and far
        Plane shadowClipPlanes[4];
        int i;

        // The vertices of the scene bounding box are expressed in the shadow frustum's local space
        for (i = 0; i < 8; i++) {
            sceneBoundVertices[i] = shadowViewInverse.transform(inShapeBounds.getVertex(static_cast<BoxVertex>(i)));
        }
        shadowFrustum.getUniformlyTransformedSidePlanes(shadowViewInverse, shadowClipPlanes);

        float near = std::numeric_limits<float>::max();
        float far = 0.0f;

        computeNearFar(sceneBoundVertices, shadowClipPlanes, near, far);
        // Limit the far range to the one used originally.
        far = glm::min(far, shadowFrustum.getFarClip());
        if (near > far) {
            near = far;
        }

        const auto depthEpsilon = 0.1f;
        auto projMatrix = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, near - depthEpsilon, far + depthEpsilon);
        auto shadowProjection = shadowFrustum.getProjection();

        shadowProjection[2][2] = projMatrix[2][2];
        shadowProjection[3][2] = projMatrix[3][2];
        shadowFrustum.setProjection(shadowProjection);
        shadowFrustum.calculate();
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void RenderShadowMap::run(const render::RenderContextPointer& renderContext, const Inputs& inputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    const auto& inShapes = inputs.get0();
    const auto& inShapeBounds = inputs.get1();
    const auto& shadowFrame = inputs.get2();

    LightStage::ShadowPointer shadow;
    if (shadowFrame && !shadowFrame->_objects.empty()) {
        shadow = shadowFrame->_objects.front();
    }
    if (!shadow || _cascadeIndex >= shadow->getCascadeCount()) {
        return;
    }

    auto& cascade = shadow->getCascade(_cascadeIndex);
    auto& fbo = cascade.framebuffer;

    RenderArgs* args = renderContext->args;

    // The casters in this cascade's frustum, and whether any of them changed or is fading since its map was drawn
    const auto& scene = renderContext->_scene;
    const uint64_t frame = scene->getFrame();
    _casterIds.clear();
    bool castersChanged = false;
    for (const auto& shapes : inShapes) {
        for (const auto& bound : shapes.second) {
            const auto& item = scene->getItem(bound.id);
            _casterIds.push_back(bound.id);
            castersChanged = castersChanged || item.getChangeFrame() > _drawnFrame ||
                !render::TransitionStage::isIndexInvalid(item.getTransitionId());
        }
    }
    std::sort(_casterIds.begin(), _casterIds.end());

    // A far cascade waiting for its turn (RenderShadowSetup) keeps its map and the matrix the shaders read it by,
    // unless one of its casters changed since the map was drawn: then the map is drawn again now, by the frustum the
    // cascade holds (RenderShadowCascadeSetup culled by it and pushed it), so that a moving caster's shadow moves
    // every frame wherever it falls, while a still scene keeps the saving. Until 2026-10-31 the hold was
    // unconditional and the monorail car's shadow on the rail, in a far cascade with a low sun, moved every other
    // frame while the car moved every frame (Caity: "the shadow beneath the carriage is juddering real bad").
    // Nothing of a held cascade is touched before this return: its frustum must stay the one its map was drawn by.
    // A mover that left the frustum this frame is not in the list, so its change would not be seen and its shadow
    // would stay in the held map a frame longer: a caster of the drawn map now missing AND changed since the draw
    // (a deleted one stamps its change frame too) counts as changed. Only a changed one: an item the nearer
    // cascade's frustum came to cover (the anti-frustum moves with the camera) has not moved and is no reason to
    // draw again. Both lists are sorted: one merge walk, getItem only for the missing ones.
    if (cascade.isHeld() && !castersChanged) {
        auto present = _casterIds.begin();
        for (auto drawnId : _drawnCasters) {
            while (present != _casterIds.end() && *present < drawnId) {
                ++present;
            }
            if ((present == _casterIds.end() || *present != drawnId) && scene->getItem(drawnId).getChangeFrame() > _drawnFrame) {
                castersChanged = true;
                break;
            }
        }
    }
    if (cascade.isHeld() && !castersChanged) {
        return;
    }

    auto adjustedShadowFrustum = args->getViewFrustum();
    if (!cascade.isHeld()) {
        // Adjust the frustum near and far depths based on the rendered items bounding box to have
        // the minimal Z range.
        adjustNearFar(inShapeBounds, adjustedShadowFrustum);
        // Reapply the frustum as it has been adjusted
        shadow->setCascadeFrustum(_cascadeIndex, adjustedShadowFrustum);
        args->popViewFrustum();
        args->pushViewFrustum(adjustedShadowFrustum);
    }

    // With caching the map stays while it would show the same: the view it was drawn from, the same casters, and
    // none of them changed or fading since (a stable fit keeps the view the same while the camera is still)
    const glm::mat4 view = adjustedShadowFrustum.getView();
    const glm::mat4 projection = adjustedShadowFrustum.getProjection();
    if (shadow->isCacheCascades() && _drawn && !castersChanged && view == _drawnView && projection == _drawnProjection &&
            _casterIds == _drawnCasters) {
        return;
    }
    _drawn = true;
    _drawnView = view;
    _drawnProjection = projection;
    _drawnCasters.swap(_casterIds);
    _drawnFrame = frame;
    shadow->noteCascadeDrawn();

    gpu::doInBatch("RenderShadowMap::run", args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;
        batch.enableStereo(false);

        glm::ivec4 viewport{0, 0, fbo->getWidth(), fbo->getHeight()};
        batch.setViewportTransform(viewport);
        batch.setStateScissorRect(viewport);

        batch.setFramebuffer(fbo);
        batch.clearDepthFramebuffer(0.0f, false); // depth 0 is the far plane (reversed-Z)

        if (!inShapeBounds.isNull()) {
            glm::mat4 projMat;
            Transform viewMat;
            args->getViewFrustum().evalProjectionMatrix(projMat);
            args->getViewFrustum().evalViewTransform(viewMat);

            batch.setProjectionTransform(projMat);
            batch.setViewTransform(viewMat, false);

            renderShadowCasters(renderContext, _shapePlumber, inShapes);
        }

        args->_batch = nullptr;
    });
}

void renderShadowCasters(const render::RenderContextPointer& renderContext, const render::ShapePlumberPointer& shapePlumber,
                         const render::ShapeBounds& inShapes) {
    RenderArgs* args = renderContext->args;

    const std::vector<ShapeKey::Builder> keys = {
        ShapeKey::Builder(), ShapeKey::Builder().withFade(),
        ShapeKey::Builder().withDeformed(), ShapeKey::Builder().withDeformed().withFade(),
        ShapeKey::Builder().withDeformed().withDualQuatSkinned(), ShapeKey::Builder().withDeformed().withDualQuatSkinned().withFade(),
        ShapeKey::Builder().withOwnPipeline(), ShapeKey::Builder().withOwnPipeline().withFade(),
        ShapeKey::Builder().withDeformed().withOwnPipeline(), ShapeKey::Builder().withDeformed().withOwnPipeline().withFade(),
        ShapeKey::Builder().withDeformed().withDualQuatSkinned().withOwnPipeline(), ShapeKey::Builder().withDeformed().withDualQuatSkinned().withOwnPipeline().withFade(),
    };
    std::vector<std::vector<ShapeKey>> sortedShapeKeys(keys.size());

    const int OWN_PIPELINE_INDEX = 6;
    for (const auto& items : inShapes) {
        int index = items.first.hasOwnPipeline() ? OWN_PIPELINE_INDEX : 0;
        if (items.first.isDeformed()) {
            index += 2;
            if (items.first.isDualQuatSkinned()) {
                index += 2;
            }
        }

        if (items.first.isFaded()) {
            index += 1;
        }

        sortedShapeKeys[index].push_back(items.first);
    }

    // Render non-withOwnPipeline things
    for (size_t i = 0; i < OWN_PIPELINE_INDEX; i++) {
        auto& shapeKeys = sortedShapeKeys[i];
        if (shapeKeys.size() > 0) {
            const auto& shapePipeline = shapePlumber->pickPipeline(args, keys[i]);
            args->_shapePipeline = shapePipeline;
            for (const auto& key : shapeKeys) {
                renderShapes(renderContext, shapePlumber, inShapes.at(key));
            }
        }
    }

    // Render withOwnPipeline things
    for (size_t i = OWN_PIPELINE_INDEX; i < keys.size(); i++) {
        auto& shapeKeys = sortedShapeKeys[i];
        if (shapeKeys.size() > 0) {
            args->_shapePipeline = nullptr;
            for (const auto& key : shapeKeys) {
                args->_itemShapeKey = key._flags.to_ulong();
                renderShapes(renderContext, shapePlumber, inShapes.at(key));
            }
        }
    }

    args->_shapePipeline = nullptr;
}

RenderShadowSetup::RenderShadowSetup() :
    _cameraFrustum{ std::make_shared<ViewFrustum>() },
    _coarseShadowFrustum{ std::make_shared<ViewFrustum>() } {
    _shadowFrameCache = std::make_shared<LightStage::ShadowFrame>();
}

void RenderShadowSetup::configure(const Config& config) {
    constantBias0 = config.constantBias0;
    constantBias1 = config.constantBias1;
    constantBias2 = config.constantBias2;
    constantBias3 = config.constantBias3;
    slopeBias0 = config.slopeBias0;
    slopeBias1 = config.slopeBias1;
    slopeBias2 = config.slopeBias2;
    slopeBias3 = config.slopeBias3;
    biasInput = config.biasInput;
    maxDistance = config.maxDistance;
    _stableCascades = config.stableCascades;
    _cacheCascades = config.cacheCascades;
    _farCascadeInterval = std::max(1, config.farCascadeInterval);
}

void RenderShadowSetup::calculateBiases(float biasInput) {
    // The bias in METRES along the light, from the map's texel (2026-11-07): a texel of fixed bias and 2.1 texels per
    // unit of tan(angle) of slope bias (the 3 x 3 tent's farthest tap is 1.5 x sqrt 2 texels from the pixel), times
    // the keylight's shadowBias / 0.5 (0.5 = this default, the builder's knob). LightStage divides them by each
    // cascade map's depth range (setKeylightCascadeBias, setCascadeFrustum). Until 2026-11-07 the bias was a fraction
    // of that range sized by the cascade's VIEW depth, and the range reaches along the light as far as the view does
    // (computeFarDistance), so at a low sun it ate metres of shadow from the contact up, more when the camera looked
    // along the sun (Caity: "the shadow disappears partially"; entry 31).
    const float FIXED_BIAS_TEXELS = 1.0f;
    const float SLOPE_BIAS_TEXELS = 2.1f;
    const float DEFAULT_SHADOW_BIAS = 0.5f;
    const float scale = biasInput / DEFAULT_SHADOW_BIAS;
    for (int i = 0; i < SHADOW_CASCADE_MAX_COUNT; i++) {
        auto& cascade = _globalShadowObject->getCascade(i);
        float cascadeWidth = cascade.getFrustum()->getWidth();
        float cascadeHeight = cascade.getFrustum()->getHeight();
        float texel = glm::max(cascadeWidth, cascadeHeight) / LightStage::Shadow::MAP_SIZE; // TODO: variable cascade resolution
        setConstantBias(i, FIXED_BIAS_TEXELS * texel * scale);
        setSlopeBias(i, SLOPE_BIAS_TEXELS * texel * scale);
    }
}

void RenderShadowSetup::setConstantBias(int cascadeIndex, float value) {
    _bias[cascadeIndex]._constant = value;
}

void RenderShadowSetup::setSlopeBias(int cascadeIndex, float value) {
    _bias[cascadeIndex]._slope = value;
}

// FROZEN (vehicle path): see the note at the top of this file.
void RenderShadowSetup::run(const render::RenderContextPointer& renderContext, const Input& input, Output& output) {
    // Abort all jobs if not casting shadows
    auto lightStage = renderContext->_scene->getStage<LightStage>();
    assert(lightStage);

    const auto lightFrame = *input.get0();
    const auto lightingModel = input.get1();

    // Clear previous shadow frame always
    _shadowFrameCache->_objects.clear();
    output.edit3() = _shadowFrameCache;

    const auto currentKeyLight = lightStage->getCurrentKeyLight(lightFrame);
    // CPM
    if (!lightingModel->isShadowEnabled() || !currentKeyLight || !currentKeyLight->getCastShadows()) {
        renderContext->taskFlow.abortTask();
        return;
    }
    output.edit4() = currentKeyLight;

    // Cache old render args
    RenderArgs* args = renderContext->args;

    output.edit0() = args->_renderMode;
    // Save main camera frustum
    *_cameraFrustum = args->getViewFrustum();
    output.edit2() = _cameraFrustum;
    // the camera the casters choose their mesh's level of detail from while the light's view is pushed
    // (MeshPartPayload::chooseLodLevel), as seen in this view
    args->_shadowLodEye = _cameraFrustum->getPosition();
    args->_shadowLodPixelsPerRadian = 0.5f * (float)args->_viewport.w * _cameraFrustum->getProjection()[1][1];

    if (!_globalShadowObject) {
        _globalShadowObject = std::make_shared<LightStage::Shadow>(currentKeyLight, SHADOW_CASCADE_MAX_COUNT);
    }
    _globalShadowObject->setLight(currentKeyLight);
    _globalShadowObject->setStableCascades(_stableCascades);
    _globalShadowObject->setCacheCascades(_cacheCascades);

    // The far cascades take turns: each is fitted to the view and drawn again only on its frame, one every
    // _farCascadeInterval frames, and held as it was in between (its map, and the matrix the shaders read it by).
    // Their casters are most of the sun's draws, and far from the camera a frame's lag in their shadows doesn't show
    ++_frameCount;
    const unsigned int FIRST_FAR_CASCADE = 2;
    int heldCascades = 0;
    for (unsigned int cascadeIndex = 0; cascadeIndex < _globalShadowObject->getCascadeCount(); ++cascadeIndex) {
        const auto& cascade = _globalShadowObject->getCascade(cascadeIndex);
        bool hold = _farCascadeInterval > 1 && cascadeIndex >= FIRST_FAR_CASCADE && cascade.isFitted() &&
            _frameCount % _farCascadeInterval != (cascadeIndex - FIRST_FAR_CASCADE) % _farCascadeInterval;
        _globalShadowObject->holdCascade(cascadeIndex, hold);
        heldCascades += hold ? 1 : 0;
    }
    std::static_pointer_cast<Config>(renderContext->jobConfig)->setCascadeCounts(_globalShadowObject->takeCascadesDrawn(), heldCascades);

    _globalShadowObject->setKeylightFrustum(args->getViewFrustum(), SHADOW_FRUSTUM_NEAR, SHADOW_FRUSTUM_FAR);

    // Update our biases and maxDistance from the light or config
    _globalShadowObject->setMaxDistance(maxDistance > 0.0f ? maxDistance : currentKeyLight->getShadowsMaxDistance());

    // Adjust each cascade frustum
    for (unsigned int cascadeIndex = 0; cascadeIndex < _globalShadowObject->getCascadeCount(); ++cascadeIndex) {
        if (_globalShadowObject->getCascade(cascadeIndex).isHeld()) {
            continue;
        }
        _globalShadowObject->setKeylightCascadeFrustum(cascadeIndex, args->getViewFrustum(), SHADOW_FRUSTUM_NEAR, SHADOW_FRUSTUM_FAR);
    }

    calculateBiases(biasInput > 0.0f ? biasInput : currentKeyLight->getShadowBias());

    std::array<float, SHADOW_CASCADE_MAX_COUNT> constantBiases = {{ constantBias0, constantBias1, constantBias2, constantBias3 }};
    std::array<float, SHADOW_CASCADE_MAX_COUNT> slopeBiases = {{ slopeBias0, slopeBias1, slopeBias2, slopeBias3 }};
    for (unsigned int cascadeIndex = 0; cascadeIndex < _globalShadowObject->getCascadeCount(); ++cascadeIndex) {
        float constantBias = constantBiases[cascadeIndex];
        if (constantBias > 0.0f) {
            setConstantBias(cascadeIndex, constantBias);
        }
        float slopeBias = slopeBiases[cascadeIndex];
        if (slopeBias > 0.0f) {
            setSlopeBias(cascadeIndex, slopeBias);
        }

        auto& bias = _bias[cascadeIndex];
        _globalShadowObject->setKeylightCascadeBias(cascadeIndex, bias._constant, bias._slope);
    }

    _shadowFrameCache->pushShadow(_globalShadowObject);

    // Now adjust coarse frustum bounds
    auto& firstCascade = _globalShadowObject->getCascade(0);
    auto& firstCascadeFrustum = firstCascade.getFrustum();
    auto frustumPosition = firstCascadeFrustum->getPosition();
    auto farTopLeft = firstCascadeFrustum->getFarTopLeft() - frustumPosition;
    auto farBottomRight = firstCascadeFrustum->getFarBottomRight() - frustumPosition;

    auto left = glm::dot(farTopLeft, firstCascadeFrustum->getRight());
    auto right = glm::dot(farBottomRight, firstCascadeFrustum->getRight());
    auto top = glm::dot(farTopLeft, firstCascadeFrustum->getUp());
    auto bottom = glm::dot(farBottomRight, firstCascadeFrustum->getUp());
    auto near = firstCascadeFrustum->getNearClip();
    auto far = firstCascadeFrustum->getFarClip();

    for (unsigned int cascadeIndex = 1; cascadeIndex < _globalShadowObject->getCascadeCount(); ++cascadeIndex) {
        auto& cascadeFrustum = _globalShadowObject->getCascade(cascadeIndex).getFrustum();

        farTopLeft = cascadeFrustum->getFarTopLeft() - frustumPosition;
        farBottomRight = cascadeFrustum->getFarBottomRight() - frustumPosition;

        auto cascadeLeft = glm::dot(farTopLeft, cascadeFrustum->getRight());
        auto cascadeRight = glm::dot(farBottomRight, cascadeFrustum->getRight());
        auto cascadeTop = glm::dot(farTopLeft, cascadeFrustum->getUp());
        auto cascadeBottom = glm::dot(farBottomRight, cascadeFrustum->getUp());
        auto cascadeNear = cascadeFrustum->getNearClip();
        auto cascadeFar = cascadeFrustum->getFarClip();
        left = glm::min(left, cascadeLeft);
        right = glm::max(right, cascadeRight);
        bottom = glm::min(bottom, cascadeBottom);
        top = glm::max(top, cascadeTop);
        near = glm::min(near, cascadeNear);
        far = glm::max(far, cascadeFar);
    }

    _coarseShadowFrustum->setPosition(firstCascadeFrustum->getPosition());
    _coarseShadowFrustum->setOrientation(firstCascadeFrustum->getOrientation());
    _coarseShadowFrustum->setProjection(glm::ortho<float>(left, right, bottom, top, near, far));
    _coarseShadowFrustum->calculate();

    // Push frustum for further culling and selection
    args->pushViewFrustum(*_coarseShadowFrustum);

    args->_renderMode = RenderArgs::SHADOW_RENDER_MODE;

    // We want for the octree query enough resolution to catch the details in the lowest cascade. So compute
    // the desired resolution for the first cascade frustum and extrapolate it to the coarse frustum.
    glm::ivec2 queryResolution = firstCascade.framebuffer->getSize();
    queryResolution.x = int(queryResolution.x * _coarseShadowFrustum->getWidth() / firstCascadeFrustum->getWidth());
    queryResolution.y = int(queryResolution.y * _coarseShadowFrustum->getHeight() / firstCascadeFrustum->getHeight());
    output.edit1() = queryResolution;
}

const std::array<float, SHADOW_CASCADE_MAX_COUNT> RenderShadowCascadeSetup::DEFAULT_MIN_CASTER_TEXELS {{ 24.0f, 24.0f, 48.0f, 48.0f }};

// FROZEN (vehicle path): see the note at the top of this file.
void RenderShadowCascadeSetup::run(const render::RenderContextPointer& renderContext, const Inputs& input, Outputs& output) {
    const auto shadowFrame = input;

    // Cache old render args
    RenderArgs* args = renderContext->args;

    RenderShadowTask::CullFunctor cullFunctor;
    if (shadowFrame && !shadowFrame->_objects.empty() && shadowFrame->_objects[0]) {
        const auto globalShadow = shadowFrame->_objects[0];

        if (globalShadow && _cascadeIndex < globalShadow->getCascadeCount()) {
            auto& cascade = globalShadow->getCascade(_cascadeIndex);
            // a far cascade waiting for its turn is culled too, by the frustum it holds, so that RenderShadowMap
            // can tell whether one of its casters moved; its map and matrix stay unless one did
            auto& cascadeFrustum = cascade.getFrustum();
            output.edit0() = _filter;

            // Set the keylight render args
            args->pushViewFrustum(*cascadeFrustum);
            auto texelSize = glm::min(cascadeFrustum->getHeight(), cascadeFrustum->getWidth()) / cascade.framebuffer->getSize().x;
            // a caster smaller than this many of the cascade's texels doesn't draw into it
            texelSize *= _minCasterTexels > 0.0f ? _minCasterTexels : DEFAULT_MIN_CASTER_TEXELS[_cascadeIndex];
            cullFunctor._minSquareSize = texelSize * texelSize;

            output.edit1() = cascadeFrustum;

        } else {
            output.edit0() = ItemFilter::Builder::nothing();
            output.edit1() = ViewFrustumPointer();
        }
    }
    else {
        output.edit0() = ItemFilter::Builder::nothing();
        output.edit1() = ViewFrustumPointer();
    }

    output.edit2() = cullFunctor;
}

void RenderShadowCascadeTeardown::run(const render::RenderContextPointer& renderContext, const Input& input) {
    RenderArgs* args = renderContext->args;

    if (args->_renderMode == RenderArgs::SHADOW_RENDER_MODE && !input.selectsNothing()) {
        args->popViewFrustum();
    }
    assert(args->hasViewFrustum());
}

void RenderShadowTeardown::run(const render::RenderContextPointer& renderContext, const Input& input) {
    RenderArgs* args = renderContext->args;

    if (args->_renderMode == RenderArgs::SHADOW_RENDER_MODE) {
        args->popViewFrustum();
    }
    assert(args->hasViewFrustum());
    // Reset the render args
    args->_renderMode = input.get0();
}

static AABox& merge(AABox& box, const AABox& otherBox, const glm::vec3& dir) {
    if (!otherBox.isInvalid()) {
        int vertexIndex = 0;
        vertexIndex |= ((dir.z > 0.0f) & 1) << 2;
        vertexIndex |= ((dir.y > 0.0f) & 1) << 1;
        vertexIndex |= ((dir.x < 0.0f) & 1);
        auto vertex = otherBox.getVertex((BoxVertex)vertexIndex);
        if (!box.isInvalid()) {
            const auto boxCenter = box.calcCenter();
            vertex -= boxCenter;
            vertex = dir * glm::max(0.0f, glm::dot(vertex, dir));
            vertex += boxCenter;
        }
        box += vertex;
    }
    return box;
}

void CullShadowBounds::run(const render::RenderContextPointer& renderContext, const Inputs& inputs, Outputs& outputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());
    RenderArgs* args = renderContext->args;

    const auto& inShapes = inputs.get0();
    const auto& filter = inputs.get1();
    ViewFrustumPointer antiFrustum;
    auto& outShapes = outputs.edit0();
    auto& outBounds = outputs.edit1();

    if (!inputs[3].isNull()) {
        antiFrustum = inputs.get2();
    }
    outShapes.clear();
    outBounds = AABox();

    const auto currentKeyLight = inputs.get3();
    auto cullFunctor = inputs.get4();

    render::CullFunctor shadowCullFunctor = [cullFunctor](const RenderArgs* args, const AABox& bounds) {
        return cullFunctor(args, bounds);
    };

    if (!filter.selectsNothing() && currentKeyLight) {
        // Stats' shadow line (considered / out of view / too small / rendered) counts here, at the cull, so "rendered"
        // includes the casters of a far cascade waiting for its turn, whose map is drawn only when one of them changed
        auto& details = args->_details.edit(RenderDetails::SHADOW);
        render::CullTest test(shadowCullFunctor, args, details, antiFrustum);
        auto scene = args->_scene;
        auto lightStage = renderContext->_scene->getStage<LightStage>();
        assert(lightStage);
        const auto globalLightDir = currentKeyLight->getDirection();
        auto castersFilter = render::ItemFilter::Builder(filter).withShadowCaster().build();

        for (auto& inItems : inShapes) {
            auto key = inItems.first;
            // A translucent item casts when it has a material (a model's mesh, a shape): solid, or cut out by the
            // material's opacity mask, which the depth-only shaders read. Translucent text, pictures, grids,
            // polylines and particles have none and only receive; opaque ones draw into the map as before.
            const bool receivesOnly = key.isTranslucent() && !key.useMaterial();
            auto outItems = outShapes.find(key);
            if (outItems == outShapes.end()) {
                outItems = outShapes.insert(std::make_pair(key, ItemBounds{})).first;
                outItems->second.reserve(inItems.second.size());
            }

            details._considered += (int)inItems.second.size();

            if (antiFrustum == nullptr) {
                for (auto& item : inItems.second) {
                    if (test.solidAngleTest(item.bound) && test.frustumTest(item.bound)) {
                        const auto shapeKey = scene->getItem(item.id).getKey();
                        if (!receivesOnly && castersFilter.test(shapeKey)) {
                            outItems->second.emplace_back(item);
                            outBounds += item.bound;
                        } else {
                            // Receivers are not rendered but they still increase the bounds of the shadow scene
                            // although only in the direction of the light direction so as to have a correct far
                            // distance without decreasing the near distance.
                            merge(outBounds, item.bound, globalLightDir);
                        }
                    }
                }
            } else {
                for (auto& item : inItems.second) {
                    if (test.solidAngleTest(item.bound) && test.frustumTest(item.bound) && test.antiFrustumTest(item.bound)) {
                        const auto shapeKey = scene->getItem(item.id).getKey();
                        if (!receivesOnly && castersFilter.test(shapeKey)) {
                            outItems->second.emplace_back(item);
                            outBounds += item.bound;
                        } else {
                            // Receivers are not rendered but they still increase the bounds of the shadow scene
                            // although only in the direction of the light direction so as to have a correct far
                            // distance without decreasing the near distance.
                            merge(outBounds, item.bound, globalLightDir);
                        }
                    }
                }
            }
            details._rendered += (int)outItems->second.size();
        }

        for (auto& items : outShapes) {
            items.second.shrink_to_fit();
        }
    }
}
