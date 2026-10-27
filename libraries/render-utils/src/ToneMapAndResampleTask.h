//
//  ToneMapAndResample.h
//  libraries/render-utils/src
//
//  Created by Anna Brewer on 7/3/19.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_ToneMapAndResample_h
#define hifi_ToneMapAndResample_h

#include <atomic>

#include <DependencyManager.h>
#include <NumericalConstants.h>

#include <gpu/Resource.h>
#include <gpu/Pipeline.h>
#include <render/Forward.h>
#include <render/DrawTask.h>

enum class ToneCurve {
    // Different tone curve available
    None,
    Gamma22,
    Reinhard,
    Filmic,
};



//Q_DECLARE_METATYPE(ToneCurve);

class ToneMappingConfig : public render::Job::Config {
    Q_OBJECT
    Q_PROPERTY(float exposure MEMBER exposure WRITE setExposure);
    Q_PROPERTY(int curve MEMBER curve WRITE setCurve);
    Q_PROPERTY(bool upscaleFSR MEMBER upscaleFSR WRITE setUpscaleFSR);
    Q_PROPERTY(float sharpness MEMBER sharpness WRITE setSharpness);

public:
    ToneMappingConfig() : render::Job::Config(true) {}

    Q_INVOKABLE void setExposure(float newExposure) { exposure = newExposure; emit dirty(); }
    Q_INVOKABLE void setCurve(int newCurve) { curve = std::max((int)ToneCurve::None, std::min((int)ToneCurve::Filmic, newCurve)); emit dirty(); }
    Q_INVOKABLE void setUpscaleFSR(bool upscale) { upscaleFSR = upscale; emit dirty(); }
    Q_INVOKABLE void setSharpness(float stops) { sharpness = std::max(0.0f, std::min(2.0f, stops)); emit dirty(); }


    float exposure{ 0.0f };
    int curve{ (int)ToneCurve::Gamma22 };
    // a main view drawn below the window's resolution (Render.viewportResolutionScale < 1) is upscaled with FSR 1
    // (fsrEasu, then fsrRcas) rather than stretched; false stretches it as before, for comparing
    bool upscaleFSR{ true };
    // RCAS's sharpening after the upscale, in stops below the most (0 = the most, 2 = a quarter of it); AMD's 0.2
    float sharpness{ 0.2f };

signals:
    void dirty();
};

class ToneMapAndResample {
public:
    ToneMapAndResample();
    virtual ~ToneMapAndResample() {}

    void render(RenderArgs* args, const gpu::TexturePointer& lightingBuffer, gpu::FramebufferPointer& destinationBuffer);

    void setExposure(float exposure);
    float getExposure() const { return _parametersBuffer.get<Parameters>()._exposure; }

    void setToneCurve(ToneCurve curve);
    ToneCurve getToneCurve() const { return (ToneCurve)_parametersBuffer.get<Parameters>()._toneCurve; }

    // a NaN fragment (a shading fault) comes out magenta instead of black, in every view (Developer > Render > Show
    // NaN Pixels)
    static void setShowNaNPixels(bool show) { _showNaNPixels = show; }

    // Inputs: lightingFramebuffer, destinationFramebuffer
    using Input = render::VaryingSet2<gpu::FramebufferPointer, gpu::FramebufferPointer>;
    using Output = gpu::FramebufferPointer;
    using Config = ToneMappingConfig;
    using JobModel = render::Job::ModelIO<ToneMapAndResample, Input, Output, Config>;

    void configure(const Config& config);
    void run(const render::RenderContextPointer& renderContext, const Input& input, Output& output);

protected:
    static gpu::PipelinePointer _pipeline;
    static gpu::PipelinePointer _mirrorPipeline;
    static gpu::PipelinePointer _fsrToneMapPipeline;
    static gpu::PipelinePointer _fsrEasuPipeline;
    static gpu::PipelinePointer _fsrRcasPipeline;
    static std::atomic<bool> _showNaNPixels;

    gpu::FramebufferPointer _destinationFrameBuffer;

    float _factor{ 2.0f };

    gpu::FramebufferPointer getResampledFrameBuffer(const gpu::FramebufferPointer& sourceFramebuffer);

private:
    gpu::PipelinePointer _blitLightBuffer;

    // Class describing the uniform buffer with all the parameters common to the tone mapping shaders
    class Parameters {
    public:
        float _exposure = 0.0f;
        float _twoPowExposure = 1.0f;
        glm::vec2 spareA;
        int _toneCurve = (int)ToneCurve::Gamma22;
        int _showNaNPixels = 0;
        glm::ivec2 spareB;

        Parameters() {}
    };

    typedef gpu::BufferView UniformBufferView;
    gpu::BufferView _parametersBuffer;

    // FSR 1 upscaling: FsrEasuCon()'s four constants, and RCAS's sharpness in rcas.x (fsrEasu.slf, fsrRcas.slf)
    class FsrParameters {
    public:
        glm::vec4 con0;
        glm::vec4 con1;
        glm::vec4 con2;
        glm::vec4 con3;
        glm::vec4 rcas;
    };
    gpu::BufferView _fsrParametersBuffer;
    bool _upscaleFSR { true };
    float _sharpness { 0.2f };
    // the tone-mapped picture at the drawn resolution, and its upscale at the window's, both in gamma 2.0
    gpu::FramebufferPointer _fsrSourceFramebuffer;
    gpu::FramebufferPointer _fsrUpscaledFramebuffer;

    void init();
    void upscaleFSR(RenderArgs* args, const gpu::TexturePointer& lightingBuffer, const glm::ivec2& srcBufferSize,
                    const gpu::FramebufferPointer& destinationFramebuffer, const glm::ivec4& destViewport);
};

#endif // hifi_ToneMapAndResample_h
