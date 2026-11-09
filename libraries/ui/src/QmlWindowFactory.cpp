//
//  QmlWindowFactory.cpp
//  libraries/ui/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "QmlWindowFactory.h"

#include "QmlWebWindowClass.h"
#include "QmlWindowClass.h"

QObject* QmlWindowFactory::create(const QVariantMap& properties) {
    QmlWindowClass* window = _kind == WebWindow ? QmlWebWindowClass::createWebWindow(properties, _restricted)
                                                : QmlWindowClass::createWindow(properties, _restricted);
    if (window) {
        // the window lives on the main thread; it goes when the script's engine goes, as before
        connect(this, &QObject::destroyed, window, &QObject::deleteLater);
    }
    return window;
}

void QmlWindowFactory::dispose(QObject* window) {
    if (window && qobject_cast<QmlWindowClass*>(window)) {
        window->deleteLater();
    }
}
