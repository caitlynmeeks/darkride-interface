//
//  VrScreen.h
//  interface/src/ui
//
//  The VR screen's headset side (team/editor-ui/VR-SCREEN-PLAN.md): the menu button brings it up in front of you or hides
//  it, its grab bar moves it, and it stays where you put it in your play space. Its pieces are local Web entities under
//  the avatar's sensor-to-world joint, made only while it's shown: anything under the avatar is refreshed every frame, so
//  hiding deletes them and a hidden screen costs nothing. Panels moves the pages in and out of the slots.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#pragma once

#include <array>

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <DependencyManager.h>
#include <PointerEvent.h>
#include <EntityItemID.h>

#include "VrScreenLayout.h"

class QQuickWindow;

class VrScreen : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:
    VrScreen();

    /// The screen in place of the tablet: in the headset. DARKRIDE_VR_SCREEN also turns it on in Desktop, for tests.
    static bool isOn();

    /// the menu button: up in front of you, or hidden
    void toggle();
    void show();
    /// up and in front of you wherever it was (a dialog that needs you). Main thread.
    void showInFront();
    void hide();
    bool isShown() const { return _shown; }

    /// one of the slots' surfaces, by the window object itself (Panels refuses a host in any other)
    bool ownsWindow(QQuickWindow* window) const;

private:
    enum Piece { Left = 0, Middle, Right, Bar, PIECE_COUNT };

    void connectOnce();
    vrscreen::Arrangement arrangement() const;
    vrscreen::Pose placedPiece(const vrscreen::Arrangement& arrangement, Piece piece) const;
    void makePiece(Piece piece, const vrscreen::Arrangement& arrangement);
    void deletePiece(Piece piece);
    void movePieces();
    void updateHead();
    void faceHead();
    void slotSizeChanged(const QString& slot);
    void entityDeleted(const EntityItemID& id);
    void displayChanged();

    void mousePressed(const EntityItemID& id, const PointerEvent& event);
    void mouseReleased(const EntityItemID& id, const PointerEvent& event);
    void beginDrag();
    void dragTick();
    void endDrag();
    void tellState();

    bool _connected { false };
    bool _shown { false };
    bool _changing { false };   // our own deletes, as against someone else's
    bool _wasHeadset { false };
    uint64_t _lastToggle { 0 };
    std::array<QUuid, PIECE_COUNT> _pieces;
    std::array<QMetaObject::Connection, 3> _entityConnections;

    // the screen's place, in the play space: the head it's arranged around and its turn around that head
    glm::vec3 _head { 0.0f };
    float _yaw { 0.0f };
    float _pitch { vrscreen::DEFAULT_PITCH_DEGREES };

    // a drag: only while the bar is held does anything here run per frame
    QTimer _dragTimer;
    bool _dragging { false };
    bool _dragRightHand { true };
    float _dragStartHandYaw { 0.0f };
    float _dragStartHandPitch { 0.0f };
    float _dragStartYaw { 0.0f };
    float _dragStartPitch { 0.0f };
};
