//
//  Created by Bradley Austin Davis on 2015/05/29
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#pragma once

#include "DisplayPlugin.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include <QtCore/QThread>
#include <QtCore/QByteArray>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <GLMHelpers.h>
#include <SimpleMovingAverage.h>
#include <shared/RateCounter.h>

#include <gpu/Batch.h>

namespace gpu { namespace gl {
class GLBackend;
}}  // namespace gpu::gl

class RefreshRateController;

class OpenGLDisplayPlugin : public DisplayPlugin {
    Q_OBJECT
    Q_PROPERTY(float hudAlpha MEMBER _hudAlpha)
    using Parent = DisplayPlugin;

protected:
    using Mutex = std::mutex;
    using Lock = std::unique_lock<Mutex>;
    using Condition = std::condition_variable;

public:
    ~OpenGLDisplayPlugin();
    // These must be final to ensure proper ordering of operations
    // between the main thread and the presentation thread

    // (the target rate, the display's own rate)
    static std::function<void(int, int)> getRefreshRateOperator();
    // A test knob: every present costs this many microseconds more, busy-waited, as a heavy scene's would (the
    // pacing's overrun cases, 10-21 review). TIVOLI_PRESENT_LOAD_US in the environment sets it at start, Test.setPresentLoad
    // while running; 0, the default, is no load.
    static void setPresentLoad(int microseconds);
    static int getPresentLoad();
    // The present loop's state, for the report when presents stop (Application's deadlock watchdog; release-1.1's
    // Windows run of 10-21, where presents and new frames stopped while the loop kept going): the frames it has
    // presented, where it is now, and whether it has no display plugin on purpose (deactivated: minimized, quitting).
    // Kept with relaxed atomic stores, so it costs the loop nothing to speak of.
    static uint64_t getPresentsDone();
    static const char* getPresentLoopStage();
    static bool isPresentLoopIdle();
    // A test knob: the present loop presents nothing for this long, as a stall would (Test.holdPresents)
    static void holdPresents(int milliseconds);

    bool activate() override final;
    void deactivate() override final;
    bool startStandBySession() override final;
    void endSession() override final;
    bool eventFilter(QObject* receiver, QEvent* event) override;
    bool isDisplayVisible() const override { return true; }
    void captureFrame(const std::string& outputName) const override;
    void submitFrame(const gpu::FramePointer& newFrame) override;

    glm::uvec2 getRecommendedRenderSize() const override { return getSurfacePixels(); }

    glm::uvec2 getRecommendedUiSize() const override { return getSurfaceSize(); }

    virtual bool setDisplayTexture(const QString& name) override;
    virtual bool onDisplayTextureReset() { return false; };

    float presentRate() const override;

    void resetPresentRate() override;

    float newFramePresentRate() const override;

    float droppedFrameRate() const override;

    float renderRate() const override;

    bool beginFrameRender(uint32_t frameIndex) override;

    // Vsync: the swap waits for the display's refresh. Off by default: the presents are paced at the display's period
    // instead (RefreshRateController) and the compositor shows the newest at each refresh, which is even; with vsync
    // on, macOS's GL swap was measured (10-19) to wait two refreshes and to queue the odd frame without waiting, a
    // ragged 72 Hz on a 144 Hz panel (PHYSICS.md "The driver's stutter"). Wanted = the Render.vsyncEnabled setting,
    // read at activation and pushed on a change; TIVOLI_VSYNC=1 or 0 in the environment forces it; the HMD plugins
    // never want it (HmdDisplayPlugin::wantVsync: the headset's compositor paces them). The present thread applies it
    // (applyVsync), but not while the display link paces the presents (macOS, vsyncApplies): they are in phase with the
    // display already, 2 ms after each refresh, and a swap that waited on top would hold the GL lock through the
    // refresh, take the presents off the paced clock (getPresentPeriod) and hide a missed refresh from the cadence
    // governor, which cannot see the display's refreshes through the swap (Caity's ride of 2026-10-31: the governor
    // blind at the full rate, the car juddering standing still; the setting had been ticked the night before).
    virtual bool wantVsync() const {
        static const int forced = qgetenv("TIVOLI_VSYNC").isEmpty() ? -1 : (qgetenv("TIVOLI_VSYNC") != "0" ? 1 : 0);
        return forced < 0 ? _vsyncWanted.load() : (forced != 0);
    }
    // whether the swap is to wait for the refresh now: wanted, and no display link pacing the presents. A forced
    // TIVOLI_VSYNC=1 applies even under the link: a measurement knob that reproduces the 2026-10-31 14:40 state (the
    // cadence governor blind under the swap's wait, the presents off the paced clock), never a way to turn vsync on.
    bool vsyncApplies() const;
    void setVsyncEnabled(bool vsyncEnabled) { _vsyncEnabled = vsyncEnabled; }
    bool isVsyncEnabled() const override { return _vsyncEnabled; }
    void setVsyncWanted(bool wanted) override { _vsyncWanted = wanted; }
    float getPresentPeriod() const override;
    void setPresentTracing(bool on) override;
    std::vector<PresentSample> takePresentTrace() override;
    // Three threads, one for rendering, one for texture transfers, one reserved for the GL driver
    // Drop to one reserved for better other-task performance in desktop
    int getRequiredThreadCount() const override { return 1; }

    virtual std::function<void(gpu::Batch&, const gpu::TexturePointer&)> getHUDOperator() override;
    void copyTextureToQuickFramebuffer(NetworkTexturePointer source,
                                       QOpenGLFramebufferObject* target,
                                       GLsync* fenceSync) override;

protected:
    friend class PresentThread;

    glm::uvec2 getSurfaceSize() const;
    glm::uvec2 getSurfacePixels() const;
    // Some display plugins require us to always execute some present logic,
    // whether we have a frame or not (Oculus Mobile plugin)
    // Such plugins must be prepared to do the right thing if the `_currentFrame`
    // is not populated
    virtual bool alwaysPresent() const { return false; }

    void updateCompositeFramebuffer();

    virtual QThread::Priority getPresentPriority() { return QThread::HighPriority; }
    virtual void compositeLayers();
    virtual void setupCompositeScenePipeline(gpu::Batch& batch);
    virtual void compositeScene();
    virtual void compositePointer();
    virtual void compositeExtra(){};

    // These functions must only be called on the presentation thread
    virtual void customizeContext();
    virtual void uncustomizeContext();

    // Returns true on successful activation
    virtual bool internalActivate() { return true; }
    virtual void internalDeactivate() {}

    // Returns true on successful activation of standby session
    virtual bool activateStandBySession() { return true; }
    virtual void deactivateSession() {}

    // Plugin specific functionality to send the composed scene to the output window or device
    virtual void internalPresent();

    void renderFromTexture(gpu::Batch& batch,
                           const gpu::TexturePointer& texture,
                           const glm::ivec4& viewport,
                           const glm::ivec4& scissor,
                           const gpu::FramebufferPointer& fbo);
    void renderFromTexture(gpu::Batch& batch,
                           const gpu::TexturePointer& texture,
                           const glm::ivec4& viewport,
                           const glm::ivec4& scissor);
    virtual void updateFrameData();
    virtual glm::mat4 getViewCorrection() { return glm::mat4(); }

    void withOtherThreadContext(std::function<void()> f) const;

    void present(const std::shared_ptr<RefreshRateController>& refreshRateController);
    virtual void swapBuffers();
    // present thread only, the context current: sets the swap interval to what is wanted when that changed
    void applyVsync();
    // present thread only, after the swap: records this present when the trace is on
    void tracePresent(uint64_t startPresent);

    void render(std::function<void(gpu::Batch& batch)> f);

    std::atomic<bool> _vsyncEnabled{ false };   // what the swap does (the driver's answer)
    std::atomic<bool> _vsyncWanted{ false };    // the setting
    bool _vsyncApplied{ false };                // what the present thread last asked the driver for
    std::atomic<bool> _presentTracing{ false };
    std::mutex _presentTraceMutex;
    std::vector<PresentSample> _presentTrace;
    bool _presentedNewFrame{ false };           // this present's frame was not shown before
    uint32_t _lastSwapUsecs{ 0 };               // how long the last swap took (the wait for the display, with vsync)
    QMetaObject::Connection _screenConnection;  // the window's screen change restarts the display link
    QMetaObject::Connection _visibilityConnection;  // minimizing the window stops it, showing it starts it
    QThread* _presentThread{ nullptr };
    std::queue<gpu::FramePointer> _newFrameQueue;
    RateCounter<200> _droppedFrameRate;
    RateCounter<200> _newFrameRate;
    RateCounter<200> _presentRate;
    RateCounter<200> _renderRate;

    gpu::FramePointer _currentFrame;
    gpu::Frame* _lastFrame{ nullptr };
    mat4 _prevRenderView;
    gpu::FramebufferPointer _compositeFramebuffer;
    gpu::PipelinePointer _hudPipeline;
    gpu::PipelinePointer _mirrorHUDPipeline;
    gpu::ShaderPointer _mirrorHUDPS;
    gpu::PipelinePointer _drawTexturePipeline;
    gpu::PipelinePointer _drawTextureSqueezePipeline;
    gpu::PipelinePointer _linearToSRGBPipeline;
    gpu::PipelinePointer _SRGBToLinearPipeline;
    gpu::PipelinePointer _cursorPipeline;
    gpu::TexturePointer _displayTexture{};
    float _compositeHUDAlpha{ 1.0f };

    virtual gpu::PipelinePointer getRenderTexturePipeline();

    struct CursorData {
        QImage image;
        vec2 hotSpot;
        uvec2 size;
        gpu::TexturePointer texture;
    };

    std::map<uint16_t, CursorData> _cursorsData;
    bool _lockCurrentTexture{ false };

    void assertNotPresentThread() const;
    void assertIsPresentThread() const;

    template <typename F>
    void withPresentThreadLock(F f) const {
        assertIsPresentThread();
        Lock lock(_presentMutex);
        f();
    }

    template <typename F>
    void withNonPresentThreadLock(F f) const {
        assertNotPresentThread();
        Lock lock(_presentMutex);
        f();
    }

    gpu::gl::GLBackend* getGLBackend();

    // Any resource shared by the main thread and the presentation thread must
    // be serialized through this mutex
    mutable Mutex _presentMutex;
    float _hudAlpha{ 1.0f };

    QImage getScreenshot(float aspectRatio);
    QImage getSecondaryCameraScreenshot();
};
