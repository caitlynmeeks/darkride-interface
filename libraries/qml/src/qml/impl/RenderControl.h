//
//  Created by Bradley Austin Davis on 2018-01-04
//  Copyright 2013-2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#pragma once

#include <QtQuick/QQuickRenderControl>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtCore/QPointer>
#include <QtGui/QWindow>   // a QPointer needs the whole class
#endif

namespace hifi { namespace qml { namespace impl {

class RenderControl : public QQuickRenderControl {
public:
    RenderControl(QObject* parent = Q_NULLPTR);
    void setRenderWindow(QWindow* renderWindow);

protected:
    QWindow* renderWindow(QPoint* offset) override;

private:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // a QPointer: Qt 6 re-creates the main window's native window when a QQuickWidget dock joins it, and this is that
    // window (the proxy window), so it nulls itself when the window dies and the surface's next sync falls back to
    // Qt's own, instead of reading a dead window on the render thread (run 10, 11-05); the app sets it again after
    QPointer<QWindow> _renderWindow;
#else
    QWindow* _renderWindow{ nullptr };
#endif
};

}}}  // namespace hifi::qml::impl
