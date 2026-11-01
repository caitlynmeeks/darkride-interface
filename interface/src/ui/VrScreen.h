//
//  VrScreen.h
//  interface/src/ui
//
//  The VR screen's headset side (team/editor-ui/VR-SCREEN-PLAN.md): the menu button brings it up in front of you or hides
//  it, its grab bar moves it, and it stays where you put it in your play space. Its pieces are local Web entities under
//  the avatar's sensor-to-world joint, made only while it's shown: anything under the avatar is refreshed every frame, so
//  hiding deletes them. Their QML surfaces are made ahead, at headset entry, and kept paused while the screen is hidden
//  (a surface pool): making one and its first draw block the main thread. Panels moves the pages in and out of the slots.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#pragma once

#include <array>

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QSharedPointer>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <DependencyManager.h>
#include <PointerEvent.h>
#include <EntityItemID.h>

#include "VrScreenLayout.h"

class OffscreenQmlSurface;
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
    /// Up and in front of you wherever it was (a dialog that needs you). dialogOnly (a world's dialog): if it was hidden,
    /// only the middle slot comes up, never the side panels, until the visitor opens the screen. Main thread.
    void showInFront(bool dialogOnly = false);
    /// a dialog-only screen's dialog has been answered: it hides again
    void dialogOnlyDone();
    void hide();
    bool isShown() const { return _shown; }

    /// one of the slots' surfaces, by the window object itself (Panels refuses a host in any other)
    bool ownsWindow(QQuickWindow* window) const;

    // The pieces' QML surfaces, for the app's acquire and release operators (ApplicationStartup.cpp). Making a surface
    // and its first draw block the main thread about 5 + 20-35 ms each, 130-190 ms for a show (results/20261101-1511-joint,
    // 20261101-1528-screen2). So they're made at headset entry, a step at a time, and kept paused while hidden. Main thread.

    /// one of the pieces' QML addresses
    static bool isPieceUrl(const QString& url);
    /// a piece's surface: the warm one for its address, or a new one if none is warm yet
    QSharedPointer<OffscreenQmlSurface> takeSurface(const QString& url);
    /// one of the pieces' surfaces, let go by its Web entity: true if it's kept (paused) for the next show; false if the
    /// caller is to delete it as any other (out of the headset, or not one of ours)
    bool keepSurface(const QSharedPointer<OffscreenQmlSurface>& surface);

private:
    enum Piece { Left = 0, Middle, Right, Bar, PIECE_COUNT };

    // what a piece's Web entity is made with
    struct PieceSpec {
        QString url;
        QString name;
        glm::vec2 metres { 0.0f };
        float dpi { 0.0f };
        int fps { 0 };
    };
    PieceSpec pieceSpec(Piece piece, const vrscreen::Arrangement& places) const;
    static QSize surfaceSize(const PieceSpec& spec);
    /// warm: made ahead, while hidden (its slot leaves telling Panels it's ready to VrScreen, at the show)
    QSharedPointer<OffscreenQmlSurface> makeSurface(const QString& url, bool warm);
    void startWarming();
    void warmLater();
    bool warmMustWait() const;
    void warmStep();
    void slotFilled(const QString& slot);
    void startSettling();
    void takeWarmPictures();
    void stopKeeping();

    void connectOnce();
    void showPieces(bool dialogOnly);
    void makeOtherPieces(const vrscreen::Arrangement& places);
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
    bool _dialogOnly { false };   // only the middle slot is up, for a world's dialog
    bool _changing { false };   // our own deletes, as against someone else's
    bool _wasHeadset { false };
    uint64_t _lastToggle { 0 };
    std::array<QUuid, PIECE_COUNT> _pieces;
    std::array<QMetaObject::Connection, 3> _entityConnections;

    // the screen's place, in the play space: the head it's arranged around and its turn around that head
    glm::vec3 _head { 0.0f };
    float _yaw { 0.0f };
    float _pitch { vrscreen::DEFAULT_PITCH_DEGREES };

    // the surface pool: every surface of the screen's (handed out or kept), and the kept ones by address
    bool _keeping { false };   // in the headset, or a Desktop test
    QHash<OffscreenQmlSurface*, QString> _surfaceUrls;
    QHash<QString, QSharedPointer<OffscreenQmlSurface>> _kept;
    // the warm-up: back to back at headset entry, staggered later (one step per timer shot, each blocking the main
    // thread once, never on a ride)
    QTimer _warmTimer;
    QList<Piece> _toWarm;
    bool _warmBackToBack { false };
    bool _warmPages { false };      // editor-ui's pages still to make ahead
    bool _warmSettling { false };
    int _warmSettleTicks { 0 };     // the settle step's ticks to go (WARM_SETTLE_TICKS)
    bool _warmAnnounce { false };   // the kept slots still to be announced to Panels (filled with their pages)
    QSharedPointer<OffscreenQmlSurface> _warmLoaded;    // staggered: made; no draw yet while its render thread starts up
    QSharedPointer<OffscreenQmlSurface> _warmDrawing;   // staggered: drawing its first frame; paused at the next step
    QList<QSharedPointer<OffscreenQmlSurface>> _warmUnpaused;   // back to back: drawing; paused at the settle step
    int _warmFps { 0 };

    // a drag: only while the bar is held does anything here run per frame
    QTimer _dragTimer;
    bool _dragging { false };
    bool _dragRightHand { true };
    float _dragStartHandYaw { 0.0f };
    float _dragStartHandPitch { 0.0f };
    float _dragStartYaw { 0.0f };
    float _dragStartPitch { 0.0f };
};
