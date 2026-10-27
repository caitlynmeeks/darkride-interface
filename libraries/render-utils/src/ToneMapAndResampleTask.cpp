//
//  ToneMapAndResampleTask.cpp
//  libraries/render-utils/src
//
//  Created by Anna Brewer on 7/3/19.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ToneMapAndResampleTask.h"

#include <cmath>

#include <gpu/Context.h>
#include <shaders/Shaders.h>

#include "render-utils/ShaderConstants.h"
#include "StencilMaskPass.h"
#include "FramebufferCache.h"

using namespace render;
using namespace shader::gpu::program;
using namespace shader::render_utils::program;

gpu::PipelinePointer ToneMapAndResample::_pipeline;
gpu::PipelinePointer ToneMapAndResample::_mirrorPipeline;
gpu::PipelinePointer ToneMapAndResample::_fsrToneMapPipeline;
gpu::PipelinePointer ToneMapAndResample::_fsrEasuPipeline;
gpu::PipelinePointer ToneMapAndResample::_fsrRcasPipeline;
std::atomic<bool> ToneMapAndResample::_showNaNPixels { false };

ToneMapAndResample::ToneMapAndResample() {
    Parameters parameters;
    _parametersBuffer = gpu::BufferView(std::make_shared<gpu::Buffer>(sizeof(Parameters), (const gpu::Byte*) &parameters));
    FsrParameters fsrParameters;
    _fsrParametersBuffer = gpu::BufferView(std::make_shared<gpu::Buffer>(sizeof(FsrParameters), (const gpu::Byte*) &fsrParameters));
}

void ToneMapAndResample::init() {
    // shared_ptr to gpu::State
    gpu::StatePointer blitState = gpu::StatePointer(new gpu::State());

    blitState->setDepthTest(gpu::State::DepthTest(false, false));
    blitState->setColorWriteMask(true, true, true, true);

    _pipeline = gpu::PipelinePointer(gpu::Pipeline::create(gpu::Shader::createProgram(toneMapping), blitState));
    _mirrorPipeline = gpu::PipelinePointer(gpu::Pipeline::create(gpu::Shader::createProgram(toneMapping_mirrored), blitState));
    _fsrToneMapPipeline = gpu::PipelinePointer(gpu::Pipeline::create(gpu::Shader::createProgram(toneMapping_fsr), blitState));
    _fsrEasuPipeline = gpu::PipelinePointer(gpu::Pipeline::create(gpu::Shader::createProgram(fsrEasu), blitState));
    _fsrRcasPipeline = gpu::PipelinePointer(gpu::Pipeline::create(gpu::Shader::createProgram(fsrRcas), blitState));
}

void ToneMapAndResample::setExposure(float exposure) {
    auto& params = _parametersBuffer.get<Parameters>();
    if (params._exposure != exposure) {
        _parametersBuffer.edit<Parameters>()._exposure = exposure;
        _parametersBuffer.edit<Parameters>()._twoPowExposure = pow(2.0, exposure);
    }
}

void ToneMapAndResample::setToneCurve(ToneCurve curve) {
    auto& params = _parametersBuffer.get<Parameters>();
    if (params._toneCurve != (int)curve) {
        _parametersBuffer.edit<Parameters>()._toneCurve = (int)curve;
    }
}

void ToneMapAndResample::configure(const Config& config) {
    setExposure(config.exposure);
    _upscaleFSR = config.upscaleFSR;
    _sharpness = config.sharpness;

    // force secondary camera tonemapping to Gamma22 because
    // scripts can change it and they cant access the ToneCurve enum
    // `Render.getConfig("SecondaryCameraJob.ToneMapping").curve = 0`
    // TODO: this needs a better fix
    if (
        config.parent() != nullptr && 
        config.parent()->parent() != nullptr && 
        config.parent()->parent()->parent() != nullptr &&
        config.parent()->parent()->parent()->parent() != nullptr &&
        config.parent()->parent()->parent()->parent()->objectName() == "RenderSecondView"
    ) {
        setToneCurve(ToneCurve::Gamma22);
    } else {
        setToneCurve((ToneCurve)config.curve);
    }
}

void ToneMapAndResample::run(const RenderContextPointer& renderContext, const Input& input, Output& output) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    RenderArgs* args = renderContext->args;

    auto lightingBuffer = input.get0()->getRenderBuffer(0);
    auto destinationFramebuffer = input.get1();

    if (!destinationFramebuffer) {
        destinationFramebuffer = args->_blitFramebuffer;
    }

    if (!lightingBuffer || !destinationFramebuffer) {
        return;
    }

    if (!_pipeline) {
        init();
    }

    const int showNaNPixels = _showNaNPixels ? 1 : 0;
    if (_parametersBuffer.get<Parameters>()._showNaNPixels != showNaNPixels) {
        _parametersBuffer.edit<Parameters>()._showNaNPixels = showNaNPixels;
    }

    const auto bufferSize = destinationFramebuffer->getSize();

    auto srcBufferSize = glm::ivec2(lightingBuffer->getDimensions());

    glm::ivec4 destViewport{ 0, 0, bufferSize.x, bufferSize.y };

    // the main view drawn below the window's resolution (Render.viewportResolutionScale < 1): upscaled with FSR 1
    // rather than stretched. Mirrors and secondary cameras keep the stretch, and so does VR (both eyes side by side in
    // one picture) until it's been tried with a headset.
    if (_upscaleFSR && args->_renderMode == RenderArgs::RenderMode::DEFAULT_RENDER_MODE && !args->isStereo() &&
        args->_viewport.z > 0 && args->_viewport.w > 0 &&
        args->_viewport.z < destViewport.z && args->_viewport.w < destViewport.w) {
        upscaleFSR(args, lightingBuffer, srcBufferSize, destinationFramebuffer, destViewport);
        args->_viewport = destViewport;
        output = destinationFramebuffer;
        return;
    }

    gpu::doInBatch("Resample::run", args->_context, [&](gpu::Batch& batch) {
        batch.enableStereo(false);
        batch.setFramebuffer(destinationFramebuffer);

        batch.setViewportTransform(destViewport);
        batch.setProjectionTransform(glm::mat4());
        batch.resetViewTransform();
        batch.setPipeline(args->_renderMode == RenderArgs::MIRROR_RENDER_MODE ? _mirrorPipeline : _pipeline);

        batch.setModelTransform(gpu::Framebuffer::evalSubregionTexcoordTransform(srcBufferSize, args->_viewport));
        batch.setUniformBuffer(render_utils::slot::buffer::ToneMappingParams, _parametersBuffer);
        batch.setResourceTexture(render_utils::slot::texture::ToneMappingColor, lightingBuffer);
        batch.draw(gpu::TRIANGLE_STRIP, 4);
    });

    // Set full final viewport
    args->_viewport = destViewport;

    output = destinationFramebuffer;
}

void ToneMapAndResample::upscaleFSR(RenderArgs* args, const gpu::TexturePointer& lightingBuffer, const glm::ivec2& srcBufferSize,
                                    const gpu::FramebufferPointer& destinationFramebuffer, const glm::ivec4& destViewport) {
    const glm::ivec2 srcSize(args->_viewport.z, args->_viewport.w);
    const glm::ivec2 destSize(destViewport.z, destViewport.w);

    // plain 8-bit buffers (not sRGB: they hold gamma 2.0 already), clamped at the edges for EASU's gathers
    auto sizeFramebuffer = [](gpu::FramebufferPointer& framebuffer, const std::string& name, const glm::ivec2& size) {
        if (!framebuffer || framebuffer->getSize() != glm::uvec2(size)) {
            framebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create(name));
            auto sampler = gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_LINEAR, gpu::Sampler::WRAP_CLAMP);
            framebuffer->setRenderBuffer(0, gpu::Texture::createRenderBuffer(gpu::Element::COLOR_RGBA_32, size.x, size.y,
                                                                             gpu::Texture::SINGLE_MIP, sampler));
        }
    };
    sizeFramebuffer(_fsrSourceFramebuffer, "fsrSource", srcSize);
    sizeFramebuffer(_fsrUpscaledFramebuffer, "fsrUpscaled", destSize);

    // FsrEasuCon(), the input being the whole of the source buffer; FsrRcasCon()'s sharpness from stops
    const glm::vec2 in(srcSize);
    const glm::vec2 out(destSize);
    FsrParameters parameters;
    parameters.con0 = glm::vec4(in / out, 0.5f * in / out - 0.5f);
    parameters.con1 = glm::vec4(1.0f / in.x, 1.0f / in.y, 1.0f / in.x, -1.0f / in.y);
    parameters.con2 = glm::vec4(-1.0f / in.x, 2.0f / in.y, 1.0f / in.x, 2.0f / in.y);
    parameters.con3 = glm::vec4(0.0f, 4.0f / in.y, 0.0f, 0.0f);
    parameters.rcas = glm::vec4(std::exp2(-_sharpness), 0.0f, 0.0f, 0.0f);
    const auto& current = _fsrParametersBuffer.get<FsrParameters>();
    if (current.con0 != parameters.con0 || current.con1 != parameters.con1 || current.con2 != parameters.con2 ||
        current.con3 != parameters.con3 || current.rcas != parameters.rcas) {
        _fsrParametersBuffer.edit<FsrParameters>() = parameters;
    }

    gpu::doInBatch("Resample::upscaleFSR", args->_context, [&](gpu::Batch& batch) {
        batch.enableStereo(false);
        batch.setProjectionTransform(glm::mat4());
        batch.resetViewTransform();

        // tone mapped at the drawn resolution, into gamma 2.0
        batch.setFramebuffer(_fsrSourceFramebuffer);
        batch.setViewportTransform(glm::ivec4(0, 0, srcSize.x, srcSize.y));
        batch.setPipeline(_fsrToneMapPipeline);
        batch.setModelTransform(gpu::Framebuffer::evalSubregionTexcoordTransform(srcBufferSize, args->_viewport));
        batch.setUniformBuffer(render_utils::slot::buffer::ToneMappingParams, _parametersBuffer);
        batch.setResourceTexture(render_utils::slot::texture::ToneMappingColor, lightingBuffer);
        batch.draw(gpu::TRIANGLE_STRIP, 4);

        // upscaled to the window's resolution (EASU)
        batch.setFramebuffer(_fsrUpscaledFramebuffer);
        batch.setViewportTransform(glm::ivec4(0, 0, destSize.x, destSize.y));
        batch.setPipeline(_fsrEasuPipeline);
        batch.setUniformBuffer(render_utils::slot::buffer::FsrParams, _fsrParametersBuffer);
        batch.setResourceTexture(render_utils::slot::texture::FsrSource, _fsrSourceFramebuffer->getRenderBuffer(0));
        batch.draw(gpu::TRIANGLE_STRIP, 4);

        // sharpened (RCAS), and back to linear for the sRGB destination
        batch.setFramebuffer(destinationFramebuffer);
        batch.setViewportTransform(destViewport);
        batch.setPipeline(_fsrRcasPipeline);
        batch.setResourceTexture(render_utils::slot::texture::FsrSource, _fsrUpscaledFramebuffer->getRenderBuffer(0));
        batch.draw(gpu::TRIANGLE_STRIP, 4);
        batch.setResourceTexture(render_utils::slot::texture::FsrSource, nullptr);
    });
}
