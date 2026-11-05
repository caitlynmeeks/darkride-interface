//
//  Created by Bradley Austin Davis on 2015/05/29
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the present thread's loop (PresentThread::run, with the GL lock's handoff), the display link's start and
// watch in activate, present, vsyncApplies and applyVsync (the Vsync setting inert while the display link paces),
// getPresentPeriod (the paced clock physics steps by), the refresh-rate and cadence operators, and the rate counters.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//
#include "OpenGLDisplayPlugin.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <queue>

#include <gl/Config.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QBuffer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QFileInfo>

#include <QtGui/QImage>
#include <QtGui/QImageWriter>
#include <QOpenGLFramebufferObject>   // QtGui on Qt 5, the QtOpenGL module on Qt 6

#include <NumericalConstants.h>
#include <DependencyManager.h>
#include <FrameDiag.h>
#include <GLMHelpers.h>
#include <SharedUtil.h>

#include <gl/QOpenGLContextWrapper.h>
#include <gl/GLWidget.h>
#include <gl/GLEscrow.h>
#include <gl/Context.h>
#include <gl/OffscreenGLCanvas.h>

#include <gpu/Texture.h>
#include <gpu/FrameIO.h>
#include <shaders/Shaders.h>
#include <gpu/gl/GLShared.h>
#include <gpu/gl/GLBackend.h>
#include <gpu/gl/GLTexelFormat.h>
#include <GeometryCache.h>

#include <CursorManager.h>
#include <FramebufferCache.h>
#include <shared/NsightHelpers.h>
#include <ui-plugins/PluginContainer.h>
#include <ui/Menu.h>
#include <CursorManager.h>
#include <TextureCache.h>
#include "CompositorHelper.h"
#include "Logging.h"
#include "RefreshRateController.h"
#include "DisplayLink.h"
#include <FrameTrace.h>

#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

using namespace shader::gpu::program;

extern QThread* RENDER_THREAD;

// Where the present loop is, for the report when presents stop (OpenGLDisplayPlugin::getPresentLoopStage): a relaxed
// store at each of its branches, read once a second by Application's deadlock watchdog
namespace {
    enum PresentStage : int {
        PresentStarting,
        PresentLendingContext,      // the main thread has the context for an operation of its own
        PresentNoPlugin,            // deactivated: minimized, quitting, or between plugins
        PresentHeldForTest,         // OpenGLDisplayPlugin::holdPresents
        PresentPaused,              // gl::isPresentationPaused (a change of the window's view, macOS)
        PresentWaitingForLock,      // gl::globalLock (macOS)
        PresentViewCatchUp,         // the main thread catches the context up with a changed view (macOS)
        PresentPresenting,          // in the plugin's present(): the frame, the composite, the swap
        PresentFinishing,           // the vsync setting, the lock's handoff
        PresentPacingSleep          // RefreshRateController::sleepThreadIfNeeded
    };
    std::atomic<int> presentStage { PresentStarting };
    std::atomic<uint64_t> presentsDone { 0 };
    std::atomic<int64_t> presentHoldUntilMs { 0 };

    void setPresentStage(PresentStage stage) {
        presentStage.store(stage, std::memory_order_relaxed);
    }

    int64_t presentSteadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

class PresentThread : public QThread, public Dependency {
    using Mutex = std::mutex;
    using Condition = std::condition_variable;
    using Lock = std::unique_lock<Mutex>;

public:

    PresentThread() {
        connect(qApp, &QCoreApplication::aboutToQuit, [this] {
            shutdown(); 
        });
        setObjectName("Present");

        _refreshRateController = std::make_shared<RefreshRateController>();
    }

    ~PresentThread() {
        shutdown();
    }

    auto getRefreshRateController() { return _refreshRateController; }

    void shutdown() {
        if (isRunning()) {
            // First ensure that we turn off any current display plugin
            setNewDisplayPlugin(nullptr);

            Lock lock(_mutex);
            _shutdown = true;
            _condition.wait(lock, [&] { return !_shutdown; });
            qCDebug(displayPlugins) << "Present thread shutdown";
        }
    }

    void setNewDisplayPlugin(OpenGLDisplayPlugin* plugin) {
        Lock lock(_mutex);
        if (isRunning()) {
            _newPluginQueue.push(plugin);
            _condition.wait(lock, [=]()->bool { return _newPluginQueue.empty(); });
        }
    }

    void setContext(gl::Context* context) {
        // Move the OpenGL context to the present thread
        // Extra code because of the widget 'wrapper' context
        _context = context;
        _context->doneCurrent();
        _context->moveToThread(this);
    }

    // FROZEN (vehicle path): see the note at the top of this file.
    virtual void run() override {
        PROFILE_SET_THREAD_NAME("Present Thread");

        // FIXME determine the best priority balance between this and the main thread...
        // It may be dependent on the display plugin being used, since VR plugins should
        // have higher priority on rendering (although we could say that the Oculus plugin
        // doesn't need that since it has async timewarp).
        // A higher priority here
        setPriority(QThread::HighPriority);
        OpenGLDisplayPlugin* currentPlugin{ nullptr };
        Q_ASSERT(_context);
        _context->makeCurrent();
        CHECK_GL_ERROR();
        while (!_shutdown) {
            if (_pendingOtherThreadOperation) {
                setPresentStage(PresentLendingContext);
                PROFILE_RANGE(render, "MainThreadOp")
                {
                    Lock lock(_mutex);
                    _context->doneCurrent();
                    // Move the context to the main thread
                    _context->moveToThread(_targetOperationThread);
                    _pendingOtherThreadOperation = false;
                    // Release the main thread to do it's action
                    _condition.notify_one();
                }

                {
                    // Main thread does it's thing while we wait on the lock to release
                    Lock lock(_mutex);
                    _condition.wait(lock, [&] { return _finishedOtherThreadOperation; });
                }
                _context->makeCurrent();
            }

            // Check for a new display plugin
            {
                Lock lock(_mutex);
                // Check if we have a new plugin to activate
                while (!_newPluginQueue.empty()) {
                    auto newPlugin = _newPluginQueue.front();
                    if (newPlugin != currentPlugin) {
                        // Deactivate the old plugin
                        if (currentPlugin != nullptr) {
                            currentPlugin->uncustomizeContext();
                            CHECK_GL_ERROR();
                            // Force completion of all pending GL commands
                            glFinish();
                        }

                        if (newPlugin) {
                            bool hasVsync = false;
                            QThread::setPriority(newPlugin->getPresentPriority());
                            bool wantVsync = newPlugin->vsyncApplies();
#if defined(Q_OS_MACOS)
                            newPlugin->swapBuffers();
#endif
                            gl::setSwapInterval(wantVsync ? 1 : 0);
#if defined(Q_OS_MACOS)
                            newPlugin->swapBuffers();
#endif
                            hasVsync = gl::getSwapInterval() != 0;
                            qCDebug(displayPlugins) << "Vsync enabled:" << hasVsync;
                            if (newPlugin->wantVsync() && !wantVsync) {
                                qCDebug(displayPlugins) << "Vsync: the setting is on, but the display link paces the presents in phase with the display: the swap does not wait";
                            }

                            newPlugin->setVsyncEnabled(hasVsync);
                            newPlugin->_vsyncApplied = wantVsync;
                            newPlugin->customizeContext();
                            CHECK_GL_ERROR();
                            // Force completion of all pending GL commands
                            glFinish();
                        }
                        currentPlugin = newPlugin;
                    }
                    _newPluginQueue.pop();
                    _condition.notify_one();
                }
            }

            // If there's no active plugin, just sleep
            if (currentPlugin == nullptr) {
                setPresentStage(PresentNoPlugin);
                // Minimum sleep ends up being about 2 ms anyway
                QThread::msleep(1);
                continue;
            }
            if (int64_t holdUntil = presentHoldUntilMs.load(std::memory_order_relaxed)) {
                if (presentSteadyMs() < holdUntil) {
                    setPresentStage(PresentHeldForTest);
                    QThread::msleep(2);
                    continue;
                }
            }

            // While the main thread moves or resizes the window's view, and for a moment after, nothing is drawn to
            // it (see gl::ViewChangeGuard): a view changed under a frame crashed inside Apple's GL driver. The frames
            // queue meanwhile, and the next present shows the newest.
            if (gl::isPresentationPaused()) {
                setPresentStage(PresentPaused);
                QThread::msleep(2);
                continue;
            }
            // The whole frame, making the context current included, is drawn under the lock a change of the view
            // takes: on macOS the context catches up with a changed view when it is made current.
            setPresentStage(PresentWaitingForLock);
            gl::globalLock();
            if (gl::isPresentationPaused()) {
                // a change began while this waited for the lock
                setPresentStage(PresentPaused);
                gl::globalRelease(false);
                QThread::msleep(2);
                continue;
            }
#if defined(Q_OS_MACOS)
            _context->makeCurrent();
            // After a change of the view, making the context current on this thread has Qt queue the context's update
            // for the main thread (QCocoaGLContext::update), which there attaches the view's new surface: drawing
            // meanwhile crashed in Apple's GL driver (10-18). The lock is let go while the main thread runs it, and the
            // next pass draws.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            if (gl::takeViewChanged()) {
                setPresentStage(PresentViewCatchUp);
                _context->doneCurrent();
                gl::globalRelease(false);
                const int VIEW_UPDATE_WAIT_MS = 100;
                gl::waitForMainQueue(VIEW_UPDATE_WAIT_MS);
                continue;
            }
#else
            // Qt 6: the view-change guard ran the context's update on the main thread before it let the lock go, and
            // this thread's makeCurrent is native (gl::Context::makeCurrent), so there is nothing to wait for
#endif
#endif
            // Execute the frame and present it to the display device.
            {
                PROFILE_RANGE(render, "PluginPresent")
                setPresentStage(PresentPresenting);
                currentPlugin->present(_refreshRateController);
                CHECK_GL_ERROR();
            }
            presentsDone.fetch_add(1, std::memory_order_relaxed);
            setPresentStage(PresentFinishing);
            // the vsync setting, changed at run time, takes from the next present
            currentPlugin->applyVsync();
#if defined(Q_OS_MACOS)
            _context->doneCurrent();
#endif
            gl::globalRelease(false);
            // The global lock is not fair. With vsync on this thread holds it through the swap's wait and, with
            // nothing to sleep for, took it straight back: the QML UI's renderer never got it, and the main thread,
            // which waits on that renderer's sync, ran its game loop at a frame a second (10-19). A waiter gets a
            // moment to take the lock; this thread then queues behind it as it should.
            {
                const int HANDOFF_WAIT_US = 100;
                const int HANDOFF_TRIES = 30;   // up to 3 ms, a fraction of the UI's own render
                for (int i = 0; i < HANDOFF_TRIES && gl::globalLockWanted(); ++i) {
                    QThread::usleep(HANDOFF_WAIT_US);
                }
            }

            setPresentStage(PresentPacingSleep);
            _refreshRateController->sleepThreadIfNeeded(this, currentPlugin->isHmd(), currentPlugin->isVsyncEnabled());
        }

        _context->doneCurrent();
        Lock lock(_mutex);
        _context->moveToThread(qApp->thread());
        _shutdown = false;
        _condition.notify_one();
    }

    void withOtherThreadContext(std::function<void()> f) {
        // Signal to the thread that there is work to be done on the main thread
        Lock lock(_mutex);
        _targetOperationThread = QThread::currentThread();
        _pendingOtherThreadOperation = true;
        _finishedOtherThreadOperation = false;
        _condition.wait(lock, [&] { return !_pendingOtherThreadOperation; });

        _context->makeCurrent();
        f();
        _context->doneCurrent();

        _targetOperationThread = nullptr;
        // Move the context back to the presentation thread
        _context->moveToThread(this);

        // restore control of the context to the presentation thread and signal
        // the end of the operation
        _finishedOtherThreadOperation = true;
        lock.unlock();
        _condition.notify_one();
    }

private:
    void makeCurrent();
    void doneCurrent();

    bool _shutdown { false };
    Mutex _mutex;
    // Used to allow the main thread to perform context operations
    Condition _condition;

    QThread* _targetOperationThread { nullptr };
    bool _pendingOtherThreadOperation { false };
    bool _finishedOtherThreadOperation { false };
    std::queue<OpenGLDisplayPlugin*> _newPluginQueue;
    gl::Context* _context { nullptr };
    std::shared_ptr<RefreshRateController> _refreshRateController { nullptr };
};

// FROZEN (vehicle path): see the note at the top of this file.
bool OpenGLDisplayPlugin::activate() {
    if (!_cursorsData.size()) {
        auto& cursorManager = Cursor::Manager::instance();
        for (const auto iconId : cursorManager.registeredIcons()) {
            auto& cursorData = _cursorsData[iconId];
            auto iconPath = cursorManager.getIconImage(iconId);
            auto image = QImage(iconPath);
            image = image.mirrored();
            image = image.convertToFormat(QImage::Format_RGBA8888);
            cursorData.image = image;
            cursorData.size = toGlm(image.size());
            cursorData.hotSpot = vec2(0.5f);
        }
    }
    if (!_container) {
        return false;
    }
    // the vsync setting (Render.vsyncEnabled; the present thread applies it when this plugin goes on)
    _vsyncWanted = _container->getBoolSetting("vsyncEnabled", false);

    // Start the present thread if necessary
    QSharedPointer<PresentThread> presentThread;
    if (DependencyManager::isSet<PresentThread>()) {
        presentThread = DependencyManager::get<PresentThread>();
    } else {
        auto widget = _container->getPrimaryWidget();
        DependencyManager::set<PresentThread>();
        presentThread = DependencyManager::get<PresentThread>();
        presentThread->setObjectName("Presentation Thread");
        if (!widget->context()->makeCurrent()) {
            throw std::runtime_error("Failed to make context current");
        }
        CHECK_GL_ERROR();
        widget->context()->doneCurrent();

        presentThread->setContext(widget->context());
        // Start execution
        presentThread->start();
    }
    _presentThread = presentThread.data();
    if (!RENDER_THREAD) {
        RENDER_THREAD = _presentThread;
    }

    // Child classes may override this in order to do things like initialize
    // libraries, etc
    if (!internalActivate()) {
        return false;
    }

    // the refresh signal of the display the window is on, for pacing the presents in phase with its refreshes
    // (DisplayLink, RefreshRateController); it follows the window from screen to screen
    if (!isHmd()) {
        auto widget = _container->getPrimaryWidget();
        QWindow* window = (widget && widget->window()) ? widget->window()->windowHandle() : nullptr;
        if (window) {
            // the link runs while the window can be seen: a minimized or hidden window has nothing to pace, and the
            // link's thread would wake at every refresh for nothing (10-21 review). The display is the one under the
            // window's own centre (its screen's centre when the window has no size yet): until 2026-10-31 it was the
            // centre of the screen Qt reported, and Qt's screen-change signal was the only thing that moved the link,
            // so one wrong answer at a window's change of state left the link on the wrong display for the rest of
            // the run (a ride on the 144 Hz panel paced by the 120 Hz one's refreshes: the free schedule, unknowingly).
            auto displayUnderWindow = [window]() -> uint32_t {
                QRect frame = window->frameGeometry();
                QPoint centre = frame.isValid() && !frame.isEmpty() ? frame.center()
                    : (window->screen() ? window->screen()->geometry().center() : QPoint(0, 0));
                uint32_t display = DisplayLink::displayAt(centre.x(), centre.y());
                if (!display && window->screen()) {
                    QPoint screenCentre = window->screen()->geometry().center();
                    display = DisplayLink::displayAt(screenCentre.x(), screenCentre.y());
                }
                return display;
            };
            auto startLink = [window, displayUnderWindow] {
                QWindow::Visibility visibility = window->visibility();
                if (visibility == QWindow::Minimized || visibility == QWindow::Hidden) {
                    DisplayLink::instance().stop();
                    return;
                }
                DisplayLink::instance().start(displayUnderWindow());
            };
            startLink();
            QObject::disconnect(_screenConnection);
            QObject::disconnect(_visibilityConnection);
            _screenConnection = connect(window, &QWindow::screenChanged, this, [startLink](QScreen*) { startLink(); });
            _visibilityConnection = connect(window, &QWindow::visibilityChanged, this, [startLink](QWindow::Visibility) { startLink(); });
            // the watchdog, every 2 s on the main thread: a link on another display than the window's, or one that has
            // gone quiet while the window can be seen, is started again on the window's display, and said so
            if (!_linkWatchdog) {
                _linkWatchdog = new QTimer(this);
                _linkWatchdog->setInterval(2000);
                _linkWatchdog->setTimerType(Qt::VeryCoarseTimer);
            }
            _linkWatchdog->disconnect();
            connect(_linkWatchdog, &QTimer::timeout, this, [this, window, displayUnderWindow] {
                QWindow::Visibility visibility = window->visibility();
                if (visibility == QWindow::Minimized || visibility == QWindow::Hidden) {
                    return;
                }
                auto& link = DisplayLink::instance();
                uint32_t wanted = displayUnderWindow();
                int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const char* reason = nullptr;
                if (!link.isRunning()) {
                    reason = "it was not running";
                } else if (link.isQuiet(nowNs)) {
                    reason = "it went quiet";
                } else if (wanted && wanted != link.displayId()) {
                    reason = "the window is on another display";
                }
                if (!reason) {
                    _linkQuietRestarts = 0;   // running and refreshing on the window's display
                    _linkNextRestartNs = 0;
                    return;
                }
                // a display that is asleep, or a link that is stuck, stays quiet after a restart too: after the first
                // restart that did not help, one more try a minute, said once (performance's review, 2026-10-31)
                const bool quietAgain = link.isQuiet(nowNs) && _linkQuietRestarts > 0;
                if (quietAgain) {
                    if (nowNs < _linkNextRestartNs) {
                        return;
                    }
                    _linkNextRestartNs = nowNs + 60 * 1000000000LL;
                    if (_linkQuietRestarts == 1) {
                        qCDebug(displayPlugins) << "Display link: still quiet after a restart on display" << wanted << "; trying again once a minute";
                    }
                    _linkQuietRestarts++;
                } else {
                    qCDebug(displayPlugins) << "Display link: restarted on display" << wanted << "(was" << link.displayId() << "):" << reason;
                    _linkQuietRestarts = link.isQuiet(nowNs) ? 1 : 0;
                    _linkNextRestartNs = 0;
                }
                link.stop();
                link.start(wanted);
            });
            _linkWatchdog->start();
        }
    }

    // This should not return until the new context has been customized
    // and the old context (if any) has been uncustomized
    presentThread->setNewDisplayPlugin(this);

    auto compositorHelper = DependencyManager::get<CompositorHelper>();
    connect(compositorHelper.data(), &CompositorHelper::alphaChanged, [this] {
        auto compositorHelper = DependencyManager::get<CompositorHelper>();
        auto animation = new QPropertyAnimation(this, "hudAlpha");
        animation->setDuration(200);
        animation->setEndValue(compositorHelper->getAlpha());
        animation->start();
    });

    if (isHmd() && (getHmdScreen() >= 0)) {
        _container->showDisplayPluginsTools();
    }

    return Parent::activate();
}

void OpenGLDisplayPlugin::deactivate() {
    auto compositorHelper = DependencyManager::get<CompositorHelper>();
    disconnect(compositorHelper.data());

    // the display link paces this plugin's presents only: off with it (the next desktop plugin starts it again)
    QObject::disconnect(_screenConnection);
    QObject::disconnect(_visibilityConnection);
    if (_linkWatchdog) {
        _linkWatchdog->stop();
        _linkWatchdog->disconnect();
    }
    DisplayLink::instance().stop();

    auto presentThread = DependencyManager::get<PresentThread>();
    // Does not return until the GL transition has completeed
    presentThread->setNewDisplayPlugin(nullptr);
    internalDeactivate();

    _container->showDisplayPluginsTools(false);
    if (!_container->currentDisplayActions().isEmpty()) {
        foreach (auto itemInfo, _container->currentDisplayActions()) {
            _container->removeMenuItem(itemInfo.first, itemInfo.second);
        }
        _container->currentDisplayActions().clear();
    }
    Parent::deactivate();
}

bool OpenGLDisplayPlugin::startStandBySession() {
    if (!activateStandBySession()) {
        return false;
    }
    return Parent::startStandBySession();
}

void OpenGLDisplayPlugin::endSession() {
    deactivateSession();
    Parent::endSession();
}

void OpenGLDisplayPlugin::customizeContext() {
    auto presentThread = DependencyManager::get<PresentThread>();
    Q_ASSERT(thread() == presentThread->thread());

    getGLBackend()->setCameraCorrection(mat4(), mat4(), true);

    for (auto& cursorValue : _cursorsData) {
        auto& cursorData = cursorValue.second;
        if (!cursorData.texture) {
            auto image = cursorData.image;
            if (image.format() != QImage::Format_ARGB32) {
                image = image.convertToFormat(QImage::Format_ARGB32);
            }
            if ((image.width() > 0) && (image.height() > 0)) {
                cursorData.texture = gpu::Texture::createStrict(
                    gpu::Element(gpu::VEC4, gpu::NUINT8, gpu::RGBA),
                    image.width(), image.height(),
                    gpu::Texture::MAX_NUM_MIPS,
                    gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_MIP_LINEAR));
                cursorData.texture->setSource("cursor texture");
                auto usage = gpu::Texture::Usage::Builder().withColor().withAlpha();
                cursorData.texture->setUsage(usage.build());
                cursorData.texture->setStoredMipFormat(gpu::Element(gpu::VEC4, gpu::NUINT8, gpu::RGBA));
                cursorData.texture->assignStoredMip(0, image.sizeInBytes(), image.constBits());
                cursorData.texture->setAutoGenerateMips(true);
            }
        }
    }

    if (!_linearToSRGBPipeline) {
        gpu::StatePointer blendState = gpu::StatePointer(new gpu::State());
        blendState->setDepthTest(gpu::State::DepthTest(false));
        blendState->setBlendFunction(true,
                                     gpu::State::SRC_ALPHA, gpu::State::BLEND_OP_ADD,
                                     gpu::State::INV_SRC_ALPHA,
                                     gpu::State::FACTOR_ALPHA, gpu::State::BLEND_OP_ADD,
                                     gpu::State::ONE);

        gpu::StatePointer scissorState = gpu::StatePointer(new gpu::State());
        scissorState->setDepthTest(gpu::State::DepthTest(false));
        scissorState->setScissorEnable(true);

        _drawTexturePipeline = gpu::Pipeline::create(gpu::Shader::createProgram(DrawTexture), scissorState);

        _drawTextureSqueezePipeline =
            gpu::Pipeline::create(gpu::Shader::createProgram(shader::display_plugins::program::DrawTextureWithVisionSqueeze), scissorState);

        _linearToSRGBPipeline = gpu::Pipeline::create(gpu::Shader::createProgram(DrawTextureLinearToSRGB), scissorState);

        _SRGBToLinearPipeline = gpu::Pipeline::create(gpu::Shader::createProgram(DrawTextureSRGBToLinear), scissorState);

        _hudPipeline = gpu::Pipeline::create(gpu::Shader::createProgram(DrawTextureSRGBToLinear), blendState);

        _cursorPipeline = gpu::Pipeline::create(gpu::Shader::createProgram(DrawTransformedTexture), blendState);
    }

    updateCompositeFramebuffer();
}

void OpenGLDisplayPlugin::uncustomizeContext() {

    _drawTexturePipeline.reset();
    _drawTextureSqueezePipeline.reset();
    _linearToSRGBPipeline.reset();
    _SRGBToLinearPipeline.reset();
    _cursorPipeline.reset();
    _hudPipeline.reset();
    _compositeFramebuffer.reset();

    withPresentThreadLock([&] {
        _currentFrame.reset();
        _lastFrame = nullptr;
        while (!_newFrameQueue.empty()) {
            _gpuContext->consumeFrameUpdates(_newFrameQueue.front());
            _newFrameQueue.pop();
        }
    });
}

// Pressing Alt (and Meta) key alone activates the menubar because its style inherits the
// SHMenuBarAltKeyNavigation from QWindowsStyle. This makes it impossible for a scripts to
// receive keyPress events for the Alt (and Meta) key in a reliable manner.
//
// This filter catches events before QMenuBar can steal the keyboard focus.
// The idea was borrowed from
// http://www.archivum.info/qt-interest@trolltech.com/2006-09/00053/Re-(Qt4)-Alt-key-focus-QMenuBar-(solved).html

// Pass input events on to the application
bool OpenGLDisplayPlugin::eventFilter(QObject* receiver, QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:

        case QEvent::TouchBegin:
        case QEvent::TouchEnd:
        case QEvent::TouchUpdate:

        case QEvent::FocusIn:
        case QEvent::FocusOut:

        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:

        case QEvent::DragEnter:
        case QEvent::Drop:

        case QEvent::Resize:
            if (QCoreApplication::sendEvent(QCoreApplication::instance(), event)) {
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void OpenGLDisplayPlugin::submitFrame(const gpu::FramePointer& newFrame) {
    newFrame->submitTime = usecTimestampNow();
    withNonPresentThreadLock([&] {
        _newFrameQueue.push(newFrame);
    });
}

ktx::StoragePointer textureToKtx(const gpu::Texture& texture) {
    ktx::Header header;
    {
        auto gpuDims = texture.getDimensions();
        header.pixelWidth = gpuDims.x;
        header.pixelHeight = gpuDims.y;
        header.pixelDepth = 0;
    }

    {
        auto gltexelformat = gpu::gl::GLTexelFormat::evalGLTexelFormat(texture.getStoredMipFormat());
        header.glInternalFormat = gltexelformat.internalFormat;
        header.glFormat = gltexelformat.format;
        header.glBaseInternalFormat = gltexelformat.format;
        header.glType = gltexelformat.type;
        header.glTypeSize = 1;
        header.numberOfMipmapLevels = 1 + texture.getMaxMip();
    }

    auto memKtx = ktx::KTX::createBare(header);
    auto storage = memKtx->_storage;
    uint32_t faceCount = std::max(header.numberOfFaces, 1u);
    uint32_t mipCount = std::max(header.numberOfMipmapLevels, 1u);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        for (uint32_t face = 0; face < faceCount; ++face) {
            const auto& image = memKtx->_images[mip];
            auto& faceBytes = const_cast<gpu::Byte*&>(image._faceBytes[face]);
            if (texture.isStoredMipFaceAvailable(mip, face)) {
                auto storedImage = texture.accessStoredMipFace(mip, face);
                if (storedImage) {
                    memcpy(faceBytes, storedImage->data(), storedImage->size());
                }
            }
        }
    }
    return storage;
}

void OpenGLDisplayPlugin::captureFrame(const std::string& filename) const {
    withOtherThreadContext([&] {
        using namespace gpu;
        TextureCapturer captureLambda = [&](const gpu::TexturePointer& texture)->storage::StoragePointer {
            return textureToKtx(*texture);
        };

        if (_currentFrame) {
            gpu::writeFrame(filename, _currentFrame, captureLambda);
        }
    });
}

void OpenGLDisplayPlugin::renderFromTexture(gpu::Batch& batch,
                                            const gpu::TexturePointer& texture,
                                            const glm::ivec4& viewport,
                                            const glm::ivec4& scissor) {
    renderFromTexture(batch, texture, viewport, scissor, nullptr);
}

void OpenGLDisplayPlugin::renderFromTexture(gpu::Batch& batch,
                                            const gpu::TexturePointer& texture,
                                            const glm::ivec4& viewport,
                                            const glm::ivec4& scissor,
                                            const gpu::FramebufferPointer& copyFbo /*=gpu::FramebufferPointer()*/) {
    auto fbo = gpu::FramebufferPointer();
    batch.enableStereo(false);
    batch.resetViewTransform();
    batch.setFramebuffer(fbo);
    batch.clearColorFramebuffer(gpu::Framebuffer::BUFFER_COLOR0, vec4(0));
    batch.setStateScissorRect(scissor);
    batch.setViewportTransform(viewport);
    batch.setResourceTexture(0, texture);

    batch.setPipeline(getRenderTexturePipeline());

    batch.draw(gpu::TRIANGLE_STRIP, 4);
    if (copyFbo) {
        gpu::Vec4i copyFboRect(0, 0, copyFbo->getWidth(), copyFbo->getHeight());
        gpu::Vec4i sourceRect(scissor.x, scissor.y, scissor.x + scissor.z, scissor.y + scissor.w);
        float aspectRatio = (float)scissor.w / (float)scissor.z;  // height/width
        // scale width first
        int xOffset = 0;
        int yOffset = 0;
        int newWidth = copyFbo->getWidth();
        int newHeight = std::round(aspectRatio * (float) copyFbo->getWidth());
        if (newHeight > copyFbo->getHeight()) {
            // ok, so now fill height instead
            newHeight = copyFbo->getHeight();
            newWidth = std::round((float)copyFbo->getHeight() / aspectRatio);
            xOffset = (copyFbo->getWidth() - newWidth) / 2;
        } else {
            yOffset = (copyFbo->getHeight() - newHeight) / 2;
        }
        gpu::Vec4i copyRect(xOffset, yOffset, xOffset + newWidth, yOffset + newHeight);
        batch.setFramebuffer(copyFbo);

        batch.resetViewTransform();
        batch.setViewportTransform(copyFboRect);
        batch.setStateScissorRect(copyFboRect);
        batch.clearColorFramebuffer(gpu::Framebuffer::BUFFER_COLOR0, {0.0f, 0.0f, 0.0f, 1.0f});
        batch.blit(fbo, sourceRect, copyFbo, copyRect);
    }
}

void OpenGLDisplayPlugin::updateFrameData() {
    PROFILE_RANGE(render, __FUNCTION__)
    if (_lockCurrentTexture) {
        return;
    }
    withPresentThreadLock([&] {
        if (!_newFrameQueue.empty()) {
            // We're changing frames, so we can cleanup any GL resources that might have been used by the old frame
            _gpuContext->recycle();
        }
        if (_newFrameQueue.size() > 1) {
            _droppedFrameRate.increment(_newFrameQueue.size() - 1);
        }

        _gpuContext->processProgramsToSync();

        int queued = (int)_newFrameQueue.size();
        while (!_newFrameQueue.empty()) {
            _currentFrame = _newFrameQueue.front();
            _newFrameQueue.pop();
            _gpuContext->consumeFrameUpdates(_currentFrame);
        }

        if (FrameDiag::enabled()) {
            static gpu::Frame* lastLogged = nullptr;
            bool changed = _currentFrame.get() != lastLogged;
            lastLogged = _currentFrame.get();
            glm::uvec2 scene(0);
            void* scenePointer = nullptr;
            if (_currentFrame && _currentFrame->framebuffer) {
                scene = _currentFrame->framebuffer->getSize();
                scenePointer = _currentFrame->framebuffer.get();
            }
            glm::uvec2 composite(0);
            if (_compositeFramebuffer) {
                composite = _compositeFramebuffer->getSize();
            }
            FrameDiag::log("P %llu f=%u new=%d queued=%d scene=%ux%u@%p comp=%ux%u", (unsigned long long)usecTimestampNow(),
                           (unsigned)(_currentFrame ? _currentFrame->frameIndex : 0), changed ? 1 : 0, queued, scene.x,
                           scene.y, scenePointer, composite.x, composite.y);
        }
    });
}

std::function<void(gpu::Batch&, const gpu::TexturePointer&)> OpenGLDisplayPlugin::getHUDOperator() {
    auto hudPipeline = _hudPipeline;
    auto hudCompositeFramebufferSize = getRecommendedRenderSize();
    return [=](gpu::Batch& batch, const gpu::TexturePointer& hudTexture) {
        if (hudPipeline && hudTexture) {
            batch.setPipeline(hudPipeline);
            batch.setResourceTexture(0, hudTexture);
            batch.setViewportTransform(ivec4(uvec2(0), hudCompositeFramebufferSize));
            batch.draw(gpu::TRIANGLE_STRIP, 4);
        }
    };
}

void OpenGLDisplayPlugin::compositePointer() {
    auto& cursorManager = Cursor::Manager::instance();
    const auto& cursorData = _cursorsData[cursorManager.getCursor()->getIcon()];
    auto cursorTransform = DependencyManager::get<CompositorHelper>()->getReticleTransform(glm::mat4());
    render([&](gpu::Batch& batch) {
        batch.setProjectionTransform(mat4());
        batch.setFramebuffer(_compositeFramebuffer);
        batch.setPipeline(_cursorPipeline);
        batch.setResourceTexture(0, cursorData.texture);
        batch.resetViewTransform();
        batch.setModelTransform(cursorTransform);
        batch.setViewportTransform(ivec4(uvec2(0), _compositeFramebuffer->getSize()));
        batch.draw(gpu::TRIANGLE_STRIP, 4);
    });
}

void OpenGLDisplayPlugin::setupCompositeScenePipeline(gpu::Batch& batch) {
    batch.setPipeline(_drawTexturePipeline);
}

void OpenGLDisplayPlugin::compositeScene() {
    render([&](gpu::Batch& batch) {
        batch.enableStereo(false);
        batch.setFramebuffer(_compositeFramebuffer);
        ivec2 size = _compositeFramebuffer->getSize();
        ivec4 viewport(ivec2(0), size);
        // A frame made for a view of another shape (the view has just been resized and the scene not yet caught up)
        // is fitted in with bars rather than stretched to fill: a frame or two of bars, not a squashed picture
        const float ASPECT_TOLERANCE = 0.02f;
        auto sceneTexture = _currentFrame->framebuffer->getRenderBuffer(0);
        if (sceneTexture && sceneTexture->getHeight() > 0 && size.x > 0 && size.y > 0) {
            float sceneAspect = (float)sceneTexture->getWidth() / (float)sceneTexture->getHeight();
            float viewAspect = (float)size.x / (float)size.y;
            if (std::abs(sceneAspect / viewAspect - 1.0f) > ASPECT_TOLERANCE) {
                batch.setStateScissorRect(viewport);
                batch.clearColorFramebuffer(gpu::Framebuffer::BUFFER_COLOR0, vec4(0.0f, 0.0f, 0.0f, 1.0f));
                if (sceneAspect > viewAspect) {
                    int height = (int)std::round((float)size.x / sceneAspect);
                    viewport = ivec4(0, (size.y - height) / 2, size.x, height);
                } else {
                    int width = (int)std::round((float)size.y * sceneAspect);
                    viewport = ivec4((size.x - width) / 2, 0, width, size.y);
                }
            }
        }
        batch.setViewportTransform(viewport);
        batch.setStateScissorRect(viewport);
        batch.resetViewTransform();
        batch.setProjectionTransform(mat4());
        batch.setResourceTexture(0, sceneTexture);
        setupCompositeScenePipeline(batch);
        batch.draw(gpu::TRIANGLE_STRIP, 4);
    });
}

void OpenGLDisplayPlugin::compositeLayers() {
    updateCompositeFramebuffer();

    {
        PROFILE_RANGE_EX(render_detail, "compositeScene", 0xff0077ff, (uint64_t)presentCount())
        compositeScene();
    }

    {
        PROFILE_RANGE_EX(render_detail, "compositeExtra", 0xff0077ff, (uint64_t)presentCount())
        compositeExtra();
    }

    auto& cursorManager = Cursor::Manager::instance();
    if (isHmd() || cursorManager.getCursor()->getIcon() == Cursor::RETICLE) {
        auto compositorHelper = DependencyManager::get<CompositorHelper>();
        // Draw the pointer last so it's on top of everything
        if (compositorHelper->getReticleVisible()) {
            PROFILE_RANGE_EX(render_detail, "compositePointer", 0xff0077ff, (uint64_t)presentCount())
            compositePointer();
        }
    }
}

void OpenGLDisplayPlugin::internalPresent() {
    render([&](gpu::Batch& batch) {
        // Note: _displayTexture must currently be the same size as the display.
        uvec2 dims = _displayTexture ? uvec2(_displayTexture->getDimensions()) : getSurfacePixels();
        auto viewport = ivec4(uvec2(0), dims);
        renderFromTexture(batch, _displayTexture ? _displayTexture : _compositeFramebuffer->getRenderBuffer(0), viewport, viewport);
     });
    swapBuffers();
    _presentRate.increment();
}

void OpenGLDisplayPlugin::tracePresent(uint64_t startPresent) {
    if (!_presentTracing) {
        return;
    }
    // the swap has returned: with vsync on, at the display's refresh (or at once, when the driver queued the frame)
    uint64_t now = usecTimestampNow();
    std::lock_guard<std::mutex> lock(_presentTraceMutex);
    if (_presentTrace.size() < 200000) {
        _presentTrace.push_back({ now, _currentFrame ? _currentFrame->frameIndex : 0u, _presentedNewFrame,
                                  (uint32_t)(now - startPresent), _lastSwapUsecs,
                                  _gpuContext ? (float)_gpuContext->getFrameTimerGPUAverage() : 0.0f });
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
bool OpenGLDisplayPlugin::vsyncApplies() const {
    static const bool forced = !qgetenv("TIVOLI_VSYNC").isEmpty();
    return wantVsync() && (forced || !DisplayLink::instance().isRunning());
}

// FROZEN (vehicle path): see the note at the top of this file.
void OpenGLDisplayPlugin::applyVsync() {
    bool wanted = vsyncApplies();
    if (wanted == _vsyncApplied) {
        return;
    }
    _vsyncApplied = wanted;
    gl::setSwapInterval(wanted ? 1 : 0);
    bool hasVsync = gl::getSwapInterval() != 0;
    if (wantVsync() && !wanted) {
        qCDebug(displayPlugins) << "Vsync: the setting is on, but the display link paces the presents in phase with the display: the swap does not wait; enabled:" << hasVsync;
    } else {
        qCDebug(displayPlugins) << "Vsync" << (wanted ? "on" : "off") << "asked for at run time; enabled:" << hasVsync;
    }
    _vsyncEnabled = hasVsync;
}

void OpenGLDisplayPlugin::setPresentTracing(bool on) {
    std::lock_guard<std::mutex> lock(_presentTraceMutex);
    _presentTrace.clear();
    _presentTracing = on;
}

std::vector<DisplayPlugin::PresentSample> OpenGLDisplayPlugin::takePresentTrace() {
    std::lock_guard<std::mutex> lock(_presentTraceMutex);
    std::vector<PresentSample> samples;
    samples.swap(_presentTrace);
    return samples;
}

static std::atomic<int>& presentLoadUs() {
    static std::atomic<int> load { qgetenv("TIVOLI_PRESENT_LOAD_US").toInt() };
    return load;
}

void OpenGLDisplayPlugin::setPresentLoad(int microseconds) {
    presentLoadUs() = std::max(0, microseconds);
}

int OpenGLDisplayPlugin::getPresentLoad() {
    return presentLoadUs();
}

uint64_t OpenGLDisplayPlugin::getPresentsDone() {
    return presentsDone.load(std::memory_order_relaxed);
}

const char* OpenGLDisplayPlugin::getPresentLoopStage() {
    switch (presentStage.load(std::memory_order_relaxed)) {
        case PresentStarting: return "starting";
        case PresentLendingContext: return "lending the context to the main thread";
        case PresentNoPlugin: return "without a display plugin";
        case PresentHeldForTest: return "held for a test";
        case PresentPaused: return "paused for a change of the window's view";
        case PresentWaitingForLock: return "waiting for the global GL lock";
        case PresentViewCatchUp: return "waiting for the main thread to catch the context up with the view";
        case PresentPresenting: return "presenting (the frame, the composite, the swap)";
        case PresentFinishing: return "finishing a present (vsync, the lock's handoff)";
        case PresentPacingSleep: return "in the pacing sleep";
        default: return "unknown";
    }
}

bool OpenGLDisplayPlugin::isPresentLoopIdle() {
    return presentStage.load(std::memory_order_relaxed) == PresentNoPlugin;
}

void OpenGLDisplayPlugin::holdPresents(int milliseconds) {
    presentHoldUntilMs.store(milliseconds > 0 ? presentSteadyMs() + milliseconds : 0, std::memory_order_relaxed);
}

// FROZEN (vehicle path): see the note at the top of this file.
void OpenGLDisplayPlugin::present(const std::shared_ptr<RefreshRateController>& refreshRateController) {
    auto frameId = (uint64_t)presentCount();
    PROFILE_RANGE_EX(render, __FUNCTION__, 0xffffff00, frameId)
    uint64_t startPresent = usecTimestampNow();
    refreshRateController->clockStartTime();
    if (int load = presentLoadUs()) {
        // the test knob (setPresentLoad): a heavier present, busy-waited
        while (usecTimestampNow() - startPresent < (uint64_t)load) {
        }
    }
    {
        PROFILE_RANGE_EX(render, "updateFrameData", 0xff00ff00, frameId)
        updateFrameData();
    }
    _presentSlot = refreshRateController->getSlotIndex();
    incrementPresentCount();

    if (_currentFrame) {
        auto correction = getViewCorrection();
        getGLBackend()->setCameraCorrection(correction, _prevRenderView);
        _prevRenderView = correction * _currentFrame->view;
        {
            withPresentThreadLock([&] {
                _renderRate.increment();
                _presentedNewFrame = _currentFrame.get() != _lastFrame;
                if (_presentedNewFrame) {
                    _newFrameRate.increment();
                }
                _lastFrame = _currentFrame.get();
            });
            refreshRateController->notePresentedFrame(_presentedNewFrame);
            if (_presentedNewFrame) {
                // the frame's own work, the main thread's frame and the render thread's build, stamped on it by the
                // game loop and the graphics engine: the governor's measure of whether the frames would fit a refresh
                // at the display's full rate (not a latency from the present before: with vsync that held the swap's
                // wait for the refresh, and the governor never came back)
                refreshRateController->noteNewFrame((int)_currentFrame->mainTime, (int)_currentFrame->buildTime);
            }
            if (FrameTrace::enabled()) {
                // the frame trace's present columns, in the record of the game-loop frame this gpu frame came from:
                // the gpu frame's index (the join with Test.takePresentTrace), the refreshes the frame has been shown
                // for (summed over held presents), whether it was held, and how late this present came past its slot
                const uint64_t traceFrame = _currentFrame->traceFrame;
                const int refreshes = refreshRateController->getPresentRefreshes();
                _tracePresentRefreshes = _presentedNewFrame ? refreshes : _tracePresentRefreshes + refreshes;
                FrameTrace::set(traceFrame, "render.frame", (double)_currentFrame->frameIndex);
                FrameTrace::set(traceFrame, "present.refreshes", (double)_tracePresentRefreshes);
                FrameTrace::set(traceFrame, "present.repeat", _presentedNewFrame ? 0.0 : 1.0);
                FrameTrace::set(traceFrame, "present.late_us", (double)refreshRateController->getPresentLateUs());
            }
            if (_presentedNewFrame || isHmd()) {
                // Execute the frame rendering commands. A held present (the same frame shown again because no new one
                // was ready) keeps the frame's framebuffer as it was drawn and only composites it again, instead of
                // drawing the whole scene a second time for the same picture: until 2026-10-31 the GPU's time doubled
                // on exactly the frames that were late already, which made the next one late too. An HMD re-executes
                // with the newer pose: that is its reprojection.
                PROFILE_RANGE_EX(render, "execute", 0xff00ff00, frameId)
                _gpuContext->executeFrame(_currentFrame);
            }
        }

        // Write all layers to a local framebuffer
        {
            PROFILE_RANGE_EX(render, "composite", 0xff00ffff, frameId)
            compositeLayers();
        }

        if (FrameDiag::enabled()) {
            // the black-frame hunt: the brightness of the presented picture's centre (a read-back, so a GPU sync a
            // frame), and what the GPU did executing this frame
            gpu::ContextStats stats;
            _gpuContext->getFrameStats(stats);
            auto size = _compositeFramebuffer->getSize();
            const int SAMPLE = 32;
            QImage sample(SAMPLE, SAMPLE, QImage::Format_ARGB32);
            getGLBackend()->downloadFramebuffer(_compositeFramebuffer,
                ivec4((int)size.x / 2 - SAMPLE / 2, (int)size.y / 2 - SAMPLE / 2, SAMPLE, SAMPLE), sample);
            uint64_t sum = 0;
            for (int y = 0; y < SAMPLE; ++y) {
                const QRgb* row = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
                for (int x = 0; x < SAMPLE; ++x) {
                    sum += qRed(row[x]) + 2 * qGreen(row[x]) + qBlue(row[x]);
                }
            }
            gpu::ContextStats total;
            _gpuContext->getStats(total);
            std::string pipelines = FrameDiag::takePipelines(total._DSNumDrawcalls);
            FrameDiag::log("C %llu f=%u centre=%llu draws=%u tris=%u pipelines=%u texBound=%u texGPU=%u texMem=%llu pending=%u pipes=%s",
                           (unsigned long long)usecTimestampNow(), (unsigned)_currentFrame->frameIndex,
                           (unsigned long long)(sum / (4 * SAMPLE * SAMPLE)), stats._DSNumDrawcalls, stats._DSNumTriangles,
                           stats._PSNumSetPipelines, stats._RSNumTextureBounded, gpu::Context::getTextureGPUCount(),
                           (unsigned long long)gpu::Context::getTextureGPUMemSize(),
                           gpu::Context::getTexturePendingGPUTransferCount(), pipelines.c_str());
        }

        { // If we have any snapshots this frame, handle them
            PROFILE_RANGE_EX(render, "snapshotOperators", 0xffff00ff, frameId)
            while (!_currentFrame->snapshotOperators.empty()) {
                auto& snapshotOperator = _currentFrame->snapshotOperators.front();
                if (std::get<2>(snapshotOperator)) {
                    std::get<0>(snapshotOperator)(getScreenshot(std::get<1>(snapshotOperator)));
                } else {
                    std::get<0>(snapshotOperator)(getSecondaryCameraScreenshot());
                }
                _currentFrame->snapshotOperators.pop();
            }
        }

        // Take the composite framebuffer and send it to the output device
        refreshRateController->clockEndTime();
        {
            PROFILE_RANGE_EX(render, "internalPresent", 0xff00ffff, frameId)
            internalPresent();
        }
        tracePresent(startPresent);

        gpu::Backend::freeGPUMemSize.set(gpu::gl::getFreeDedicatedMemory());
    } else if (alwaysPresent()) {
        refreshRateController->clockEndTime();
        internalPresent();
        tracePresent(startPresent);
    } else {
        refreshRateController->clockEndTime();
    }
    uint64_t presentUsecs = usecTimestampNow() - startPresent;
    _movingAveragePresent.addSample((float)presentUsecs);
    // the present's own work: the present less its swap (with vsync the swap waits for the refresh, which is not work)
    refreshRateController->notePresentCost(presentUsecs > _lastSwapUsecs ? (int)(presentUsecs - _lastSwapUsecs) : 0,
                                           _gpuContext ? (float)_gpuContext->getFrameTimerGPUAverage() : 0.0f);
    _previousPresentStart = startPresent;
    if (FrameTrace::enabled() && _currentFrame) {
        // how long before the refresh this present's picture was due the present returned; negative when it missed
        const int64_t deadlineNs = refreshRateController->getPresentDeadlineNs();
        if (deadlineNs > 0) {
            const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            FrameTrace::set(_currentFrame->traceFrame, "present.slack_us", (double)(deadlineNs - nowNs) / 1000.0);
        }
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
float OpenGLDisplayPlugin::newFramePresentRate() const {
    return _newFrameRate.rate();
}

// FROZEN (vehicle path): see the note at the top of this file.
float OpenGLDisplayPlugin::droppedFrameRate() const {
    return _droppedFrameRate.rate();
}

// FROZEN (vehicle path): see the note at the top of this file.
float OpenGLDisplayPlugin::presentRate() const {
    return _presentRate.rate();
}

// FROZEN (vehicle path): see the note at the top of this file.
float OpenGLDisplayPlugin::getPresentPeriod() const {
    // paced by the controller unless the swap paces (vsync, an HMD)
    if (isHmd() || _vsyncEnabled || !DependencyManager::isSet<PresentThread>()) {
        return 0.0f;
    }
    return DependencyManager::get<PresentThread>()->getRefreshRateController()->getPacedPeriodSeconds();
}

// FROZEN (vehicle path): see the note at the top of this file.
std::function<void(int, int)> OpenGLDisplayPlugin::getRefreshRateOperator() {
    return [](int targetRefreshRate, int displayRefreshRate) {
        auto refreshRateController = DependencyManager::get<PresentThread>()->getRefreshRateController();
        refreshRateController->setRefreshRateLimitPeriod(targetRefreshRate);
        refreshRateController->setDisplayRefreshRate(displayRefreshRate);
    };
}

// FROZEN (vehicle path): see the note at the top of this file.
std::function<RefreshRateController::CadenceCounters()> OpenGLDisplayPlugin::getCadenceCountersOperator() {
    return []() -> RefreshRateController::CadenceCounters {
        if (!DependencyManager::isSet<PresentThread>()) {
            return RefreshRateController::CadenceCounters();
        }
        return DependencyManager::get<PresentThread>()->getRefreshRateController()->getCadenceCounters();
    };
}

// FROZEN (vehicle path): see the note at the top of this file.
void OpenGLDisplayPlugin::resetPresentRate() {
    // FIXME
    // _presentRate = RateCounter<100>();
}

// FROZEN (vehicle path): see the note at the top of this file.
float OpenGLDisplayPlugin::renderRate() const {
    return _renderRate.rate();
}

void OpenGLDisplayPlugin::swapBuffers() {
    static auto context = _container->getPrimaryWidget()->context();
    uint64_t start = usecTimestampNow();
    context->swapBuffers();
    _lastSwapUsecs = (uint32_t)(usecTimestampNow() - start);
}

void OpenGLDisplayPlugin::withOtherThreadContext(std::function<void()> f) const {
    static auto presentThread = DependencyManager::get<PresentThread>();
    presentThread->withOtherThreadContext(f);
    if (!OffscreenGLCanvas::restoreThreadContext()) {
        qWarning("Unable to restore original OpenGL context");
    }
}

bool OpenGLDisplayPlugin::setDisplayTexture(const QString& name) {
    // Note: it is the caller's responsibility to keep the network texture in cache.
    if (name.isEmpty()) {
        _displayTexture.reset();
        onDisplayTextureReset();
        return true;
    }
    auto textureCache = DependencyManager::get<TextureCache>();
    auto displayNetworkTexture = textureCache->getTexture(name);
    if (!displayNetworkTexture) {
        return false;
    }
    _displayTexture = displayNetworkTexture->getGPUTexture();
    return !!_displayTexture;
}

QImage OpenGLDisplayPlugin::getScreenshot(float aspectRatio) {
    auto size = _compositeFramebuffer->getSize();
    if (isHmd()) {
        size.x /= 2;
    }
    auto bestSize = size;
    uvec2 corner(0);
    if (aspectRatio != 0.0f) { // Pick out the largest piece of the center that produces the requested width/height aspectRatio
        if (ceil(size.y * aspectRatio) < size.x) {
            bestSize.x = round(size.y * aspectRatio);
        } else {
            bestSize.y = round(size.x / aspectRatio);
        }
        corner.x = round((size.x - bestSize.x) / 2.0f);
        corner.y = round((size.y - bestSize.y) / 2.0f);
    }
    QImage screenshot(bestSize.x, bestSize.y, QImage::Format_ARGB32);
    getGLBackend()->downloadFramebuffer(_compositeFramebuffer, ivec4(corner, bestSize), screenshot);
    return screenshot.mirrored(false, true);
}

QImage OpenGLDisplayPlugin::getSecondaryCameraScreenshot() {
    auto textureCache = DependencyManager::get<TextureCache>();
    auto secondaryCameraFramebuffer = textureCache->getSpectatorCameraFramebuffer();
    gpu::Vec4i region(0, 0, secondaryCameraFramebuffer->getWidth(), secondaryCameraFramebuffer->getHeight());

    QImage screenshot(region.z, region.w, QImage::Format_ARGB32);
    getGLBackend()->downloadFramebuffer(secondaryCameraFramebuffer, region, screenshot);
    return screenshot.mirrored(false, true);
}

glm::uvec2 OpenGLDisplayPlugin::getSurfacePixels() const {
    uvec2 result;
    auto window = _container->getPrimaryWidget();
    if (window) {
        // the exact ratio: QWidget::devicePixelRatio() is a whole number, 1 at Windows' 125 % or 150 %, which drew the
        // view at two-thirds of the window's pixels; the GL context already uses the exact one (gl/Context.cpp)
        result = toGlm(window->geometry().size() * window->devicePixelRatioF());
#if defined(Q_OS_WIN)
        // the view's own window as Windows has it: just after a change of the display's scale it can differ from the
        // widget's size until the size is put again (ViewportContainer), and a frame drawn at the widget's shape was
        // stretched into it, squashed sideways. Drawn at the window's own pixels, the picture keeps its shape
        RECT client;
        HWND hwnd = (HWND)window->internalWinId();
        if (hwnd && GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0) {
            result = uvec2(client.right, client.bottom);
        }
#endif
    }
    return result;
}

glm::uvec2 OpenGLDisplayPlugin::getSurfaceSize() const {
    uvec2 result;
    auto window = _container->getPrimaryWidget();
    if (window) {
        result = toGlm(window->geometry().size());
    }
    return result;
}

void OpenGLDisplayPlugin::assertNotPresentThread() const {
    Q_ASSERT(QThread::currentThread() != _presentThread);
}

void OpenGLDisplayPlugin::assertIsPresentThread() const {
    Q_ASSERT(QThread::currentThread() == _presentThread);
}

bool OpenGLDisplayPlugin::beginFrameRender(uint32_t frameIndex) {
    withNonPresentThreadLock([&] {
        _compositeHUDAlpha = _hudAlpha;
    });
    return Parent::beginFrameRender(frameIndex);
}

gpu::gl::GLBackend* OpenGLDisplayPlugin::getGLBackend() {
    if (!_gpuContext || !_gpuContext->getBackend()) {
        return nullptr;
    }
    auto backend = _gpuContext->getBackend().get();
#if defined(Q_OS_MACOS)
    // Should be dynamic_cast, but that doesn't work in plugins on OSX
    auto glbackend = static_cast<gpu::gl::GLBackend*>(backend);
#else
    auto glbackend = dynamic_cast<gpu::gl::GLBackend*>(backend);
#endif

    return glbackend;
}

void OpenGLDisplayPlugin::render(std::function<void(gpu::Batch& batch)> f) {
    gpu::Batch batch;
    f(batch);
    _gpuContext->executeBatch(batch);
}

OpenGLDisplayPlugin::~OpenGLDisplayPlugin() {
}

void OpenGLDisplayPlugin::updateCompositeFramebuffer() {
    auto renderSize = getRecommendedRenderSize();
    if (!_compositeFramebuffer || _compositeFramebuffer->getSize() != renderSize) {
        _compositeFramebuffer = gpu::FramebufferPointer(gpu::Framebuffer::create("OpenGLDisplayPlugin::composite", gpu::Element::COLOR_SRGBA_32, renderSize.x, renderSize.y));
    }
}

void OpenGLDisplayPlugin::copyTextureToQuickFramebuffer(NetworkTexturePointer networkTexture, QOpenGLFramebufferObject* target, GLsync* fenceSync) {
#if !defined(USE_GLES)
    auto glBackend = const_cast<OpenGLDisplayPlugin&>(*this).getGLBackend();
    withOtherThreadContext([&] {
        GLuint sourceTexture = glBackend->getTextureID(networkTexture->getGPUTexture());
        GLuint targetTexture = target->texture();
        GLuint fbo[2] {0, 0};

        // need mipmaps for blitting texture
        glGenerateTextureMipmap(sourceTexture);

        // create 2 fbos (one for initial texture, second for scaled one)
        glCreateFramebuffers(2, fbo);

        // setup source fbo
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTexture, 0);

        GLint texWidth = networkTexture->getWidth();
        GLint texHeight = networkTexture->getHeight();

        // setup destination fbo
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // maintain aspect ratio, filling the width first if possible.  If that makes the height too
        // much, fill height instead. TODO: only do this when texture changes
        GLint newX = 0;
        GLint newY = 0;
        float aspectRatio = (float)texHeight / (float)texWidth;
        GLint newWidth = target->width();
        GLint newHeight = std::round(aspectRatio * (float)target->width());
        if (newHeight > target->height()) {
            newHeight = target->height();
            newWidth = std::round((float)target->height() / aspectRatio);
            newX = (target->width() - newWidth) / 2;
        } else {
            newY = (target->height() - newHeight) / 2;
        }

        glBlitNamedFramebuffer(fbo[0], fbo[1], 0, 0, texWidth, texHeight, newX, newY, newX + newWidth, newY + newHeight, GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // don't delete the textures!
        glDeleteFramebuffers(2, fbo);
        *fenceSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    });
#endif
}

gpu::PipelinePointer OpenGLDisplayPlugin::getRenderTexturePipeline() {
    return _drawTexturePipeline;
}
