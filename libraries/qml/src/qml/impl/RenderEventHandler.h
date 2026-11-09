//
//  Created by Bradley Austin Davis on 2018-01-04
//  Copyright 2013-2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#pragma once


#include <QtCore/QObject>
#include <QtCore/QThread>
#include <QtGui/qevent.h>

#include <GLMHelpers.h>
#include <gl/OffscreenGLCanvas.h>

namespace hifi { namespace qml { namespace impl {

class SharedObject;

class OffscreenEvent : public QEvent {
public:
    enum Type {
        Initialize = QEvent::User + 1,
        Render,
        RenderSync,
        Quit
    };

    OffscreenEvent(Type type) : QEvent(static_cast<QEvent::Type>(type)) {}
};

/* The render event handler lives on the QML rendering thread for a given surface
 * (each surface has a dedicated rendering thread) and handles events of type 
 * OffscreenEvent to do one time initialization or destruction, and to actually 
 * perform the render.  
 */
class RenderEventHandler : public QObject {
public:
    RenderEventHandler(SharedObject* shared, QThread* targetThread);

private:
    bool event(QEvent* e) override;
    void onInitalize();
    void resize();
    void onRender();
    void onRenderSync();
    void qmlRender(bool sceneGraphSync);
    void onQuit();

    SharedObject* const _shared;
    OffscreenGLCanvas _canvas;
    QSize _currentSize;

    uint32_t _fbo{ 0 };
    uint32_t _depthStencil{ 0 };
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Shape (B) of the Qt 6 render target (QT6-STAGE1-RENDERING.md R2, entry 21): one fixed texture per surface size
    // that Qt draws into, blitted into the cache texture each frame; shape (A) gives Qt the cache texture itself.
    // TIVOLI_QML_TARGET=fixed picks (B), read once; the lab's kit times both on one binary, the loser goes.
    static bool fixedTargetShape();
    uint32_t _fixedTarget{ 0 };
    uint32_t _blitReadFbo{ 0 };
    uint32_t _blitDrawFbo{ 0 };
#endif

    bool _initialized { false };
};

}}}  // namespace hifi::qml::impl

