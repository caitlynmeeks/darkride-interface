
//
//  RenderForwardTask.cpp
//  render-utils/src/
//
//  Created by Zach Pomerantz on 12/13/2016.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderForwardTask.h"

#include <PerfStat.h>
#include <PathUtils.h>
#include <ViewFrustum.h>
#include <gpu/Context.h>
#include <gpu/Texture.h>
#include <graphics/ShaderConstants.h>
#include <render/ShapePipeline.h>

#include <render/FilterTask.h>

#include "RenderHifi.h"
#include "render-utils/ShaderConstants.h"
#include "StencilMaskPass.h"
#include "ZoneRenderer.h"
#include "FadeEffect.h"
#include "ToneMapAndResampleTask.h"
#include "BackgroundStage.h"
#include "FramebufferCache.h"
#include "TextureCache.h"
#include "RenderCommonTask.h"
#include "GeometryCache.h"

extern void initZPassPipelines(render::ShapePlumber& plumber, gpu::StatePointer state, const render::ShapePipeline::BatchSetter& batchSetter, const render::ShapePipeline::ItemSetter& itemSetter);
#include "RenderHUDLayerTask.h"
#include "DeferredLightingEffect.h"
#include "HighlightEffect.h"
#include "BloomEffect.h"
#include <gpu/Context.h>
#include <FrameDiag.h>

namespace ru {
    using render_utils::slot::texture::Texture;
    using render_utils::slot::buffer::Buffer;
}

namespace gr {
    using graphics::slot::texture::Texture;
    using graphics::slot::buffer::Buffer;
}

using namespace render;

extern void initForwardPipelines(ShapePlumber& plumber, const render::ShapePipeline::BatchSetter& batchSetter, const render::ShapePipeline::ItemSetter& itemSetter);

void PreparePrimaryFramebufferMSAAConfig::setResolutionScale(float scale) {
    const float SCALE_RANGE_MIN = 0.1f;
    const float SCALE_RANGE_MAX = 5.0f;
    resolutionScale = std::max(SCALE_RANGE_MIN, std::min(SCALE_RANGE_MAX, scale));
}

void PreparePrimaryFramebufferMSAAConfig::setNumSamples(int num) {
    numSamples = std::max(1, std::min(32, num));
    emit dirty();
}

void RenderForwardTask::configure(const Config& config) {
    // Propagate resolution scale to sub jobs who need it
    auto preparePrimaryBufferConfig = config.getConfig<PreparePrimaryFramebufferMSAA>("PreparePrimaryBufferForward");
    assert(preparePrimaryBufferConfig);
    preparePrimaryBufferConfig->setResolutionScale(config.resolutionScale);
}

void RenderForwardTask::build(JobModel& task, const render::Varying& input, render::Varying& output) {
    task.addJob<SetRenderMethod>("SetRenderMethodTask", render::Args::FORWARD);
    task.addJob<WarmPipelines>("WarmPipelines", true);

    // Prepare the ShapePipelines
    auto fadeEffect = DependencyManager::get<FadeEffect>();
    ShapePlumberPointer shapePlumber = std::make_shared<ShapePlumber>();
    initForwardPipelines(*shapePlumber, fadeEffect->getBatchSetter(), fadeEffect->getItemUniformSetter());
    task.addJob<WarmPipelines>("WarmForwardPipelines", shapePlumber);   // entry 29 step 2a: compiled while loading
    // The sun shadow pass's programs too (2026-11-07, after entry 31's ride): its own plumber (RenderShadowTask::build,
    // initZPassPipelines) and the geometry cache's simple pipelines a shape's shadow draws with are outside the walk
    // above, so they compiled at the first shadow drawn in the world, 17 ms in one frame of a ride. A fresh plumber
    // here carries the same program objects (gpu::Shader::createProgram keeps one per program id), so binding its
    // pipelines compiles the shadow task's own; the shadow task's pipelines are never touched. The shadow pass runs
    // before this task sets the frame's render method, so a shape's shadow binds the non-forward simple pipelines.
    ShapePlumberPointer shadowPlumber = std::make_shared<ShapePlumber>();
    initZPassPipelines(*shadowPlumber, std::make_shared<gpu::State>(), fadeEffect->getBatchSetter(), fadeEffect->getItemUniformSetter());
    task.addJob<WarmPipelines>("WarmShadowPipelines", shadowPlumber, std::vector<gpu::PipelinePointer>{
        GeometryCache::getSimplePipeline(false, false, false),   // an opaque shape's shadow
        GeometryCache::getSimplePipeline(false, true, false),    // a translucent one's (they cast too)
        GeometryCache::getSimplePipeline(false, false, true) }); // an unlit one's

    // Unpack inputs
    const auto& inputs = input.get<Input>();
    
    // Separate the fetched items
    const auto& fetchedItems = inputs.get0();

        const auto& items = fetchedItems.get0();

            // Extract opaques / transparents / lights / metas / layered / background
            const auto& opaques = items[RenderFetchCullSortTask::OPAQUE_SHAPE];
            const auto& transparents = items[RenderFetchCullSortTask::TRANSPARENT_SHAPE];
            const auto& metas = items[RenderFetchCullSortTask::META];
            const auto& inFrontOpaque = items[RenderFetchCullSortTask::LAYER_FRONT_OPAQUE_SHAPE];
            const auto& inFrontTransparent = items[RenderFetchCullSortTask::LAYER_FRONT_TRANSPARENT_SHAPE];
            const auto& hudOpaque = items[RenderFetchCullSortTask::LAYER_HUD_OPAQUE_SHAPE];
            const auto& hudTransparent = items[RenderFetchCullSortTask::LAYER_HUD_TRANSPARENT_SHAPE];

    // Lighting model comes next, the big configuration of the view
    const auto& lightingModel = inputs[1];

    // Extract the Lighting Stages Current frame ( and zones)
    const auto& lightingStageInputs = inputs.get2();
        // Fetch the current frame stacks from all the stages
        const auto currentStageFrames = lightingStageInputs.get0();
            const auto lightFrame = currentStageFrames[0];
            const auto backgroundFrame = currentStageFrames[1];
            const auto hazeFrame = currentStageFrames[2];
            const auto bloomFrame = currentStageFrames[3];
 
        const auto& zones = lightingStageInputs[1];

    // Shadow Task Outputs: the frame's shadows, drawn before the deferred/forward choice
    const auto& shadowTaskOutputs = inputs.get3();

        // Shadow Stage Frame: the sun's cascades, and the lights' shadow maps and cookies with them
        const auto shadowFrame = shadowTaskOutputs[1];

    // First job, alter faded
    fadeEffect->build(task, opaques);


    // GPU jobs: Start preparing the main framebuffer
    const auto scaledPrimaryFramebuffer = task.addJob<PreparePrimaryFramebufferMSAA>("PreparePrimaryBufferForward");

    // Prepare deferred, generate the shared Deferred Frame Transform. Only valid with the scaled frame buffer
    const auto deferredFrameTransform = task.addJob<GenerateDeferredFrameTransform>("DeferredFrameTransform");

    // Light Clustering: the grid of the lights in view, as the deferred path builds it (on the CPU, from the lights'
    // bounds against the view: it needs no depth buffer, so the pass takes none)
    const auto lightClusteringPassInputs = LightClusteringPass::Input(deferredFrameTransform, lightingModel, lightFrame, render::Varying(LinearDepthFramebufferPointer())).asVarying();
    const auto lightClusters = task.addJob<LightClusteringPass>("LightClustering", lightClusteringPassInputs);

    // The clusters and the shadows go to the forward pipelines' batch setter, which binds them at every pipeline change
    const auto frameLightingInputs = SetFrameLighting::Inputs(lightClusters, shadowFrame, lightingModel).asVarying();
    task.addJob<SetFrameLighting>("SetFrameLighting", frameLightingInputs);

    // Prepare Forward Framebuffer pass 
    const auto prepareForwardInputs = PrepareForward::Inputs(scaledPrimaryFramebuffer, lightFrame).asVarying();
    task.addJob<PrepareForward>("PrepareForward", prepareForwardInputs);

    // draw a stencil mask in hidden regions of the framebuffer.
    task.addJob<PrepareStencil>("PrepareStencil", scaledPrimaryFramebuffer);

    // Draw opaques forward
    const auto opaqueInputs = DrawForward::Inputs(opaques, lightingModel, hazeFrame).asVarying();
    task.addJob<DrawForward>("DrawOpaques", opaqueInputs, shapePlumber, true);

    // Similar to light stage, background stage has been filled by several potential render items and resolved for the frame in this job
    const auto backgroundInputs = DrawBackgroundStage::Inputs(lightingModel, backgroundFrame, hazeFrame).asVarying();
    task.addJob<DrawBackgroundStage>("DrawBackgroundForward", backgroundInputs);

    // Draw transparent objects forward
    const auto transparentInputs = DrawForward::Inputs(transparents, lightingModel, hazeFrame).asVarying();
    task.addJob<DrawForward>("DrawTransparents", transparentInputs, shapePlumber, false);

    // the scene's depth, resolved to one sample now, before the in-front layers clear the primary's depth for
    // themselves: the selection outline tests its occlusion against it, as the deferred path's outline tests against
    // its scene depth before those layers are drawn
    const auto resolvedDepth = task.addJob<ResolveDepth>("ResolveDepth", scaledPrimaryFramebuffer);

     // Layered
    const auto nullJitter = Varying(glm::vec2(0.0f, 0.0f));
    const auto inFrontOpaquesInputs = DrawLayered3D::Inputs(inFrontOpaque, lightingModel, hazeFrame, nullJitter).asVarying();
    const auto inFrontTransparentsInputs = DrawLayered3D::Inputs(inFrontTransparent, lightingModel, hazeFrame, nullJitter).asVarying();
    task.addJob<DrawLayered3D>("DrawInFrontOpaque", inFrontOpaquesInputs, true);
    task.addJob<DrawLayered3D>("DrawInFrontTransparent", inFrontTransparentsInputs, false);

    {  // Debug the bounds of the rendered items, still look at the zbuffer

        task.addJob<DrawBounds>("DrawMetaBounds", metas);
        task.addJob<DrawBounds>("DrawBounds", opaques);
        task.addJob<DrawBounds>("DrawTransparentBounds", transparents);

        task.addJob<DrawBounds>("DrawZones", zones);
        const auto debugZoneInputs = DebugZoneLighting::Inputs(deferredFrameTransform, lightFrame, backgroundFrame).asVarying();
        task.addJob<DebugZoneLighting>("DrawZoneStack", debugZoneInputs);
    }

    const auto newResolvedFramebuffer = task.addJob<NewFramebuffer>("MakeResolvingFramebuffer", gpu::Element(gpu::SCALAR, gpu::FLOAT, gpu::R11G11B10));

    const auto resolveInputs = ResolveFramebuffer::Inputs(scaledPrimaryFramebuffer, newResolvedFramebuffer).asVarying();
    const auto resolvedColorFramebuffer = task.addJob<ResolveFramebuffer>("Resolve", resolveInputs);

    // with the scene's depth attached, for the selection outline, which is drawn over the resolved colour as the
    // deferred path draws it over its lighting (the in-front layers are already in, multisampled, so an outline runs
    // over them where they cross it; deferred draws them after)
    const auto attachInputs = AttachResolvedDepth::Inputs(resolvedColorFramebuffer, resolvedDepth).asVarying();
    const auto resolvedFramebuffer = task.addJob<AttachResolvedDepth>("AttachResolvedDepth", attachInputs);

    const auto outlineInputs = DrawHighlightTask::Inputs(items, resolvedFramebuffer, deferredFrameTransform, nullJitter).asVarying();
    task.addJob<DrawHighlightTask>("DrawHighlightForward", outlineInputs);

    // Bloom over the resolved colour, as the deferred path has it over its lighting before tone mapping (the zone's
    // bloom settings and the lighting model's switch apply to both)
    const auto bloomInputs = BloomEffect::Inputs(deferredFrameTransform, resolvedFramebuffer, bloomFrame, lightingModel).asVarying();
    task.addJob<BloomEffect>("BloomForward", bloomInputs);

    const auto destFramebuffer = static_cast<gpu::FramebufferPointer>(nullptr);

    const auto toneMappingInputs = ToneMapAndResample::Input(resolvedFramebuffer, destFramebuffer).asVarying();
    const auto toneMappedBuffer = task.addJob<ToneMapAndResample>("ToneMapping", toneMappingInputs);
    // HUD Layer
    const auto renderHUDLayerInputs = RenderHUDLayerTask::Input(toneMappedBuffer, lightingModel, hudOpaque, hudTransparent, hazeFrame).asVarying();
    task.addJob<RenderHUDLayerTask>("RenderHUDLayer", renderHUDLayerInputs);
}

void ResolveDepth::run(const render::RenderContextPointer& renderContext, const Inputs& inputs, Outputs& outputs) {
    RenderArgs* args = renderContext->args;
    const auto primary = inputs;
    outputs = nullptr;
    if (!primary || !primary->getDepthStencilBuffer()) {
        return;
    }
    const auto depthFormat = primary->getDepthStencilBufferFormat();
    const auto frameSize = primary->getSize();
    // the resolved depth is of the primary's format, since a depth blit copies only between equal formats
    if (!_depth || glm::uvec2(_depth->getDimensions()) != frameSize || _depth->getTexelFormat() != depthFormat) {
        _depth = gpu::TexturePointer(gpu::Texture::createRenderBuffer(depthFormat, frameSize.x, frameSize.y));
        _depth->setSource("forward resolved depth");
        _target = gpu::FramebufferPointer(gpu::Framebuffer::create("forward resolved depth"));
        _target->setDepthStencilBuffer(_depth, depthFormat);
    }
    if (!_source || _source->getDepthStencilBuffer() != primary->getDepthStencilBuffer()) {
        _source = gpu::FramebufferPointer(gpu::Framebuffer::create("forward depth"));
        _source->setDepthStencilBuffer(primary->getDepthStencilBuffer(), depthFormat);
    }
    gpu::Vec4i rect(0, 0, frameSize.x, frameSize.y);
    gpu::doInBatch("ResolveDepth", args->_context, [&](gpu::Batch& batch) {
        batch.blit(_source, rect, _target, rect);
    });
    outputs = _depth;
}

void AttachResolvedDepth::run(const render::RenderContextPointer& renderContext, const Inputs& inputs, Outputs& outputs) {
    const auto resolved = inputs.get0();
    const auto depth = inputs.get1();
    outputs = resolved;
    if (!resolved || !depth || glm::uvec2(depth->getDimensions()) != resolved->getSize()) {
        return;
    }
    // the resolved framebuffer is remade on a size change, without depth
    if (resolved->getDepthStencilBuffer() != depth) {
        resolved->setDepthStencilBuffer(depth, depth->getTexelFormat());
    }
}

gpu::FramebufferPointer PreparePrimaryFramebufferMSAA::createFramebuffer(const char* name, const glm::uvec2& frameSize, int numSamples) {
    gpu::FramebufferPointer framebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create(name));

    auto defaultSampler = gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_LINEAR);

    auto colorFormat = gpu::Element(gpu::SCALAR, gpu::FLOAT, gpu::R11G11B10);
    auto colorTexture =
        gpu::Texture::createRenderBufferMultisample(colorFormat, frameSize.x, frameSize.y, numSamples, defaultSampler);
    framebuffer->setRenderBuffer(0, colorTexture);

    auto depthFormat = gpu::Element::DEPTH32F_STENCIL8;  // reversed-Z float depth, see gpu::Backend::reversedZProjection
    auto depthTexture =
        gpu::Texture::createRenderBufferMultisample(depthFormat, frameSize.x, frameSize.y, numSamples, defaultSampler);
    framebuffer->setDepthStencilBuffer(depthTexture, depthFormat);

    return framebuffer;
}

void PreparePrimaryFramebufferMSAA::configure(const Config& config) {
    _resolutionScale = config.getResolutionScale();
    _numSamples = config.getNumSamples();
}

void PreparePrimaryFramebufferMSAA::run(const RenderContextPointer& renderContext, gpu::FramebufferPointer& framebuffer) {
    glm::uvec2 frameSize(renderContext->args->_viewport.z, renderContext->args->_viewport.w);
    glm::uvec2 scaledFrameSize(glm::vec2(frameSize) * _resolutionScale);

    // no more samples than the GPU has (a multisampled texture beyond GL_MAX_SAMPLES gets no storage, and the
    // framebuffer is incomplete: nothing draws, and the frame before stays on screen)
    int numSamples = std::max(1, _numSamples);
    const int maxSamples = gpu::Context::getMaxSamples();
    if (maxSamples > 0) {
        numSamples = std::min(numSamples, maxSamples);
    }
    // Resizing framebuffers instead of re-building them seems to cause issues with threaded rendering
    if (!_framebuffer || (_framebuffer->getSize() != scaledFrameSize) || (_framebuffer->getNumSamples() != numSamples)) {
        _framebuffer = createFramebuffer("forward", scaledFrameSize, numSamples);
    }

    framebuffer = _framebuffer;

    // Set viewport for the rest of the scaled passes
    renderContext->args->_viewport.z = scaledFrameSize.x;
    renderContext->args->_viewport.w = scaledFrameSize.y;
}

void PrepareForward::run(const RenderContextPointer& renderContext, const Inputs& inputs) {
    RenderArgs* args = renderContext->args;

    auto primaryFramebuffer = inputs.get0();
    auto lightStageFrame = inputs.get1();

    gpu::doInBatch("RenderForward::Draw::run", args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;

        batch.enableStereo(false);
        batch.setViewportTransform(args->_viewport);
        batch.setStateScissorRect(args->_viewport);

        batch.setFramebuffer(primaryFramebuffer);
        batch.clearFramebuffer(gpu::Framebuffer::BUFFER_COLOR0 | gpu::Framebuffer::BUFFER_DEPTH |
            gpu::Framebuffer::BUFFER_STENCIL,
            vec4(vec3(0), 0), 0.0, 0, true); // depth 0 is the far plane (reversed-Z)

        graphics::LightPointer keySunLight;
        auto lightStage = args->_scene->getStage<LightStage>();
        if (lightStage) {
            keySunLight = lightStage->getCurrentKeyLight(*lightStageFrame);
        }

        graphics::LightPointer keyAmbiLight;
        if (lightStage) {
            keyAmbiLight = lightStage->getCurrentAmbientLight(*lightStageFrame);
        }

        if (keySunLight) {
            batch.setUniformBuffer(gr::Buffer::KeyLight, keySunLight->getLightSchemaBuffer());
        }

        if (keyAmbiLight) {
            batch.setUniformBuffer(gr::Buffer::AmbientLight, keyAmbiLight->getAmbientSchemaBuffer());

            if (keyAmbiLight->getAmbientMap()) {
                batch.setResourceTexture(ru::Texture::Skybox, keyAmbiLight->getAmbientMap());
            }
            // the zone's probe volume (LightAmbient.slh), or an empty one
            batch.setResourceTexture(ru::Texture::ProbeVolume, keyAmbiLight->getProbeVolume() ? keyAmbiLight->getProbeVolume() : DeferredLightingEffect::getEmptyProbeVolume());
        }
    });
}

void DrawForward::run(const RenderContextPointer& renderContext, const Inputs& inputs) {
    RenderArgs* args = renderContext->args;

    const auto& inItems = inputs.get0();
    const auto& lightingModel = inputs.get1();
    const auto& hazeFrame = inputs.get2();

    if (_opaquePass && args->_renderMode == RenderArgs::RenderMode::DEFAULT_RENDER_MODE) {
        FrameDiag::mainOpaques() = (int)inItems.size();
    }

    graphics::HazePointer haze;
    const auto& hazeStage = renderContext->args->_scene->getStage<HazeStage>();
    if (hazeStage && hazeFrame->_hazes.size() > 0) {
        haze = hazeStage->getHaze(hazeFrame->_hazes.front());
    }

    gpu::doInBatch("DrawForward::run", args->_context, [&](gpu::Batch& batch) {
        args->_batch = &batch;

        // Setup projection
        glm::mat4 projMat;
        Transform viewMat;
        args->getViewFrustum().evalProjectionMatrix(projMat);
        args->getViewFrustum().evalViewTransform(viewMat);
        batch.setProjectionTransform(projMat);
        batch.setViewTransform(viewMat);
        batch.setModelTransform(Transform());

        // Setup lighting model for all items;
        batch.setUniformBuffer(ru::Buffer::LightModel, lightingModel->getParametersBuffer());
        batch.setResourceTexture(ru::Texture::AmbientFresnel, lightingModel->getAmbientFresnelLUT());

        // the frame's local lights and shadows (bound again by each pipeline's batch setter as the pipelines change)
        DependencyManager::get<DeferredLightingEffect>()->setupFrameLightingBatch(batch, args);

        if (haze) {
            batch.setUniformBuffer(graphics::slot::buffer::Buffer::HazeParams, haze->getHazeParametersBuffer());
        }

        // From the lighting model define a global shapeKey ORED with individiual keys
        ShapeKey::Builder keyBuilder;
        if (lightingModel->isWireframeEnabled()) {
            keyBuilder.withWireframe();
        }
        ShapeKey globalKey = keyBuilder.build();
        args->_globalShapeKey = globalKey._flags.to_ulong();

        // Render items
        if (_opaquePass) {
            renderStateSortShapes(renderContext, _shapePlumber, inItems, -1, globalKey);
        } else {
            renderShapes(renderContext, _shapePlumber, inItems, -1, globalKey);
        }

        args->_batch = nullptr;
        args->_globalShapeKey = 0;
    });
}


