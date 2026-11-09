//
//  Created by Bradley Austin Davis on 2018-01-04
//  Copyright 2013-2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderEventHandler.h"


#include <gl/Config.h>
#include <gl/QOpenGLContextWrapper.h>
#include <gl/GLHelpers.h>

#include <QtCore/QString>
#include <QtQuick/QQuickWindow>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/QOffscreenSurface>   // the canvas's surface, handed over as a QSurface
#include <QtQuick/QQuickOpenGLUtils>
#include <QtQuick/QQuickRenderControl>
#include <mutex>
#endif

#include <shared/NsightHelpers.h>
#include "Profiling.h"
#include "SharedObject.h"
#include "TextureCache.h"
#include "RenderControl.h"
#include "../Logging.h"

using namespace hifi::qml::impl;

bool RenderEventHandler::event(QEvent* e) {
    switch (static_cast<OffscreenEvent::Type>(e->type())) {
        case OffscreenEvent::Render:
            onRender();
            return true;

        case OffscreenEvent::RenderSync:
            onRenderSync();
            return true;

        case OffscreenEvent::Initialize:
            onInitalize();
            return true;

        case OffscreenEvent::Quit:
            onQuit();
            return true;

        default:
            break;
    }
    return QObject::event(e);
}

RenderEventHandler::RenderEventHandler(SharedObject* shared, QThread* targetThread) :
        _shared(shared) {
    // Create the GL canvas in the same thread as the share canvas
    if (!_canvas.create(SharedObject::getSharedContext())) {
        qFatal("Unable to create new offscreen GL context");
    }

    _canvas.moveToThreadWithContext(targetThread);
    moveToThread(targetThread);
}

void RenderEventHandler::onInitalize() {
    if (_shared->isQuit()) {
        return;
    }

    _canvas.setThreadContext();
    if (!_canvas.makeCurrent()) {
        qFatal("Unable to make QML rendering context current on render thread");
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    _shared->initializeRenderControl(_canvas.getContext(), _canvas.getOffscreenSurface());
#else
    _shared->initializeRenderControl(_canvas.getContext());
#endif
    _initialized = true;
}

void RenderEventHandler::resize() {
    PROFILE_RANGE(render_qml_gl, __FUNCTION__);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    auto targetSize = _shared->getPixelSize();   // in pixels (SharedObject::getPixelSize): a ratio change resizes too
#else
    auto targetSize = _shared->getSize();
#endif
    if (_currentSize != targetSize) {
        auto& offscreenTextures = SharedObject::getTextureCache();
        // Release hold on the textures of the old size
        if (_currentSize != QSize()) {
            _shared->releaseTextureAndFence();
            offscreenTextures.releaseSize(_currentSize);
        }

        _currentSize = targetSize;

        // Acquire the new texture size
        if (_currentSize != QSize()) {
            qCDebug(qmlLogging) << "Upating offscreen textures to " << _currentSize.width() << " x " << _currentSize.height();
            offscreenTextures.acquireSize(_currentSize);
            if (_depthStencil) {
                glDeleteRenderbuffers(1, &_depthStencil);
                _depthStencil = 0;
            }
            glGenRenderbuffers(1, &_depthStencil);
            glBindRenderbuffer(GL_RENDERBUFFER, _depthStencil);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, _currentSize.width(), _currentSize.height());
            if (!_fbo) {
                glGenFramebuffers(1, &_fbo);
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);
            glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthStencil);
            glViewport(0, 0, _currentSize.width(), _currentSize.height());
            glScissor(0, 0, _currentSize.width(), _currentSize.height());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (fixedTargetShape()) {
                if (_fixedTarget) {
                    glDeleteTextures(1, &_fixedTarget);
                }
                glGenTextures(1, &_fixedTarget);
                glBindTexture(GL_TEXTURE_2D, _fixedTarget);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, _currentSize.width(), _currentSize.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (!_blitReadFbo) {
                    glGenFramebuffers(1, &_blitReadFbo);
                    glGenFramebuffers(1, &_blitDrawFbo);
                }
                // Qt's target, set once per size: it builds its depth-stencil round this texture
                _shared->setRenderTarget(_fbo, _fixedTarget, _currentSize);
            }
#endif
        }
    }
}

void RenderEventHandler::onRender() {
    qmlRender(false);
}

void RenderEventHandler::onRenderSync() {
    qmlRender(true);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// Qt 6: the scene graph draws through QRhi (forced to OpenGL); the render target is Qt's own, built round this frame's
// cache texture, and the sync and the render happen inside beginFrame/endFrame on this thread
// (team/rendering/QT6-STAGE1-RENDERING.md, R2). SharedObject::preRender keeps the paused check and the GUI thread's
// sync hand-off, as on Qt 5.
bool RenderEventHandler::fixedTargetShape() {
    static const bool fixed = qEnvironmentVariable("TIVOLI_QML_TARGET") == QStringLiteral("fixed");
    return fixed;
}

void RenderEventHandler::qmlRender(bool sceneGraphSync) {
    if (_shared->isQuit()) {
        return;
    }

    if (_canvas.getContext() != QOpenGLContextWrapper::currentContext()) {
        qFatal("QML rendering context not current on render thread");
    }

    PROFILE_RANGE(render_qml_gl, __FUNCTION__);

    gl::globalLock();
    resize();
    if (_currentSize == QSize()) {
        // nothing to draw into yet; the GUI thread waiting on a sync is let go (the next frame syncs)
        if (sceneGraphSync) {
            _shared->wake();
        }
        gl::globalRelease();
        return;
    }

    GLuint texture = SharedObject::getTextureCache().acquireTexture(_currentSize);
    if (!fixedTargetShape()) {
        _shared->setRenderTarget(_fbo, texture, _currentSize);   // (A): Qt's target round this frame's cache texture
    }
    _shared->_renderControl->beginFrame();
    bool rendered = _shared->preRender(sceneGraphSync);
    if (rendered) {
        // Serialize QML rendering because of a crash caused by Qt bug https://bugreports.qt.io/browse/QTBUG-77469
        static std::mutex qmlRenderMutex;
        std::unique_lock<std::mutex> qmlRenderLock{ qmlRenderMutex };
        _shared->_renderControl->render();
    }
    _shared->_renderControl->endFrame();
    if (!rendered) {
        SharedObject::getTextureCache().releaseTexture({ texture, nullptr });
        gl::globalRelease();
        return;
    }
    _shared->_lastRenderTime = usecTimestampNow();
    if (fixedTargetShape()) {
        // (B): one copy of the fixed target into this frame's cache texture (GL 4.1 has no glCopyImageSubData)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _blitReadFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _fixedTarget, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _blitDrawFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        glBlitFramebuffer(0, 0, _currentSize.width(), _currentSize.height(), 0, 0, _currentSize.width(), _currentSize.height(),
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // Fence will be used in another thread / context, so a flush is required
    glFlush();
    _shared->updateTextureAndFence({ texture, fence });
    QQuickOpenGLUtils::resetOpenGLState();
    gl::globalRelease();
}
#else
void RenderEventHandler::qmlRender(bool sceneGraphSync) {
    if (_shared->isQuit()) {
        return;
    }

    if (_canvas.getContext() != QOpenGLContextWrapper::currentContext()) {
        qFatal("QML rendering context not current on render thread");
    }

    PROFILE_RANGE(render_qml_gl, __FUNCTION__);

    gl::globalLock();
    if (!_shared->preRender(sceneGraphSync)) {
        gl::globalRelease();
        return;
    }

    resize();

    if (_currentSize != QSize()) {
        PROFILE_RANGE(render_qml_gl, "render");
        GLuint texture = SharedObject::getTextureCache().acquireTexture(_currentSize);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);
        if (nsightActive()) {
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            _shared->setRenderTarget(_fbo, _currentSize);

            // workaround for https://highfidelity.atlassian.net/browse/BUGZ-1119
            {
                // Serialize QML rendering because of a crash caused by Qt bug 
                // https://bugreports.qt.io/browse/QTBUG-77469
                static std::mutex qmlRenderMutex;
                std::unique_lock<std::mutex> qmlRenderLock{ qmlRenderMutex };
                _shared->_renderControl->render();
            }
        }
        _shared->_lastRenderTime = usecTimestampNow();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // Fence will be used in another thread / context, so a flush is required
        glFlush();
        _shared->updateTextureAndFence({ texture, fence });
        _shared->_quickWindow->resetOpenGLState();
    }
    gl::globalRelease();
}
#endif

void RenderEventHandler::onQuit() {
    if (_initialized) {
        if (_canvas.getContext() != QOpenGLContextWrapper::currentContext()) {
            qFatal("QML rendering context not current on render thread");
        }

        if (_depthStencil) {
            glDeleteRenderbuffers(1, &_depthStencil);
            _depthStencil = 0;
        }

        if (_fbo) {
            glDeleteFramebuffers(1, &_fbo);
            _fbo = 0;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (_fixedTarget) {
            glDeleteTextures(1, &_fixedTarget);
            _fixedTarget = 0;
        }
        if (_blitReadFbo) {
            glDeleteFramebuffers(1, &_blitReadFbo);
            glDeleteFramebuffers(1, &_blitDrawFbo);
            _blitReadFbo = _blitDrawFbo = 0;
        }
#endif

        _shared->shutdownRendering(_currentSize);
        _canvas.doneCurrent();
    }
    _canvas.moveToThreadWithContext(qApp->thread());
    moveToThread(qApp->thread());
    QThread::currentThread()->quit();
}

