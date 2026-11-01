//
//  Created by Bradley Austin Davis on 2018/01/09
//  Copyright 2013-2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderCommonTask_h
#define hifi_RenderCommonTask_h

#include <set>
#include <gpu/Pipeline.h>
#include "LightStage.h"
#include "HazeStage.h"
#include "LightingModel.h"

class BeginGPURangeTimer {
public:
    using JobModel = render::Job::ModelO<BeginGPURangeTimer, gpu::RangeTimerPointer>;

    BeginGPURangeTimer(const std::string& name) : _gpuTimer(std::make_shared<gpu::RangeTimer>(name)) {}

    void run(const render::RenderContextPointer& renderContext, gpu::RangeTimerPointer& timer);

protected:
    gpu::RangeTimerPointer _gpuTimer;
};

using GPURangeTimerConfig = render::GPUJobConfig;

class EndGPURangeTimer {
public:
    using Config = GPURangeTimerConfig;
    using JobModel = render::Job::ModelI<EndGPURangeTimer, gpu::RangeTimerPointer, Config>;

    EndGPURangeTimer() {}

    void configure(const Config& config) {}
    void run(const render::RenderContextPointer& renderContext, const gpu::RangeTimerPointer& timer);

protected:
};

// Entry 29 step 2a: while the loading screen is up, the shape plumber's programs are compiled ahead of their first
// draw, one a frame, on the thread that executes the frame (on a Mac every program compiles from source at its first
// bind, 5-130 ms each, and a ride met them as content came into view). Off when the world is presented (the
// interstitial mode's end turns it off); whatever was not reached compiles at first use as before.
class WarmPipelinesConfig : public render::Job::Config {
    Q_OBJECT
    Q_PROPERTY(int perFrame MEMBER perFrame NOTIFY dirty)
    Q_PROPERTY(int warmed READ getWarmed NOTIFY dirty)
    Q_PROPERTY(int left READ getLeft NOTIFY dirty)
public:
    WarmPipelinesConfig() : render::Job::Config(true) {}   // enableable, on until the world is presented
    int perFrame { 4 };   // distinct programs bound per frame (the plumber's pipelines share programs: ~960 keys, ~90 programs)
    int getWarmed() const { return warmed; }
    int getLeft() const { return left; }
    int warmed { 0 };
    int left { 0 };
signals:
    void dirty();
};

class WarmPipelines {
public:
    using Config = WarmPipelinesConfig;
    using JobModel = render::Job::Model<WarmPipelines, Config>;

    // extras: pipelines outside the plumber, bound after its (the sun shadow pass's shape pipelines, 2026-11-07)
    WarmPipelines(const render::ShapePlumberPointer& shapePlumber, const std::vector<gpu::PipelinePointer>& extras = {}) :
        _shapePlumber(shapePlumber), _extras(extras) {}

    void configure(const Config& config);
    void run(const render::RenderContextPointer& renderContext);

protected:
    render::ShapePlumberPointer _shapePlumber;
    std::vector<gpu::PipelinePointer> _extras;
    int _perFrame { 4 };
    size_t _next { 0 };       // the next pipeline to look at: the plumber's filtered ones, then the extras
    int _warmed { 0 };        // distinct programs bound so far
    bool _done { false };
    bool _wasEnabled { true };
    std::set<const gpu::Shader*> _programsBound;   // a program shared by many keys is bound once
};

class DrawLayered3DConfig : public render::Job::Config {
    Q_OBJECT
        Q_PROPERTY(int numDrawn READ getNumDrawn NOTIFY numDrawnChanged)
        Q_PROPERTY(int maxDrawn MEMBER maxDrawn NOTIFY dirty)
public:
    int getNumDrawn() { return numDrawn; }
    void setNumDrawn(int num) { numDrawn = num; emit numDrawnChanged(); }

    int maxDrawn{ -1 };

signals:
    void numDrawnChanged();
    void dirty();

protected:
    int numDrawn{ 0 };
};

class DrawLayered3D {
public:
    using Inputs = render::VaryingSet4<render::ItemBounds, LightingModelPointer, HazeStage::FramePointer, glm::vec2>;
    using Config = DrawLayered3DConfig;
    using JobModel = render::Job::ModelI<DrawLayered3D, Inputs, Config>;

    DrawLayered3D(bool opaque);

    void configure(const Config& config) { _maxDrawn = config.maxDrawn; }
    void run(const render::RenderContextPointer& renderContext, const Inputs& inputs);

protected:
    render::ShapePlumberPointer _shapePlumber;
    int _maxDrawn; // initialized by Config
    bool _opaquePass { true };
};

class Blit {
public:
    using JobModel = render::Job::ModelI<Blit, gpu::FramebufferPointer>;

    void run(const render::RenderContextPointer& renderContext, const gpu::FramebufferPointer& srcFramebuffer);
};

class NewFramebuffer {
public:
    using Output = gpu::FramebufferPointer;
    using JobModel = render::Job::ModelO<NewFramebuffer, Output>;

    NewFramebuffer(gpu::Element pixelFormat = gpu::Element::COLOR_SRGBA_32);

    void run(const render::RenderContextPointer& renderContext, Output& output);
protected:
    gpu::Element _pixelFormat;
private:
    gpu::FramebufferPointer _outputFramebuffer;
};

class NewOrDefaultFramebuffer {
public:
    using Input = glm::uvec2;
    using Output = gpu::FramebufferPointer;
    using JobModel = render::Job::ModelIO<NewOrDefaultFramebuffer, Input, Output>;

    void run(const render::RenderContextPointer& renderContext, const Input& input, Output& output);
private:
    gpu::FramebufferPointer _outputFramebuffer;
};

class ResolveFramebuffer {
public:
    using Inputs = render::VaryingSet2<gpu::FramebufferPointer, gpu::FramebufferPointer>;
    using Outputs = gpu::FramebufferPointer;
    using JobModel = render::Job::ModelIO<ResolveFramebuffer, Inputs, Outputs>;

    void run(const render::RenderContextPointer& renderContext, const Inputs& source, Outputs& dest);
};

class ExtractFrustums {
public:

    enum Frustum {
        SHADOW_CASCADE0_FRUSTUM = 0,
        SHADOW_CASCADE1_FRUSTUM,
        SHADOW_CASCADE2_FRUSTUM,
        SHADOW_CASCADE3_FRUSTUM,

        SHADOW_CASCADE_FRUSTUM_COUNT,

        VIEW_FRUSTUM = SHADOW_CASCADE_FRUSTUM_COUNT,

        FRUSTUM_COUNT
    };

    using Inputs = LightStage::ShadowFramePointer;
    using Outputs = render::VaryingArray<ViewFrustumPointer, FRUSTUM_COUNT>;
    using JobModel = render::Job::ModelIO<ExtractFrustums, Inputs, Outputs>;

    void run(const render::RenderContextPointer& renderContext, const Inputs& inputs, Outputs& output);
};

// Sets the pipelines a warm-up asked for (GeometryCache::warmWebBrowserPrograms) so their programs compile in this
// frame on the GPU thread with nothing drawn; nothing at all while no warm-up is pending.
class WarmPipelines {
public:
    using JobModel = render::Job::Model<WarmPipelines>;

    WarmPipelines(bool forward) : _forward(forward) {}

    void run(const render::RenderContextPointer& renderContext);

protected:
    bool _forward;
};

class SetRenderMethod {
public:
    using JobModel = render::Job::Model<SetRenderMethod>;

    SetRenderMethod(render::Args::RenderMethod method) : _method(method) {}

    void run(const render::RenderContextPointer& renderContext) { renderContext->args->_renderMethod = _method; }

protected:
    render::Args::RenderMethod _method;
};

#endif // hifi_RenderDeferredTask_h
