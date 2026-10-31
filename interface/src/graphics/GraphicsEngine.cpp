//
//  GraphicsEngine.cpp
//
//  Created by Sam Gateau on 29/6/2018.
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "GraphicsEngine.h"

#include <MeshPartPayload.h>

#include <shared/GlobalAppProperties.h>

#include "WorldBox.h"
#include "LODManager.h"

#include <GeometryCache.h>
#include <TextureCache.h>
#include <FramebufferCache.h>
#include <UpdateSceneTask.h>
#include <RenderViewTask.h>
#include <AmbientOcclusionEffect.h>
#include <BloomEffect.h>
#include <HighlightEffect.h>
#include <Mirrors.h>
#include <RenderLocalShadowTask.h>
#include <RenderMirrorsTask.h>
#include <ImpostorBake.h>
#include <RenderableTerrainEntityItem.h>
#include <RenderShadowTask.h>
#include <ToneMapAndResampleTask.h>
#include <SecondaryCamera.h>

#include "RenderEventHandler.h"

#include <StartupProfiler.h>

#include <gpu/Batch.h>
#include <gpu/Context.h>
#include <gpu/gl/GLBackend.h>
#include <display-plugins/DisplayPlugin.h>

#include <display-plugins/CompositorHelper.h>
#include <FrameDiag.h>
#include <QMetaObject>
#include <ColorUtils.h>
#include "ui/Stats.h"
#include "Application.h"

GraphicsEngine::GraphicsEngine() {
    const QString SPLASH_SKYBOX { "{\"ProceduralEntity\":{ \"version\":2, \"shaderUrl\":\"qrc:///shaders/splashSkybox.frag\" } }" };
    _splashScreen->parse(SPLASH_SKYBOX);
}

GraphicsEngine::~GraphicsEngine() {
}

void GraphicsEngine::initializeGPU(GLWidget* glwidget) {

    _renderEventHandler = new RenderEventHandler(
        [this]() { return this->shouldPaint(); },
        [this]() { this->render_performFrame(); }
    );

    // Requires the window context, because that's what's used in the actual rendering
    // and the GPU backend will make things like the VAO which cannot be shared across
    // contexts
    glwidget->makeCurrent();
    gpu::Context::init<gpu::gl::GLBackend>();
    glwidget->makeCurrent();
    _gpuContext = std::make_shared<gpu::Context>();

#if defined(Q_OS_ANDROID) || defined(DISABLE_PROCEDURAL_DEFAULT_SKYBOX)
#else
    _gpuContext->pushProgramsToSync(shader::allPrograms(), [this] {
        _programsCompiled.store(true);
    }, 1);
#endif

    DependencyManager::get<TextureCache>()->setGPUContext(_gpuContext);
}

namespace {
    const float MIRROR_SHADOW_DISTANCE = 20.0f;   // metres
}

void GraphicsEngine::initializeRender() {

    // Set up the render engine
    int graphStep = StartupProfiler::begin("initializeRender: the task graph");
    render::CullFunctor cullFunctor = LODManager::shouldRender;
    _renderEngine->addJob<UpdateSceneTask>("UpdateScene");
    // the terrain layer sets' slices due this frame, drawn before any view: their batch leaves its framebuffer and
    // viewport bound, which a pass later in the frame must not inherit (the black frames of KNOWN-ISSUES.md)
    _renderEngine->addJob<render::entities::DrawTerrainLayerMaps>("TerrainLayerMaps");
#ifndef Q_OS_ANDROID
    _renderEngine->addJob<SecondaryCameraRenderTask>("SecondaryCameraJob", cullFunctor);
#endif
    // the impostor bake's frames (IMPOSTORS.md 2.2): one a frame while a bake is queued, nothing otherwise
    _renderEngine->addJob<RenderImpostorBakeTask>("RenderImpostorBake", cullFunctor);
    // the mirrors' pictures, drawn before the main view shows them
    _renderEngine->addJob<RenderMirrorsTask>("RenderMirrors", cullFunctor);
    _renderEngine->addJob<RenderViewTask>("RenderMainView", cullFunctor, render::ItemKey::TAG_BITS_0, render::ItemKey::TAG_BITS_0);
    StartupProfiler::end(graphStep);
    {
        StartupProfiler::Scope step("initializeRender: load (every pipeline's shaders)");
        _renderEngine->load();
    }
    int mirrorsStep = StartupProfiler::begin("initializeRender: the mirror views' configuration");
    // a mirror's view: no ambient occlusion, bloom, selection outline or HUD; no local light shadows (an atlas for
    // each would cost too much); the picture kept linear, since it is tone mapped again in the main view
    auto configuration = _renderEngine->getConfiguration();
    for (int slot = 0; slot < Mirrors::SLOT_COUNT; ++slot) {
        std::string view = "RenderMirrors.MirrorView" + std::to_string(slot) + ".";
        // the jobs both branches have, by their full paths: a bare name finds the deferred branch's only. Gamma22 is
        // the linear curve (the sRGB write and read cancel); None raises the colour to 2.2 for debugging, which drew
        // deferred mirrors darker, most of all where the ambient light alone lights them
        for (const char* branch : { "DeferredForwardSwitch.RenderDeferredTask.", "DeferredForwardSwitch.RenderForwardTask." }) {
            if (auto config = configuration->getConfig<ToneMapAndResample>(view + branch + "ToneMapping")) {
                config->setCurve((int)ToneCurve::Gamma22);
            }
            if (auto config = configuration->getJobConfig(view + branch + "RenderHUDLayer")) {
                config->setEnabled(false);
            }
        }
        if (auto config = configuration->getConfig<AmbientOcclusionEffect>(view + "AmbientOcclusion")) {
            config->setEnabled(false);
        }
        if (auto config = configuration->getConfig<BloomEffect>(view + "Bloom")) {
            config->setEnabled(false);
        }
        if (auto config = configuration->getConfig<BloomEffect>(view + "BloomForward")) {
            config->setEnabled(false);
        }
        if (auto config = configuration->getConfig<DrawHighlightTask>(view + "DrawHighlight")) {
            config->setEnabled(false);
        }
        if (auto config = configuration->getConfig<DrawHighlightTask>(view + "DrawHighlightForward")) {
            config->setEnabled(false);
        }
        if (auto config = configuration->getConfig<RenderLocalShadowSetup>(view + "LocalShadowSetup")) {
            config->setLocalLights(false);
        }
        // the sun's shadows reach less far in a reflection (mirrors mostly stand in rooms), and the view is
        // drawn only while the slot has a mirror
        if (auto config = configuration->getConfig<RenderShadowSetup>(view + "ShadowSetup")) {
            config->setProperty("maxDistance", MIRROR_SHADOW_DISTANCE);
        }
        Mirrors::instance().setSlotView(slot, configuration->getJobConfig("RenderMirrors.MirrorView" + std::to_string(slot)));
    }
    StartupProfiler::end(mirrorsStep);
    _renderEngine->registerScene(_renderScene);

    // Now that OpenGL is initialized, we are sure we have a valid context and can create the various pipeline shaders with success.
    {
        StartupProfiler::Scope step("initializeRender: the shape pipelines (GeometryCache)");
        DependencyManager::get<GeometryCache>()->initializeShapePipelines();
    }
}

void GraphicsEngine::startup() {
    static_cast<RenderEventHandler*>(_renderEventHandler)->resumeThread();
}

void GraphicsEngine::shutdown() {
    // The cleanup process enqueues the transactions but does not process them.  Calling this here will force the actual
    // removal of the items.
    // See https://highfidelity.fogbugz.com/f/cases/5328
    _renderScene->enqueueFrame(); // flush all the transactions
    _renderScene->processTransactionQueue(); // process and apply deletions

    _gpuContext->shutdown();


    // shutdown render engine
    _renderScene = nullptr;
    _renderEngine = nullptr;

    _renderEventHandler->deleteLater();
}


void GraphicsEngine::render_runRenderFrame(RenderArgs* renderArgs) {
    PROFILE_RANGE(render, __FUNCTION__);
    PerformanceTimer perfTimer("render");


    if (Menu::getInstance()->isOptionChecked(MenuOption::StopRendering)) return;
    // Make sure the WorldBox is in the scene
    // For the record, this one RenderItem is the first one we created and added to the scene.
    // We could move that code elsewhere but you know...
    if (!render::Item::isValidID(WorldBoxRenderData::_item)) {
        render::Transaction transaction;
        auto worldBoxRenderData = std::make_shared<WorldBoxRenderData>();
        auto worldBoxRenderPayload = std::make_shared<WorldBoxRenderData::Payload>(worldBoxRenderData);

        WorldBoxRenderData::_item = _renderScene->allocateID();

        transaction.resetItem(WorldBoxRenderData::_item, worldBoxRenderPayload);
        _renderScene->enqueueTransaction(transaction);
    }

    {
        MeshPartPayload::beginFrame();   // the draw dump's frame count (Test.dumpDraws), on the thread that draws
        _renderEngine->getRenderContext()->args = renderArgs;
        _renderEngine->run();
    }
}

static const unsigned int THROTTLED_SIM_FRAMERATE = 15;
static const int THROTTLED_SIM_FRAME_PERIOD_MS = MSECS_PER_SECOND / THROTTLED_SIM_FRAMERATE;

bool GraphicsEngine::shouldPaint() const {
    auto displayPlugin = qApp->getActiveDisplayPlugin();
    if (!displayPlugin) {
        // We're shutting down
        return false;
    }

#ifdef DEBUG_PAINT_DELAY
        static uint64_t paintDelaySamples{ 0 };
        static uint64_t paintDelayUsecs{ 0 };

        paintDelayUsecs += displayPlugin->getPaintDelayUsecs();

        static const int PAINT_DELAY_THROTTLE = 1000;
        if (++paintDelaySamples % PAINT_DELAY_THROTTLE == 0) {
            qCDebug(interfaceapp).nospace() <<
                "Paint delay (" << paintDelaySamples << " samples): " <<
                (float)paintDelaySamples / paintDelayUsecs << "us";
        }
#endif

    // Throttle if requested
    //if (displayPlugin->isThrottled() && (_graphicsEngine._renderEventHandler->_lastTimeRendered.elapsed() < THROTTLED_SIM_FRAME_PERIOD_MS)) {
    if (displayPlugin->isThrottled() &&
            (static_cast<RenderEventHandler*>(_renderEventHandler)->_lastTimeRendered.elapsed() < THROTTLED_SIM_FRAME_PERIOD_MS)) {
        return false;
    }

    return true;
}

bool GraphicsEngine::checkPendingRenderEvent() {
    bool expected = false;
    return (_renderEventHandler && static_cast<RenderEventHandler*>(_renderEventHandler)->_pendingRenderEvent.compare_exchange_strong(expected, true));
}

void GraphicsEngine::render_performFrame() {
    // Some plugins process message events, allowing paintGL to be called reentrantly.

    _renderFrameCount++;
    if (_renderFrameCount == 1) {
        StartupProfiler::instant("first frame rendered");
    }

    auto lastPaintBegin = usecTimestampNow();
    PROFILE_RANGE_EX(render, __FUNCTION__, 0xff0000ff, (uint64_t)_renderFrameCount);
    PerformanceTimer perfTimer("paintGL");

    DisplayPluginPointer displayPlugin;
    {
        PROFILE_RANGE(render, "/getActiveDisplayPlugin");
        displayPlugin = qApp->getActiveDisplayPlugin();
        if (!displayPlugin) {
            // We're shutting down
            return;
        }
    }

    {
        PROFILE_RANGE(render, "/pluginBeginFrameRender");
        // If a display plugin loses it's underlying support, it
        // needs to be able to signal us to not use it
        if (!displayPlugin->beginFrameRender(_renderFrameCount)) {
            QMetaObject::invokeMethod(qApp, "updateDisplayMode");
            return;
        }
    }

    RenderArgs renderArgs;
    glm::mat4  HMDSensorPose;
    glm::mat4  eyeToWorld;
    glm::mat4  sensorToWorld;
    ViewFrustum viewFrustum;

    bool isStereo;
    uint64_t traceFrame = 0;
    glm::mat4  stereoEyeOffsets[2];
    glm::mat4  stereoEyeProjections[2];

    {
        QMutexLocker viewLocker(&_renderArgsMutex);
        renderArgs = _appRenderArgs._renderArgs;

        // don't render if there is no context.
        if (!_appRenderArgs._renderArgs._context) {
            return;
        }

        HMDSensorPose = _appRenderArgs._headPose;
        eyeToWorld = _appRenderArgs._eyeToWorld;
        sensorToWorld = _appRenderArgs._sensorToWorld;
        isStereo = _appRenderArgs._isStereo;
        traceFrame = _appRenderArgs._traceFrame;
        for_each_eye([&](Eye eye) {
            stereoEyeOffsets[eye] = _appRenderArgs._eyeOffsets[eye];
            stereoEyeProjections[eye] = _appRenderArgs._eyeProjections[eye];
        });
        viewFrustum = _appRenderArgs._renderArgs.getViewFrustum();
    }

    {
        PROFILE_RANGE(render, "/gpuContextReset");
        getGPUContext()->beginFrame(_appRenderArgs._view, HMDSensorPose);
        // Reset the gpu::Context Stages
        // Back to the default framebuffer;
        gpu::doInBatch("Application_render::gpuContextReset", getGPUContext(), [&](gpu::Batch& batch) {
            batch.resetStages();
        });

        if (isStereo) {
            renderArgs._context->enableStereo(true);
            renderArgs._context->setStereoProjections(stereoEyeProjections);
            renderArgs._context->setStereoViews(stereoEyeOffsets);
        }
    }

    gpu::FramebufferPointer finalFramebuffer;
    QSize finalFramebufferSize;
    {
        PROFILE_RANGE(render, "/getOutputFramebuffer");
        // Primary rendering pass
        auto framebufferCache = DependencyManager::get<FramebufferCache>();
        finalFramebufferSize = framebufferCache->getFrameBufferSize();
        // Final framebuffer that will be handed to the display-plugin
        finalFramebuffer = framebufferCache->getFramebuffer();
    }

    // Until the viewer goes in (Application::isSignInPending), the window shows only Darkride's navy and the user
    // interface over it, where the sign-in screen is: no avatar in an empty world before that screen is up, at the start
    // or after signing out. A headset still gets the 3D view, where its sign-in screen is (SignInGate::createLoginDialog).
    const bool signInCover = qApp->isSignInPending() && !displayPlugin->isHmd();
    auto drawSignInNavy = [&](gpu::Batch& batch) {
        if (!_signInNavy) {
            // the sign-in screen's #1c2136 (LoginDialog.qml), stored as the user interface's texture stores it, so the
            // two come out the same colour
            static const uint8_t NAVY[4] = { 0x1c, 0x21, 0x36, 0xff };
            _signInNavy = gpu::Texture::createStrict(gpu::Element::COLOR_RGBA_32, 1, 1);
            _signInNavy->setSource("GraphicsEngine::_signInNavy");
            _signInNavy->setStoredMipFormat(_signInNavy->getTexelFormat());
            _signInNavy->assignStoredMip(0, sizeof(NAVY), NAVY);
        }
        batch.setFramebuffer(finalFramebuffer);
        // in case the display's drawing isn't ready yet
        batch.clearColorFramebuffer(gpu::Framebuffer::BUFFER_COLOR0,
            ColorUtils::sRGBToLinearVec4(glm::vec4(0x1c / 255.0f, 0x21 / 255.0f, 0x36 / 255.0f, 1.0f)));
        batch.setProjectionTransform(glm::mat4());
        batch.resetViewTransform();
        batch.setModelTransform(Transform());
        displayPlugin->getHUDOperator()(batch, _signInNavy);
    };

    std::queue<SnapshotRequests::Operator> snapshotOperators;
    if (signInCover && !_programsCompiled.load()) {
        gpu::doInBatch("signInFrame", _gpuContext, drawSignInNavy);
    } else if (signInCover) {
        // the scene's changes are still applied, as the render engine would, so they don't pile up while someone signs in
        _renderScene->processTransactionQueue();
        {
            PROFILE_RANGE(render, "/renderOverlay");
            PerformanceTimer perfTimer("renderOverlay");
            renderArgs._viewport = glm::ivec4(0, 0, qApp->getDeviceSize());
            qApp->getApplicationOverlay().renderOverlay(&renderArgs);
        }
        auto hudTexture = qApp->getApplicationOverlay().getOverlayTexture();
        gpu::doInBatch("signInFrame", _gpuContext, [&](gpu::Batch& batch) {
            drawSignInNavy(batch);
            displayPlugin->getHUDOperator()(batch, hudTexture);
        });
    } else if (!_programsCompiled.load()) {
        gpu::doInBatch("splashFrame", _gpuContext, [&](gpu::Batch& batch) {
            batch.setFramebuffer(finalFramebuffer);
            batch.enableSkybox(true);
            batch.enableStereo(isStereo);
            batch.clearDepthStencilFramebuffer(0.0f, 0); // depth 0 is the far plane (reversed-Z)
            batch.setViewportTransform({ 0, 0, finalFramebuffer->getSize() });
            _splashScreen->render(batch, viewFrustum, renderArgs._renderMethod == RenderArgs::RenderMethod::FORWARD);
        });
    } else {
        {
            PROFILE_RANGE(render, "/renderOverlay");
            PerformanceTimer perfTimer("renderOverlay");
            // NOTE: There is no batch associated with this renderArgs
            // the ApplicationOverlay class assumes it's viewport is setup to be the device size
            renderArgs._viewport = glm::ivec4(0, 0, qApp->getDeviceSize());
            qApp->getApplicationOverlay().renderOverlay(&renderArgs);
        }

        {
            PROFILE_RANGE(render, "/updateCompositor");
            qApp->getApplicationCompositor().setFrameInfo(_renderFrameCount, eyeToWorld, sensorToWorld);
        }

        {
            PROFILE_RANGE(render, "/runRenderFrame");
            renderArgs._hudOperator = displayPlugin->getHUDOperator();
            renderArgs._hudTexture = qApp->getApplicationOverlay().getOverlayTexture();
            renderArgs._takingSnapshot = qApp->getSnapshotRequests().take(snapshotOperators);
            renderArgs._blitFramebuffer = finalFramebuffer;
            FrameDiag::beginFrame();
            render_runRenderFrame(&renderArgs);
            // the trace: this frame's camera against the traced item as it was just drawn
            render::ItemID traced = _traceItem.load();
            if (render::Item::isValidID(traced)) {
                const auto& item = _renderScene->getItem(traced);
                if (item.exist()) {
                    glm::vec3 center = item.getBound().calcCenter();
                    std::lock_guard<std::mutex> lock(_traceMutex);
                    if (_traceSamples.size() < 200000) {
                        _traceSamples.push_back({ usecTimestampNow(), viewFrustum.getPosition(), center, _renderFrameCount });
                    }
                }
            }
        }
    }

    if (FrameDiag::enabled()) {
        auto size = finalFramebuffer->getSize();
        FrameDiag::log("R %llu f=%u vp=%dx%d fb=%dx%d@%p items=%d zones=%d sun=%d amb=%d bake=%d shadowMaps=%d drawn=%d static=%d lm=%x",
                       (unsigned long long)usecTimestampNow(), (unsigned)_renderFrameCount, renderArgs._viewport.z,
                       renderArgs._viewport.w, (int)size.x, (int)size.y, (void*)finalFramebuffer.get(),
                       FrameDiag::mainOpaques().load(), FrameDiag::zones().load(), FrameDiag::sunLights().load(),
                       FrameDiag::ambientLights().load(), FrameDiag::bake().load(), renderArgs._details._localShadowMaps,
                       renderArgs._details._localShadowMapsDrawn, renderArgs._details._localShadowStaticMapsDrawn,
                       FrameDiag::lightingFlags().load());
    }

    auto frame = getGPUContext()->endFrame();
    frame->frameIndex = _renderFrameCount;
    frame->traceFrame = traceFrame;
    frame->framebuffer = finalFramebuffer;
    frame->framebufferRecycler = [](const gpu::FramebufferPointer& framebuffer) {
        auto frameBufferCache = DependencyManager::get<FramebufferCache>();
        if (frameBufferCache) {
            frameBufferCache->releaseFramebuffer(framebuffer);
        }
    };
    frame->snapshotOperators = snapshotOperators;
    // deliver final scene rendering commands to the display plugin
    {
        PROFILE_RANGE(render, "/pluginOutput");
        PerformanceTimer perfTimer("pluginOutput");
        _renderLoopCounter.increment();
        displayPlugin->submitFrame(frame);
    }

    // Reset the framebuffer and stereo state
    renderArgs._blitFramebuffer.reset();
    renderArgs._context->enableStereo(false);

    {
        auto stats = Stats::getInstance();
        if (stats) {
            stats->setRenderDetails(renderArgs._details);
        }
    }

    uint64_t lastPaintDuration = usecTimestampNow() - lastPaintBegin;
    _frameTimingsScriptingInterface.addValue(lastPaintDuration);
}

void GraphicsEngine::editRenderArgs(RenderArgsEditor editor) {
    QMutexLocker renderLocker(&_renderArgsMutex);
    editor(_appRenderArgs);
}

void GraphicsEngine::setRenderTraceItem(render::ItemID id) {
    std::lock_guard<std::mutex> lock(_traceMutex);
    _traceSamples.clear();
    _traceItem = id;
}

std::vector<GraphicsEngine::TraceSample> GraphicsEngine::takeRenderTrace() {
    std::lock_guard<std::mutex> lock(_traceMutex);
    std::vector<TraceSample> samples;
    samples.swap(_traceSamples);
    return samples;
}
