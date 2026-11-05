//
//  OffscreenSurfaceTests.cpp
//  tests/qml/src
//
//  No window opens and nothing is drawn: a surface's QML window never has a native window, in the app or here, and the
//  events below reach it as the 3D view's reach it in the app.
//
//  Created by scripting on 2026-11-05.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "OffscreenSurfaceTests.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <qml/OffscreenSurface.h>

namespace {

const QSize SURFACE_SIZE { 200, 200 };

// a surface as the app has it, filtering the 3D view's events
class ViewSurface : public hifi::qml::OffscreenSurface {
public:
    ViewSurface() { getWindow()->contentItem()->setSize(SURFACE_SIZE); }

    void putTheCursorAt(const QPointF& position) { _position = position; }

    // true: the move stopped in the QML, and neither the view nor the app got it
    bool moveOverTheView(Qt::MouseButtons held) {
        _position += QPointF(3.0, 2.0);
        QMouseEvent move(QEvent::MouseMove, _position, _position, Qt::NoButton, held, Qt::NoModifier);
        return eventFilter(&_view, &move);
    }

    // a press or a release, handed to the QML as OffscreenUi's filter hands it; true: the QML took it
    bool sendToTheQml(QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, _position, _position, button, held, Qt::NoModifier);
        event.ignore();
        QCoreApplication::sendEvent(getWindow(), &event);
        return event.isAccepted();
    }

private:
    QObject _view;
    QPointF _position { 50.0, 50.0 };
};

// a hover area over the whole surface, like the desktop's: it watches the mouse and takes nothing
class HoverArea : public QQuickItem {
public:
    explicit HoverArea(QQuickWindow* window) : QQuickItem(window->contentItem()) {
        setSize(SURFACE_SIZE);
        setAcceptHoverEvents(true);
    }

    int hovers { 0 };

protected:
    void hoverEnterEvent(QHoverEvent* event) override {
        ++hovers;
        QQuickItem::hoverEnterEvent(event);
    }
    void hoverMoveEvent(QHoverEvent* event) override {
        ++hovers;
        QQuickItem::hoverMoveEvent(event);
    }
};

// what a panel's title bar or a slider does: it takes the press, and so holds the mouse until the release
class DragArea : public HoverArea {
public:
    explicit DragArea(QQuickWindow* window) : HoverArea(window) { setAcceptedMouseButtons(Qt::LeftButton); }

    int moves { 0 };

protected:
    void mousePressEvent(QMouseEvent* event) override { event->accept(); }
    void mouseMoveEvent(QMouseEvent* event) override {
        ++moves;
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override { event->accept(); }
};

} // namespace

// A surface's shared half goes with deleteLater, and no event loop runs here: this takes it down, so that is tested too.
void OffscreenSurfaceTests::cleanup() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// The hover counts are the control here and below: the moves did reach the QML, and it let them go on.
void OffscreenSurfaceTests::testAHoverLeavesTheMoveToTheApp() {
    ViewSurface surface;
    HoverArea area(surface.getWindow());
    QVERIFY(!surface.moveOverTheView(Qt::NoButton));   // entering
    QVERIFY(!surface.moveOverTheView(Qt::NoButton));   // moving on
    QCOMPARE(area.hovers, 2);
}

// Mouse look holds the right button on the 3D view and Alt orbit the left, and nothing in the QML took the press.
void OffscreenSurfaceTests::testADragOnTheViewLeavesTheMoveToTheApp() {
    for (Qt::MouseButton button : { Qt::RightButton, Qt::LeftButton }) {
        ViewSurface surface;
        HoverArea area(surface.getWindow());
        QVERIFY(!surface.sendToTheQml(QEvent::MouseButtonPress, button, button));
        QVERIFY(!surface.moveOverTheView(button));
        QVERIFY(!surface.moveOverTheView(button));
        QVERIFY(!surface.sendToTheQml(QEvent::MouseButtonRelease, button, Qt::NoButton));
        QCOMPARE(area.hovers, 2);
    }
}

// A drag begun on a panel stays the panel's until the button is released; then moves are the app's again.
void OffscreenSurfaceTests::testAnItemHoldingTheMouseKeepsTheMove() {
    ViewSurface surface;
    DragArea area(surface.getWindow());
    QVERIFY(surface.sendToTheQml(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton));
    QVERIFY(surface.moveOverTheView(Qt::LeftButton));
    QCOMPARE(area.moves, 1);
    QVERIFY(surface.sendToTheQml(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton));
    QVERIFY(!surface.moveOverTheView(Qt::NoButton));
    QCOMPARE(area.moves, 1);
}

// A release can go missing, the press's item then holding the mouse with no button down. The next click's release ends
// that, wherever it lands: Qt Quick lets no hold outlast a release of the last button.
void OffscreenSurfaceTests::testTheNextClickEndsAHoldWhoseReleaseWentMissing() {
    ViewSurface surface;
    DragArea panel(surface.getWindow());
    panel.setSize(QSizeF(100.0, 100.0));
    QVERIFY(surface.sendToTheQml(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton));
    QVERIFY(surface.moveOverTheView(Qt::NoButton));
    surface.putTheCursorAt(QPointF(150.0, 150.0));   // on the view, away from the panel
    surface.sendToTheQml(QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton);
    surface.sendToTheQml(QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton);
    QVERIFY(!surface.moveOverTheView(Qt::NoButton));
}

// Qt's platform without a screen: a test on the shared Mac opens nothing and takes no focus.
int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    OffscreenSurfaceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
