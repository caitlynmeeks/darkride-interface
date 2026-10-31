//
//  ViewState.h
//  interface/src
//
//  The camera and what it sees. The main camera (a FancyCamera, the scripts' and QML's Camera object); the view
//  frustum, what the frame computes from the camera for culling, picking and the servers' queries; the display view
//  frustum, the current rendering pass's, which a shadow or mirror pass changes; the conical views the servers are
//  asked for (the main view and the secondary camera's); and the recursive lock they are kept under.
//  updateCamera() places the camera for its mode once a frame (from the avatar, the mirror, the entity it follows,
//  the headset); update() loads the view frustum from the camera and rebuilds the conical views before the
//  queries; the mode members keep the camera and the View menu in step (cycleCamera, cameraModeChanged,
//  cameraMenuChanged, changeViewAsNeeded, rotationModeChanged; setModeKeepingPrevious and restorePreviousMode for
//  the sign-in gate); computePickRay() turns a screen point into a ray. Application owns one, reached by
//  getViewState(); its getCamera(), its view-state overrides for the renderer (copyCurrentViewFrustum,
//  computePickRay, getConicalViews) and its two View menu slots forward here. These were Application's _myCamera,
//  _viewFrustum, _displayViewFrustum, _viewMutex, _conicalViews, _previousCameraMode, the mirror and headset-boom
//  state, the members of ApplicationCamera.cpp and the frame loop's updateCamera, view-frustum step and
//  updateSecondaryCameraViewFrustum (ARCHITECTURE.md §5.4 step 4, the fourth owner class). "View state" is the
//  renderer's own name for this (AbstractViewStateInterface); the workload's View (workload/View.h), the regions
//  around a position, is derived from this view frustum each frame.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_ViewState_h
#define hifi_ViewState_h

#include <atomic>

#include <QtCore/QMutex>
#include <QtCore/QUuid>
#include <QtCore/QVector>

#include <glm/glm.hpp>

#include <render/Forward.h>
#include <shared/ConicalViewFrustum.h>
#include <ViewFrustum.h>

#include "FancyCamera.h"

class ViewState {
public:
    FancyCamera& camera() { return _camera; }
    const FancyCamera& camera() const { return _camera; }

    // The frustums and the conical views are kept under one recursive lock, mutex(). The copies, the pick ray and the
    // writes below take it themselves; a caller that reads viewFrustum(), displayViewFrustum() or conicalViews()
    // over several statements holds it for that scope, as the frame loop's query decision and render arguments do.
    QMutex& mutex() const { return _mutex; }
    const ViewFrustum& viewFrustum() const { return _viewFrustum; }                 // the avatar's current view
    const ViewFrustum& displayViewFrustum() const { return _displayViewFrustum; }   // the current rendering pass's view
    const ConicalViewFrustums& conicalViews() const { return _conicalViews; }       // the views the servers are asked for
    void copyViewFrustum(ViewFrustum& viewOut) const;
    void copyDisplayViewFrustum(ViewFrustum& viewOut) const;
    PickRay computePickRay(float x, float y) const;   // from a screen point: the headset's through the compositor; x flipped while mirrored()

    void loadViewFrustum();                            // the view frustum from the camera, after the camera's projection changed
    void setProjection(const glm::mat4& projection);   // the view frustum's projection for this frame, its planes recalculated
    void loadDisplayViewFrustum();                     // the display view frustum from the camera, for the render arguments

    void update();   // once a frame, before the queries: the view frustum from the camera, the impostor load view, the conical views rebuilt
    void updateCamera(RenderArgs& renderArgs, float deltaTime);   // the camera placed for its mode; the render mode is the mirror's when the camera is
    // the picture is shown flipped left to right (RenderArgs::_mirrored): the selfie view on a flat screen, and the
    // full-screen mirror camera; set by updateCamera each frame, read by computePickRay from any thread
    bool mirrored() const { return _mirrored; }

    // the camera's mode and the View menu, kept in step
    void cycleCamera();                             // First Person, Look At, Selfie (when shown), First Person again
    void cameraModeChanged();                       // the camera's mode changed (a script, the sign-in gate): the menu follows
    void cameraMenuChanged();                       // a View menu item checked: the camera follows, the boom length with it
    void changeViewAsNeeded(float boomLength);      // first person at the minimum boom length, look-at beyond it
    void rotationModeChanged() const;               // Center Player In View unchecked: the head pitch reset
    void setModeKeepingPrevious(CameraMode mode);   // the sign-in gate's: this mode now, the one before kept
    void restorePreviousMode();                     // the kept mode back

    // camera collision's test (View > Camera Collision), the third-person camera's and Camera.collideBoom's: how far
    // from the pivot toward where the camera wants to be it can stand without going into what would stop you, seeing
    // through the ignored entities and their children; the whole length when nothing is in the way or the option is
    // off. On the main thread (the physics' queries). clearPivot, when given, is set to the pivot moved out of what the
    // probe overlaps there, and the length is then measured from it. holder, when given, is set to the entity that
    // stopped the probe (the frame trace's boom.holder), null when nothing did.
    float allowedBoomLength(const glm::vec3& pivot, const glm::vec3& wanted, const QVector<QUuid>& ignore,
                            glm::vec3* clearPivot = nullptr, QUuid* holder = nullptr) const;

private:
    void addSecondaryCameraView();   // the secondary camera's frustum among the conical views (was updateSecondaryCameraViewFrustum)
    // camera collision for the third-person boom (View > Camera Collision): from the pivot toward where the camera
    // wants to be, the camera stops short of what would stop you; it moves in at once and eases back out
    glm::vec3 collideCameraBoom(const glm::vec3& pivot, const glm::vec3& wanted, float deltaTime);
    // the chase camera (Camera.setChaseTarget) in independent mode, worked out this frame; false when there is none
    bool updateChaseCamera(float deltaTime);

    FancyCamera _camera;   // my view onto the world
    mutable QMutex _mutex{ QMutex::Recursive };
    ViewFrustum _viewFrustum;   // current state of view frustum, perspective, orientation, etc.
    ViewFrustum _displayViewFrustum;
    ConicalViewFrustums _conicalViews;
    CameraMode _previousMode{ CAMERA_MODE_LOOK_AT };   // the mode before setModeKeepingPrevious()
    std::atomic<bool> _mirrored{ false };               // the picture flipped left to right this frame (mirrored())

    // the mirror's camera
    float _scaleMirror{ 1.0f };
    float _mirrorYawOffset{ 0.0f };
    float _raiseMirror{ 0.0f };
    // the third-person camera in a headset: the boom from the head, kept while the mode lasts
    glm::vec3 _thirdPersonHMDCameraBoom{ 0.0f, 0.0f, -1.0f };
    bool _thirdPersonHMDCameraBoomValid{ true };
    // camera collision: the boom's length now (-1 when no third-person boom was placed last frame), and whether it is
    // short of the wanted length because something held it in (it then eases back out)
    float _boomLength{ -1.0f };
    bool _boomHeldIn{ false };
    // the chase camera: where it has settled (camera collision aside), its boom's length (-1 to start at the wanted
    // length) and whether a wall holds it in, the heading kept for an entity that points straight up or down, the
    // settings' version it follows and whether it ran last frame (if not, it starts where it wants to be)
    glm::vec3 _chasePosition{ 0.0f };
    glm::vec3 _chaseFlat{ 0.0f, 0.0f, -1.0f };
    float _chaseBoom{ -1.0f };
    bool _chaseHeldIn{ false };
    uint32_t _chaseVersion{ 0 };
    bool _chaseValid{ false };
};

#endif // hifi_ViewState_h
