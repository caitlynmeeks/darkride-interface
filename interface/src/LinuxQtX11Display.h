//
//  LinuxQtX11Display.h
//  interface/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE or https://opensource.org/licenses/MIT
//
//  Included first in every C++ file of the app on Linux with Qt 6 (interface/CMakeLists.txt). Qt's
//  QtGui/qguiapplication_platform.h, which <QGuiApplication> brings in, declares X11's display type at global scope
//  (`typedef struct _XDisplay Display;`) when Qt is built with xcb, and that name collides with the app's own class
//  Display (Display.h). Qt's header is read here once with that name changed to QtX11Display (the same type, struct
//  _XDisplay), and its include guard keeps the typedef from coming back. Code that needs X11's display type writes
//  QtX11Display (or struct _XDisplay). Nothing changes on the Mac or Windows.
//

#pragma once

#ifdef __cplusplus
#define Display QtX11Display
#include <QtGui/qguiapplication_platform.h>
#undef Display
#endif
