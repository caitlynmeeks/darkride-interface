//
//  Created by Bradley Austin Davis on 2016/08/21
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

// Note, `gl::Context is split into two files because a single file cannot include both the GLAD headers 
// and the QOpenGLContext definition headers

#include "Context.h"

#include "Config.h"

#include <QtGui/QWindow>
#include "QOpenGLContextWrapper.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/qopenglcontext_platform.h>
#elif defined(Q_OS_WIN)
#include <QtPlatformHeaders/QWGLNativeContext>
#endif

#include <QOpenGLDebugMessage>   // QtGui on Qt 5, the QtOpenGL module on Qt 6

#include "GLHelpers.h"
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <objc/message.h>
#include <objc/runtime.h>
namespace {
    // The NSOpenGLContext behind a Qt context (Qt 6's native interface) and a message to it, from C++
    void* nativeGLContext(QOpenGLContext* context) {
        auto cocoa = context ? context->nativeInterface<QNativeInterface::QCocoaGLContext>() : nullptr;
        return cocoa ? (void*)cocoa->nativeContext() : nullptr;
    }
    void sendMessage(void* target, const char* selector) {
        if (target) {
            ((void (*)(void*, SEL))objc_msgSend)(target, sel_registerName(selector));
        }
    }
    void sendMessage(void* target, const char* selector, void* argument) {
        if (target) {
            ((void (*)(void*, SEL, void*))objc_msgSend)(target, sel_registerName(selector), argument);
        }
    }
    void sendMessage(void* target, const char* selector, bool argument) {
        if (target) {
            ((void (*)(void*, SEL, bool))objc_msgSend)(target, sel_registerName(selector), argument);
        }
    }
    bool onMainThread() {
        return QThread::currentThread() == QCoreApplication::instance()->thread();
    }
}
#endif

using namespace gl;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
// [[NSAutoreleasePool alloc] init] and -drain through the runtime (this file is C++): what Qt's own pool does
AutoreleasePool::AutoreleasePool() {
    void* pool = ((void* (*)(void*, SEL))objc_msgSend)((void*)objc_getClass("NSAutoreleasePool"), sel_registerName("alloc"));
    _pool = ((void* (*)(void*, SEL))objc_msgSend)(pool, sel_registerName("init"));
}

AutoreleasePool::~AutoreleasePool() {
    if (_pool) {
        ((void (*)(void*, SEL))objc_msgSend)(_pool, sel_registerName("drain"));
    }
}
#endif

void Context::destroyContext(QOpenGLContext* context) {
    context->deleteLater();
}

void Context::makeCurrent(QOpenGLContext* context, QSurface* surface) {
    context->makeCurrent(surface);
}

#if defined(GL_CUSTOM_CONTEXT)
void Context::createWrapperContext() {
    if (0 != _hglrc && nullptr == _qglContext) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        // Qt 6 adopts a native context through its platform interface (QtPlatformHeaders are gone)
        _qglContext = QNativeInterface::QWGLContext::fromNative(_hglrc, _hwnd);
#else
        _qglContext = new QOpenGLContext();
        _qglContext->setNativeHandle(QVariant::fromValue(QWGLNativeContext(_hglrc, _hwnd)));
        _qglContext->create();
#endif
    }
}
#endif


void Context::moveToThread(QThread* thread) {
    if (_qglContext) {
        _qglContext->moveToThread(thread);
    }
}

void Context::debugMessageHandler(const QOpenGLDebugMessage& debugMessage) {
    auto type = debugMessage.type();
    if (type == QOpenGLDebugMessage::PerformanceType) {
        return;
    }

    auto severity = debugMessage.severity();
    switch (severity) {
        case QOpenGLDebugMessage::NotificationSeverity:
        case QOpenGLDebugMessage::LowSeverity:
            qCDebug(glLogging) << debugMessage;
            return;
        default:
            qCWarning(glLogging) << debugMessage;
            break;
    }
}

void Context::setupDebugLogging(QOpenGLContext *context) {
    QOpenGLDebugLogger *logger = new QOpenGLDebugLogger(context);
    QObject::connect(logger, &QOpenGLDebugLogger::messageLogged, context, [](const QOpenGLDebugMessage& message){
        Context::debugMessageHandler(message);
    });
    if (logger->initialize()) {
        logger->enableMessages();
        logger->startLogging(QOpenGLDebugLogger::SynchronousLogging);
    } else {
        qCWarning(glLogging) <<  "OpenGL context does not support debugging";
    }
}


#if !defined(GL_CUSTOM_CONTEXT)
bool Context::makeCurrent() {
    updateSwapchainMemoryCounter();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    // Off the main thread (the present thread), the context is made current natively. Qt 6's makeCurrent runs
    // -[NSOpenGLContext update] on the calling thread when the view's frame changed (Qt 5 queued it to the main
    // thread), and AppKit on macOS 26 traps in it off the main thread (the first Qt 6 runs, 11-04). The main thread's
    // calls stay Qt's: the first one attaches the view to the context, and an update there is where AppKit allows it;
    // after a change of the view, the view-change guard runs the update on the main thread (updateForViewChange).
    if (!onMainThread()) {
        sendMessage(nativeGLContext(_qglContext), "makeCurrentContext");
        gl::initModuleGl();
        return true;
    }
#endif
    bool result = _qglContext->makeCurrent(_window);
    gl::initModuleGl();
    return result;
}

void Context::swapBuffers() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    // no window (its re-creation under way, replaceWindow): nothing to swap into
    if (!_window) {
        return;
    }
#endif
    qglContext()->swapBuffers(_window);
}

void Context::doneCurrent() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    if (!onMainThread()) {
        sendMessage((void*)objc_getClass("NSOpenGLContext"), "clearCurrentContext");
        return;
    }
#endif
    if (_qglContext) {
        _qglContext->doneCurrent();
    }
}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)

void Context::updateForViewChange() {
    // Main thread, with the global lock held (the present thread is between frames, the context current nowhere):
    // the context takes the view's new frame, as Qt's queued update did on Qt 5
    if (!onMainThread()) {
        return;
    }
    sendMessage(nativeGLContext(_qglContext), "update");
}

void Context::replaceWindow(QWindow* window) {
    // Qt's own attach (setDrawable) only runs inside QOpenGLContext::makeCurrent, which the main thread may not
    // call once the present thread owns the context: the drawable is set natively. On macOS a QWindow's winId is
    // its NSView; the view wants the display's full resolution, as Qt's attach asks (a retina display otherwise
    // draws at half size).
    if (!onMainThread()) {
        return;
    }
    _window = window;
    void* context = nativeGLContext(_qglContext);
    if (window) {
        void* view = (void*)window->winId();
        sendMessage(view, "setWantsBestResolutionOpenGLSurface:", true);
        sendMessage(context, "setView:", view);
        sendMessage(context, "update");
    } else {
        sendMessage(context, "clearDrawable");
    }
    updateSwapchainMemoryCounter();
}
#endif
#endif

Q_GUI_EXPORT QOpenGLContext *qt_gl_global_share_context();
const QSurfaceFormat& getDefaultOpenGLSurfaceFormat();

void Context::qtCreate(QOpenGLContext* shareContext) {
    _qglContext = new QOpenGLContext();
    _qglContext->setFormat(_window->format());
    if (!shareContext) {
        shareContext = qt_gl_global_share_context();
    }

    if (shareContext) {
        _qglContext->setShareContext(shareContext);
    }
    _qglContext->create();
    _swapchainPixelSize = evalGLFormatSwapchainPixelSize(_qglContext->format());
}
