//
//  Created by Bradley Austin Davis 2015/05/29
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once
#ifndef hifi_GLHelpers_h
#define hifi_GLHelpers_h

#include <cstdint>
#include <functional>
#include <QJsonObject>

#include "GLLogging.h"

// 16 bits of depth precision
#define DEFAULT_GL_DEPTH_BUFFER_BITS 16
// 8 bits of stencil buffer (typically you really only need 1 bit for functionality
// but GL implementations usually just come with buffer sizes in multiples of 8)
#define DEFAULT_GL_STENCIL_BUFFER_BITS 8

class QObject;
class QOpenGLDebugMessage;
class QSurfaceFormat;

size_t evalGLFormatSwapchainPixelSize(const QSurfaceFormat& format);

const QSurfaceFormat& getDefaultOpenGLSurfaceFormat();
int glVersionToInteger(QString glVersion);

bool isRenderThread();

#define GL_MAKE_VERSION(major, minor) ((major << 8) | minor)
#define GL_GET_MINOR_VERSION(glversion) (glversion & 0x00FF)
#define GL_GET_MAJOR_VERSION(glversion) ((glversion & 0xFF00) >> 8)

namespace gl {
    struct ContextInfo {
        std::string version;
        std::string shadingLanguageVersion;
        std::string vendor;
        std::string renderer;
        std::vector<std::string> extensions;

        ContextInfo& init();
        operator bool() const { return !version.empty(); }

        static const ContextInfo& get(bool init = false);
    };

#define LOG_GL_CONTEXT_INFO(category, contextInfo) \
    qCDebug(category) << "GL Version: " << contextInfo.version.c_str(); \
    qCDebug(category) << "GL Shader Language Version: " << contextInfo.shadingLanguageVersion.c_str(); \
    qCDebug(category) << "GL Vendor: " << contextInfo.vendor.c_str(); \
    qCDebug(category) << "GL Renderer: " << contextInfo.renderer.c_str();


    void globalLock();
    void globalRelease(bool finish = true);
    // Whether a thread is waiting for the global lock: the lock is not fair, so the thread that holds it most of the
    // time (the presenting thread, with vsync) gives a waiter a moment to take it after a release, or the waiter
    // (the QML UI's renderer, whose sync the main thread waits on) starves. False where GL is not serialized.
    bool globalLockWanted();

    // Whether the presenting thread should leave the window's GL view alone for now (see ViewChangeGuard); macOS only
    bool isPresentationPaused();
    // How the main thread puts its pending window changes in place at once (on macOS, a Core Animation commit,
    // which otherwise waits for the end of the run loop pass): set by the app, run by ViewChangeGuard
    void setViewChangeFlush(const std::function<void()>& flush);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    // How the view's GL context takes the view's new frame once the change is in place (gl::Context::updateForViewChange,
    // on the main thread, under the lock): set by the GL widget, run by ViewChangeGuard. Qt 6 only: Qt 5 queued the
    // context's update to the main thread by itself when the present thread made the context current.
    void setViewChangeContextUpdate(const std::function<void()>& update);
#endif
    // Whether a ViewChangeGuard has ended since the last call: the presenting thread then lets the main thread catch
    // the context up with the view before it draws (on macOS, making the context current on another thread after its
    // view changed queues Qt's update of the context for the main thread, QCocoaGLContext::update, which attaches the
    // view's new surface there while this thread would draw: the crash of 10-18 20:16, Apple's GL driver aborting a
    // clear of the window's framebuffer)
    bool takeViewChanged();
    // Waits until the main thread has run what was queued for it so far (its dispatch queue on macOS), at most
    // milliseconds; returns at once elsewhere. Never with the global lock held: a change of the view waits for it.
    void waitForMainQueue(int milliseconds);

    // While the main thread moves or resizes the window's GL view, the presenting thread doesn't draw to it: the
    // global lock is held for the change, and before it is let go the change is put in place (setViewChangeFlush);
    // a frame's pause follows. macOS resizes a view's surface when Core Animation commits, normally at the end of
    // the main thread's loop pass, after any lock round the change itself: toggling build mode quickly (its panels
    // move and resize the view) crashed in Apple's GL driver, in the clear of the window's framebuffer. Main thread
    // only, nests; nothing under it may wait for the QML render thread, which takes the lock too.
    class ViewChangeGuard {
    public:
        // eventType: the Qt event that changes the view, for the present-stall report (0 for the editor's own changes)
        explicit ViewChangeGuard(int eventType = 0);
        ~ViewChangeGuard();
        ViewChangeGuard(const ViewChangeGuard&) = delete;
        ViewChangeGuard& operator=(const ViewChangeGuard&) = delete;
    };

    // The view changes so far, for the report when presents stop (Application's deadlock watchdog)
    struct ViewChangeState {
        uint32_t guards { 0 };          // changes begun since start
        int lastEventType { 0 };        // the last one's Qt event type, 0 for the editor's own
        int64_t pauseLeftMs { 0 };      // how much longer presents are held off (honoured on macOS only)
    };
    ViewChangeState viewChangeState();

    bool debugContextEnabled();

    bool khrDebugEnabled();

    bool extDebugMarkerEnabled();

    void withSavedContext(const std::function<void()>& f);

    bool checkGLError(const char* name);

    bool checkGLErrorDebug(const char* name);

    bool disableGl45();
    void setDisableGl45(bool disable);

    uint16_t getTargetVersion();

    uint16_t getAvailableVersion();

    uint16_t getRequiredVersion();
} // namespace gl

#define CHECK_GL_ERROR() ::gl::checkGLErrorDebug(__FUNCTION__)

#endif
