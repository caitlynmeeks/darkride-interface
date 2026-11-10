//
//  Created by Bradley Austin Davis on 2015-04-04
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#pragma once
#ifndef hifi_OffscreenQmlSurface_h
#define hifi_OffscreenQmlSurface_h

#include <qml/OffscreenSurface.h>
#include <QtGui/qevent.h>

#include <QTouchEvent>

// the device pointer events are delivered as: Qt 5's touch device, Qt 6's pointing device
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QPointingDevice;
using OffscreenTouchDevice = QPointingDevice;
#else
class QTouchDevice;
using OffscreenTouchDevice = QTouchDevice;
#endif
#include "PointerEvent.h"

using QmlContextCallback = std::function<void(QQmlContext*)>;

class OffscreenQmlSurface : public hifi::qml::OffscreenSurface {
    using Parent = hifi::qml::OffscreenSurface;
    Q_OBJECT
    Q_PROPERTY(bool focusText READ isFocusText NOTIFY focusTextChanged)
public:
    ~OffscreenQmlSurface();

    static void addWhitelistContextHandler(const std::initializer_list<QUrl>& urls, const QmlContextCallback& callback);
    static void addWhitelistContextHandler(const QUrl& url, const QmlContextCallback& callback) { addWhitelistContextHandler({ { url } }, callback); };
    static void applyWhiteList(const QUrl& url,QQmlContext* context);
    
    bool isFocusText() const { return _focusText; }
    bool getCleaned() { return _isCleaned; }

    bool eventFilter(QObject* originalDestination, QEvent* event) override;
    void setKeyboardRaised(QObject* object, bool raised, bool numeric = false, bool passwordField = false);
    Q_INVOKABLE void synthesizeKeyPress(QString key, QObject* targetOverride = nullptr);
    Q_INVOKABLE void lowerKeyboard();
    PointerEvent::EventType choosePointerEventType(QEvent::Type type);
    Q_INVOKABLE unsigned int deviceIdByTouchPoint(qreal x, qreal y);
   
    static bool viewportFocused;

signals:
    void focusObjectChanged(QObject* newFocus);
    void focusTextChanged(bool focusText);
    void audioOutputDeviceChanged(const QString& deviceName);

    // web event bridge
    void webEventReceived(const QVariant& message);
    // script event bridge
    void scriptEventReceived(const QVariant& message);
    // qml event bridge
    void fromQml(const QVariant& message);

public slots:
    void focusDestroyed(QObject *obj);
    // script event bridge
    void emitScriptEvent(const QVariant& scriptMessage);
    // web event bridge
    void emitWebEvent(const QVariant& webMessage);
    // qml event bridge
    void sendToQml(const QVariant& message);

public:
    /// The app's own tablet surface: set by the app where it makes one, before the surface loads. Only such a surface
    /// becomes the system tablet's root.
    void setAppTablet(bool appTablet) { _appTablet = appTablet; }
    bool isAppTablet() const { return _appTablet; }

protected:
    void loadFromQml(const QUrl& qmlSource, QQuickItem* parent, const QJSValue& callback) override;
    void clearFocusItem();
    void setFocusText(bool newFocusText);
    void initializeEngine(QQmlEngine* engine) override;
    void onRootContextCreated(QQmlContext* qmlContext) override;

private:
    void onRootCreated() override;
    void onItemCreated(QQmlContext* qmlContext, QQuickItem* newItem) override;
    QQmlContext* contextForUrl(const QUrl& url, QQuickItem* parent, bool forceNewContext = false) override;
    QPointF mapWindowToUi(const QPointF& sourcePosition, QObject* sourceObject);

private slots:
    void onFocusObjectChanged(QObject* newFocus) override;

public slots:
    void hoverBeginEvent(const PointerEvent& event, OffscreenTouchDevice& device);
    void hoverEndEvent(const PointerEvent& event, OffscreenTouchDevice& device);
    bool handlePointerEvent(const PointerEvent& event, OffscreenTouchDevice& device, bool release = false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // the device this surface's own mouse events are sent as (see its definition)
    QPointingDevice* mouseDevice();
#endif
    void changeAudioOutputDevice(const QString& deviceName, bool isHtmlUpdate = false);
    void forceHtmlAudioOutputDeviceUpdate();
    void forceQmlAudioOutputDeviceUpdate();
    void updateCursor(const QString& cursor);

private:
    bool _focusText { false };
    bool _isCleaned{ false };
    QQuickItem* _currentFocusItem { nullptr };

    struct TouchState {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QEventPoint touchPoint;
#else
        QTouchEvent::TouchPoint touchPoint;
#endif
        bool hovering { false };
        bool pressed { false };
    };

    bool _pressed;
    bool _touchBeginAccepted { false };
    std::map<uint32_t, TouchState> _activeTouchPoints;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPointingDevice* _mouseDevice { nullptr };
#endif
    bool _appTablet { false };

    QString _currentAudioOutputDevice;
    QTimer _audioOutputUpdateTimer;
};

#endif
