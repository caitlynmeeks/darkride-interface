//
//  ViewState.cpp
//  interface/src
//
//  The camera and what it sees (see ViewState.h). The code of Application's cycleCamera, cameraModeChanged,
//  changeViewAsNeeded, cameraMenuChanged, rotationModeChanged, computePickRay, copyViewFrustum and
//  copyDisplayViewFrustum, of the frame loop's updateCamera, its view-frustum step and
//  updateSecondaryCameraViewFrustum, and of the sign-in gate's save and restore of the mode, with Application reached
//  through qApp for the headset mode, the display plug-in, the compositor, the canvas, the entities and the graphics
//  engine.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ViewState.h"

#include <glm/gtx/transform.hpp>   // the one-argument translate and scale

#include <QtCore/QSet>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <controllers/UserInputMapper.h>
#include <display-plugins/CompositorHelper.h>
#include <EntityMotionState.h>
#include <EntityScriptingInterface.h>
#include <FrameTrace.h>
#include <ObjectMotionState.h>
#include <PhysicsCollisionGroups.h>
#include <PhysicsEngine.h>
#include <SecondaryCamera.h>
#include <ShapeEntityItem.h>

#include "Application.h"
#include "avatar/AvatarManager.h"
#include "avatar/MyHead.h"
#include "editor/Editor.h"
#include "LODManager.h"

static const float MIRROR_FULLSCREEN_DISTANCE = 0.789f;

// Application's getMyAvatar is private; this is the same call
static std::shared_ptr<MyAvatar> getMyAvatar() {
    return DependencyManager::get<AvatarManager>()->getMyAvatar();
}

// The camera and the ride, into the frame trace (FrameTrace; team/performance/INSTRUMENTS-PLAN.md, item 7) when it is
// on: the camera as this frame shows it (cam.x/y/z_m, cam.qw/qx/qy/qz, cam.mode) and, while the avatar is carried by
// a frame or a vehicle (a monorail car, a kart, a seat's vehicle: the avatar's frame, else the top of its parent
// chain), that entity as this frame draws it (ride.id, ride.x/y/z_m, ride.qw/qx/qy/qz) and the correction it is still
// blending in (ride.correction_m). Written at the end of updateCamera, so the camera and the ride are the frame's own
// pair; the boom's columns (boom.*) are written where the boom is placed. Until 2026-10-31 this was
// TIVOLI_CAMERA_TRACE, a file of the same pair for vehicles only (physics, 10-26: the chase camera's judder at speed);
// the substeps and the leftover time it carried are the physics step's own columns (physics.*).
static void traceCamera(const Camera& camera) {
    if (!FrameTrace::enabled()) {
        return;
    }
    glm::vec3 cameraPosition = camera.getPosition();
    glm::quat cameraRotation = camera.getOrientation();
    FrameTrace::set("cam.x_m", cameraPosition.x);
    FrameTrace::set("cam.y_m", cameraPosition.y);
    FrameTrace::set("cam.z_m", cameraPosition.z);
    FrameTrace::set("cam.qw", cameraRotation.w);
    FrameTrace::set("cam.qx", cameraRotation.x);
    FrameTrace::set("cam.qy", cameraRotation.y);
    FrameTrace::set("cam.qz", cameraRotation.z);
    FrameTrace::set("cam.mode", (double)camera.getMode());
    auto myAvatar = getMyAvatar();
    EntityTreePointer tree = qApp->getEntities()->getTree();
    if (!myAvatar || !tree) {
        return;
    }
    EntityItemPointer ride = tree->findEntityByID(myAvatar->getFrameID());
    if (!ride) {   // parented to something that is not a frame (a seat): what carries the chain is the ride
        QUuid id = myAvatar->getParentID();
        const int MAX_DEPTH = 16;
        for (int depth = 0; !id.isNull() && depth < MAX_DEPTH; depth++) {
            EntityItemPointer entity = tree->findEntityByID(id);
            if (!entity) {
                break;
            }
            ride = entity;
            id = entity->getParentID();
        }
    }
    if (!ride) {
        return;
    }
    glm::vec3 position = ride->getWorldPosition();
    glm::quat rotation = ride->getWorldOrientation();
    FrameTrace::setText("ride.id", ride->getID().toString());
    FrameTrace::set("ride.x_m", position.x);
    FrameTrace::set("ride.y_m", position.y);
    FrameTrace::set("ride.z_m", position.z);
    FrameTrace::set("ride.qw", rotation.w);
    FrameTrace::set("ride.qx", rotation.x);
    FrameTrace::set("ride.qy", rotation.y);
    FrameTrace::set("ride.qz", rotation.z);
    auto motionState = static_cast<EntityMotionState*>(ride->getPhysicsInfo());
    FrameTrace::set("ride.correction_m", motionState ? motionState->getCorrectionDistance() : 0.0f);
}

void ViewState::copyViewFrustum(ViewFrustum& viewOut) const {
    QMutexLocker viewLocker(&_mutex);
    viewOut = _viewFrustum;
}

void ViewState::copyDisplayViewFrustum(ViewFrustum& viewOut) const {
    QMutexLocker viewLocker(&_mutex);
    viewOut = _displayViewFrustum;
}

PickRay ViewState::computePickRay(float x, float y) const {
    vec2 pickPoint { x, y };
    PickRay result;
    if (qApp->isHMDMode()) {
        qApp->getApplicationCompositor().computeHmdPickRay(pickPoint, result.origin, result.direction);
    } else {
        pickPoint /= qApp->getCanvasSize();
        if (_mirrored) {   // the picture is flipped left to right (the selfie view, the mirror camera): so is the point
            pickPoint.x = 1.0f - pickPoint.x;
        }
        QMutexLocker viewLocker(&_mutex);
        _viewFrustum.computePickRay(pickPoint.x, pickPoint.y, result.origin, result.direction);
    }
    return result;
}

void ViewState::loadViewFrustum() {
    QMutexLocker viewLocker(&_mutex);
    _camera.loadViewFrustum(_viewFrustum);
}

void ViewState::setProjection(const glm::mat4& projection) {
    QMutexLocker viewLocker(&_mutex);
    _viewFrustum.setProjection(projection);
    _viewFrustum.calculate();
}

void ViewState::loadDisplayViewFrustum() {
    QMutexLocker viewLocker(&_mutex);
    _camera.loadViewFrustum(_displayViewFrustum);
}

void ViewState::update() {
    // Update _viewFrustum with latest camera and view frustum data...
    // NOTE: we get this from the view frustum, to make it simpler, since the
    // loadViewFrumstum() method will get the correct details from the camera
    // We could optimize this to not actually load the viewFrustum, since we don't
    // actually need to calculate the view frustum planes to send these details
    // to the server.
    QMutexLocker viewLocker(&_mutex);
    _camera.loadViewFrustum(_viewFrustum);
    // where a model shown as its impostor card fetches its mesh (ENTITY-STREAMING.md 3.1)
    qApp->getEntities()->setImpostorLoadView(_viewFrustum.getPosition(),
                                             DependencyManager::get<LODManager>()->getImpostorLoadHalfTanSq());

    _conicalViews.clear();
    _conicalViews.push_back(_viewFrustum);
    // TODO: Fix this by modeling the way the secondary camera works on how the main camera works
    // ie. Use a camera object stored in the game logic and informs the Engine on where the secondary
    // camera should be.
    addSecondaryCameraView();
}

void ViewState::addSecondaryCameraView() {
    // TODO: Fix this by modeling the way the secondary camera works on how the main camera works
    // ie. Use a camera object stored in the game logic and informs the Engine on where the secondary
    // camera should be.

    // Code based on SecondaryCameraJob
    auto renderConfig = qApp->getGraphicsEngine().getRenderEngine()->getConfiguration();
    assert(renderConfig);
    auto camera = dynamic_cast<SecondaryCameraJobConfig*>(renderConfig->getConfig("SecondaryCamera"));

    if (!camera || !camera->isEnabled()) {
        return;
    }

    ViewFrustum secondaryViewFrustum;
    if (camera->portalProjection && !camera->attachedEntityId.isNull() && !camera->portalEntranceEntityId.isNull()) {
        auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
        EntityItemPointer portalEntrance = qApp->getEntities()->getTree()->findEntityByID(camera->portalEntranceEntityId);
        EntityItemPointer portalExit = qApp->getEntities()->getTree()->findEntityByID(camera->attachedEntityId);

        glm::vec3 portalEntrancePropertiesPosition = portalEntrance->getWorldPosition();
        glm::quat portalEntrancePropertiesRotation = portalEntrance->getWorldOrientation();
        glm::mat4 worldFromPortalEntranceRotation = glm::mat4_cast(portalEntrancePropertiesRotation);
        glm::mat4 worldFromPortalEntranceTranslation = glm::translate(portalEntrancePropertiesPosition);
        glm::mat4 worldFromPortalEntrance = worldFromPortalEntranceTranslation * worldFromPortalEntranceRotation;
        glm::mat4 portalEntranceFromWorld = glm::inverse(worldFromPortalEntrance);

        glm::vec3 portalExitPropertiesPosition = portalExit->getWorldPosition();
        glm::quat portalExitPropertiesRotation = portalExit->getWorldOrientation();
        glm::vec3 portalExitPropertiesDimensions = portalExit->getScaledDimensions();
        glm::vec3 halfPortalExitPropertiesDimensions = 0.5f * portalExitPropertiesDimensions;

        glm::mat4 worldFromPortalExitRotation = glm::mat4_cast(portalExitPropertiesRotation);
        glm::mat4 worldFromPortalExitTranslation = glm::translate(portalExitPropertiesPosition);
        glm::mat4 worldFromPortalExit = worldFromPortalExitTranslation * worldFromPortalExitRotation;

        glm::vec3 mainCameraPositionWorld = _camera.getPosition();
        glm::vec3 mainCameraPositionPortalEntrance = vec3(portalEntranceFromWorld * vec4(mainCameraPositionWorld, 1.0f));
        mainCameraPositionPortalEntrance = vec3(-mainCameraPositionPortalEntrance.x, mainCameraPositionPortalEntrance.y,
            -mainCameraPositionPortalEntrance.z);
        glm::vec3 portalExitCameraPositionWorld = vec3(worldFromPortalExit * vec4(mainCameraPositionPortalEntrance, 1.0f));

        secondaryViewFrustum.setPosition(portalExitCameraPositionWorld);
        secondaryViewFrustum.setOrientation(portalExitPropertiesRotation);

        float nearClip = mainCameraPositionPortalEntrance.z + portalExitPropertiesDimensions.z * 2.0f;
        // `mainCameraPositionPortalEntrance` should technically be `mainCameraPositionPortalExit`,
        // but the values are the same.
        glm::vec3 upperRight = halfPortalExitPropertiesDimensions - mainCameraPositionPortalEntrance;
        glm::vec3 bottomLeft = -halfPortalExitPropertiesDimensions - mainCameraPositionPortalEntrance;
        glm::mat4 frustum = glm::frustum(bottomLeft.x, upperRight.x, bottomLeft.y, upperRight.y, nearClip, camera->farClipPlaneDistance);
        secondaryViewFrustum.setProjection(frustum);
    } else if (camera->mirrorProjection && !camera->attachedEntityId.isNull()) {
        auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
        auto entityProperties = entityScriptingInterface->getEntityProperties(camera->attachedEntityId);
        glm::vec3 mirrorPropertiesPosition = entityProperties.getPosition();
        glm::quat mirrorPropertiesRotation = entityProperties.getRotation();
        glm::vec3 mirrorPropertiesDimensions = entityProperties.getDimensions();
        glm::vec3 halfMirrorPropertiesDimensions = 0.5f * mirrorPropertiesDimensions;

        // setup mirror from world as inverse of world from mirror transformation using inverted x and z for mirrored image
        // TODO: we are assuming here that UP is world y-axis
        glm::mat4 worldFromMirrorRotation = glm::mat4_cast(mirrorPropertiesRotation) * glm::scale(vec3(-1.0f, 1.0f, -1.0f));
        glm::mat4 worldFromMirrorTranslation = glm::translate(mirrorPropertiesPosition);
        glm::mat4 worldFromMirror = worldFromMirrorTranslation * worldFromMirrorRotation;
        glm::mat4 mirrorFromWorld = glm::inverse(worldFromMirror);

        // get mirror camera position by reflecting main camera position's z coordinate in mirror space
        glm::vec3 mainCameraPositionWorld = _camera.getPosition();
        glm::vec3 mainCameraPositionMirror = vec3(mirrorFromWorld * vec4(mainCameraPositionWorld, 1.0f));
        glm::vec3 mirrorCameraPositionMirror = vec3(mainCameraPositionMirror.x, mainCameraPositionMirror.y,
                                                    -mainCameraPositionMirror.z);
        glm::vec3 mirrorCameraPositionWorld = vec3(worldFromMirror * vec4(mirrorCameraPositionMirror, 1.0f));

        // set frustum position to be mirrored camera and set orientation to mirror's adjusted rotation
        glm::quat mirrorCameraOrientation = glm::quat_cast(worldFromMirrorRotation);
        secondaryViewFrustum.setPosition(mirrorCameraPositionWorld);
        secondaryViewFrustum.setOrientation(mirrorCameraOrientation);

        // build frustum using mirror space translation of mirrored camera
        float nearClip = mirrorCameraPositionMirror.z + mirrorPropertiesDimensions.z * 2.0f;
        glm::vec3 upperRight = halfMirrorPropertiesDimensions - mirrorCameraPositionMirror;
        glm::vec3 bottomLeft = -halfMirrorPropertiesDimensions - mirrorCameraPositionMirror;
        glm::mat4 frustum = glm::frustum(bottomLeft.x, upperRight.x, bottomLeft.y, upperRight.y, nearClip, camera->farClipPlaneDistance);
        secondaryViewFrustum.setProjection(frustum);
    } else {
        if (!camera->attachedEntityId.isNull()) {
            auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
            auto entityProperties = entityScriptingInterface->getEntityProperties(camera->attachedEntityId);
            secondaryViewFrustum.setPosition(entityProperties.getPosition());
            secondaryViewFrustum.setOrientation(entityProperties.getRotation());
        } else {
            secondaryViewFrustum.setPosition(camera->position);
            secondaryViewFrustum.setOrientation(camera->orientation);
        }

        float aspectRatio = (float)camera->textureWidth / (float)camera->textureHeight;
        secondaryViewFrustum.setProjection(camera->vFoV,
                                            aspectRatio,
                                            camera->nearClipPlaneDistance,
                                            camera->farClipPlaneDistance);
    }
    // Without calculating the bound planes, the secondary camera will use the same culling frustum as the main camera,
    // which is not what we want here.
    secondaryViewFrustum.calculate();

    _conicalViews.push_back(secondaryViewFrustum);
}

void ViewState::updateCamera(RenderArgs& renderArgs, float deltaTime) {
    PROFILE_RANGE(render, __FUNCTION__);
    PerformanceTimer perfTimer("updateCamera");

    glm::vec3 boomOffset;
    auto myAvatar = getMyAvatar();
    boomOffset = myAvatar->getModelScale() * myAvatar->getBoomLength() * -IDENTITY_FORWARD;

    // The render mode is default or mirror if the camera is in mirror mode, assigned further below
    renderArgs._renderMode = RenderArgs::DEFAULT_RENDER_MODE;

    // build mode drives the camera itself, on CAMERA_MODE_INDEPENDENT
    bool editing = false;
    {
        auto editor = DependencyManager::get<Editor>();
        if (editor && editor->isActive()) {
            editor->update(deltaTime);
            editing = true;
        }
    }

    // Always use the default eye position, not the actual head eye position.
    // Using the latter will cause the camera to wobble with idle animations,
    // or with changes from the face tracker
    CameraMode mode = _camera.getMode();
    bool boomPlaced = false;   // the desktop's third-person boom, which camera collision keeps out of walls
    if (mode == CAMERA_MODE_FIRST_PERSON || mode == CAMERA_MODE_FIRST_PERSON_LOOK_AT) {
        _thirdPersonHMDCameraBoomValid= false;
        if (qApp->isHMDMode()) {
            mat4 camMat = myAvatar->getSensorToWorldMatrix() * myAvatar->getHMDSensorMatrix();
            _camera.setPosition(extractTranslation(camMat));
            _camera.setOrientation(glmExtractRotation(camMat));
        } else if (mode == CAMERA_MODE_FIRST_PERSON) {
            _camera.setPosition(myAvatar->getDefaultEyePosition());
            _camera.setOrientation(myAvatar->getMyHead()->getHeadOrientation());
        } else {
            _camera.setPosition(myAvatar->getCameraEyesPosition(deltaTime));
            _camera.setOrientation(myAvatar->getLookAtRotation());
        }
    } else if (mode == CAMERA_MODE_THIRD_PERSON || mode == CAMERA_MODE_LOOK_AT || mode == CAMERA_MODE_SELFIE) {
        if (qApp->isHMDMode()) {
            if (!_thirdPersonHMDCameraBoomValid) {
                const glm::vec3 CAMERA_OFFSET = glm::vec3(0.0f, 0.0f, 0.7f);
                _thirdPersonHMDCameraBoom = cancelOutRollAndPitch(myAvatar->getHMDSensorOrientation()) * CAMERA_OFFSET;
                _thirdPersonHMDCameraBoomValid = true;
            }

            glm::mat4 thirdPersonCameraSensorToWorldMatrix = myAvatar->getSensorToWorldMatrix();

            const glm::vec3 cameraPos = myAvatar->getHMDSensorPosition() + _thirdPersonHMDCameraBoom * myAvatar->getBoomLength();
            glm::mat4 sensorCameraMat = createMatFromQuatAndPos(myAvatar->getHMDSensorOrientation(), cameraPos);
            glm::mat4 worldCameraMat = thirdPersonCameraSensorToWorldMatrix * sensorCameraMat;

            _camera.setOrientation(glm::normalize(glmExtractRotation(worldCameraMat)));
            _camera.setPosition(extractTranslation(worldCameraMat));
        } else {
            _thirdPersonHMDCameraBoomValid = false;
            boomPlaced = true;
            if (mode == CAMERA_MODE_THIRD_PERSON) {
                _camera.setOrientation(myAvatar->getHead()->getOrientation());
                glm::vec3 pivot = myAvatar->getDefaultEyePosition();
                if (qApp->isOptionChecked(MenuOption::CenterPlayerInView)) {
                    _camera.setPosition(collideCameraBoom(pivot, pivot + _camera.getOrientation() * boomOffset, deltaTime));
                } else {
                    _camera.setPosition(collideCameraBoom(pivot, pivot + myAvatar->getWorldOrientation() * boomOffset, deltaTime));
                }
            } else {
                glm::quat lookAtRotation = myAvatar->getLookAtRotation();
                if (mode == CAMERA_MODE_SELFIE) {
                    lookAtRotation = lookAtRotation * glm::angleAxis(PI, myAvatar->getWorldOrientation() * Vectors::UP);
                    // the picture as a mirror shows you (Caity, 2026-10-28: like FaceTime's own picture): your left on
                    // the picture's left, writing backwards. On a flat screen only: a stereo picture (two eyes side by
                    // side) is never flipped
                    renderArgs._mirrored = !qApp->getActiveDisplayPlugin()->isStereo();
                }
                glm::vec3 pivot = myAvatar->getLookAtPivotPoint();
                glm::vec3 wanted = pivot + lookAtRotation * boomOffset;
                // the orientation from where the camera wants to be: collision only shortens the boom, and may
                // shorten it to nothing
                _camera.setPosition(wanted);
                _camera.lookAt(pivot);
                _camera.setPosition(collideCameraBoom(pivot, wanted, deltaTime));
            }
        }
    } else if (mode == CAMERA_MODE_MIRROR) {
        _thirdPersonHMDCameraBoomValid= false;

        if (qApp->isHMDMode()) {
            auto mirrorBodyOrientation = myAvatar->getWorldOrientation() * glm::quat(glm::vec3(0.0f, PI + _mirrorYawOffset, 0.0f));

            glm::quat hmdRotation = extractRotation(myAvatar->getHMDSensorMatrix());
            // Mirror HMD yaw and roll
            glm::vec3 mirrorHmdEulers = glm::eulerAngles(hmdRotation);
            mirrorHmdEulers.y = -mirrorHmdEulers.y;
            mirrorHmdEulers.z = -mirrorHmdEulers.z;
            glm::quat mirrorHmdRotation = glm::quat(mirrorHmdEulers);

            glm::quat worldMirrorRotation = mirrorBodyOrientation * mirrorHmdRotation;

            _camera.setOrientation(worldMirrorRotation);

            glm::vec3 hmdOffset = extractTranslation(myAvatar->getHMDSensorMatrix());
            // Mirror HMD lateral offsets
            hmdOffset.x = -hmdOffset.x;

            _camera.setPosition(myAvatar->getDefaultEyePosition()
                + glm::vec3(0, _raiseMirror * myAvatar->getModelScale(), 0)
                + mirrorBodyOrientation * glm::vec3(0.0f, 0.0f, 1.0f) * MIRROR_FULLSCREEN_DISTANCE * _scaleMirror
                + mirrorBodyOrientation * hmdOffset);
        } else {
            auto userInputMapper = DependencyManager::get<UserInputMapper>();
            const float YAW_SPEED = TWO_PI / 5.0f;
            float deltaYaw = userInputMapper->getActionState(controller::Action::YAW) * YAW_SPEED * deltaTime;
            _mirrorYawOffset += deltaYaw;
            _camera.setOrientation(myAvatar->getWorldOrientation() * glm::quat(glm::vec3(0.0f, PI + _mirrorYawOffset, 0.0f)));
            _camera.setPosition(myAvatar->getDefaultEyePosition()
                + glm::vec3(0, _raiseMirror * myAvatar->getModelScale(), 0)
                + (myAvatar->getWorldOrientation() * glm::quat(glm::vec3(0.0f, _mirrorYawOffset, 0.0f))) *
                glm::vec3(0.0f, 0.0f, -1.0f) * myAvatar->getBoomLength() * _scaleMirror);
        }
        renderArgs._renderMode = RenderArgs::MIRROR_RENDER_MODE;
        renderArgs._mirrored = true;
    } else if (mode == CAMERA_MODE_ENTITY) {
        _thirdPersonHMDCameraBoomValid= false;
        if (auto cameraEntity = _camera.getParentPointer()) {
            bool success;
            Transform entityTransform = _camera.parentJointIndex != -1 ?
              cameraEntity->getJointTransform(_camera.parentJointIndex, success) :
              cameraEntity->getTransform();
            Transform localTransform = { _camera.localRotation, glm::vec3(), _camera.localPosition };
            Transform worldTransform = entityTransform.worldTransform(localTransform);
            if (qApp->isHMDMode()) {
                glm::quat hmdRotation = extractRotation(myAvatar->getHMDSensorMatrix());
                _camera.setOrientation(worldTransform.getRotation() * hmdRotation);
                glm::vec3 hmdOffset = extractTranslation(myAvatar->getHMDSensorMatrix());
                _camera.setPosition(worldTransform.getTranslation() + (hmdRotation * hmdOffset));
            } else {
                _camera.setOrientation(worldTransform.getRotation());
                _camera.setPosition(worldTransform.getTranslation());
            }
        }
    }
    if (mode != CAMERA_MODE_INDEPENDENT || editing || !updateChaseCamera(deltaTime)) {
        _chaseValid = false;   // the next chase starts where it wants to be, not eased from an old place
    }
    if (!boomPlaced) {
        _boomLength = -1.0f;   // the next third-person boom starts where it wants to be, not eased from an old length
        _boomHeldIn = false;
    }
    // Update camera position
    if (!qApp->isHMDMode()) {
        _camera.update();
    }
    traceCamera(_camera);

    renderArgs._cameraMode = (int8_t)_camera.getMode();
    _mirrored = renderArgs._mirrored;
}

// What stops the camera: what would stop you (the casts are made as your avatar's body: other avatars, collisionless
// entities and entities that don't collide with you let it through), less what you can see through (a shape entity that
// isn't opaque, such as glass), small things that move (a ball rolling by shouldn't yank the view) and the entities the
// caller names with their children (the vehicle a chase camera follows).
class CameraBoomFilter : public JPH::BodyFilter {
public:
    explicit CameraBoomFilter(const QSet<QUuid>& ignored) : _ignored(ignored) {}
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        const float SMALL_MOVING_THING = 1.0f;   // m, the largest side of its bounding box
        if (body.IsDynamic() && body.GetShape()->GetLocalBounds().GetSize().ReduceMax() < SMALL_MOVING_THING) {
            return false;
        }
        if (auto motionState = dynamic_cast<EntityMotionState*>(PhysicsEngine::motionStateOf(body))) {
            const EntityItemPointer& entity = motionState->getEntity();
            if (entity && _ignored.contains(entity->getID())) {
                return false;
            }
            auto shape = std::dynamic_pointer_cast<ShapeEntityItem>(entity);
            if (shape && shape->getAlpha() < 1.0f) {
                return false;
            }
        }
        return true;
    }
private:
    const QSet<QUuid>& _ignored;
};

float ViewState::allowedBoomLength(const glm::vec3& pivot, const glm::vec3& wanted, const QVector<QUuid>& ignore,
                                   glm::vec3* clearPivot, QUuid* holder) const {
    if (clearPivot) {
        *clearPivot = pivot;
    }
    if (holder) {
        *holder = QUuid();
    }
    float wantedLength = glm::distance(pivot, wanted);
    PhysicsEnginePointer physicsEngine = qApp->getPhysicsEngine();
    if (!physicsEngine || wantedLength < EPSILON || !qApp->isOptionChecked(MenuOption::CameraCollision)) {
        return wantedLength;
    }
    QSet<QUuid> ignored;   // the named entities and their children (a vehicle's parts)
    if (!ignore.isEmpty()) {
        EntityTreePointer tree = qApp->getEntities()->getTree();
        for (const QUuid& id : ignore) {
            ignored.insert(id);
            EntityItemPointer entity = tree ? tree->findEntityByID(id) : nullptr;
            if (entity) {
                entity->forEachDescendant([&](const SpatiallyNestablePointer& child) { ignored.insert(child->getID()); });
            }
        }
    }

    // The probe: a sphere round the camera's near plane, out to its corners (at a wide field of view on a wide window
    // they reach well past twice the near clip), swept out from the pivot. Its start is first moved out of whatever it
    // overlaps: a pivot pressed against a wall (Caity's strafing, 10-25) would otherwise start the sphere inside the
    // wall, and the sweep skips what it starts inside. So the camera never stands nearer a wall than the probe's radius.
    // A ray from the same start guards a thin wall the sphere could straddle.
    float radius;
    {
        QMutexLocker viewLocker(&_mutex);
        float nearClip = _viewFrustum.getNearClip();
        float halfHeight = nearClip * tanf(glm::radians(0.5f * _viewFrustum.getFieldOfView()));
        float halfWidth = halfHeight * _viewFrustum.getAspectRatio();
        const float MARGIN = 1.1f;
        radius = glm::max(2.0f * nearClip, MARGIN * sqrtf(nearClip * nearClip + halfHeight * halfHeight + halfWidth * halfWidth));
    }
    const int32_t group = BULLET_COLLISION_GROUP_MY_AVATAR;
    const int32_t mask = BULLET_COLLISION_GROUP_STATIC | BULLET_COLLISION_GROUP_DYNAMIC | BULLET_COLLISION_GROUP_KINEMATIC;
    CameraBoomFilter filter(ignored);
    glm::vec3 offset = ObjectMotionState::getWorldOffset();   // world to the physics' coordinates
    glm::vec3 start = physicsEngine->sphereOutOfOverlapsAllWorlds(pivot - offset, radius, group, mask, &filter) + offset;
    if (clearPivot) {
        *clearPivot = start;
    }
    float startLength = glm::distance(start, wanted);
    if (startLength < EPSILON) {
        return 0.0f;
    }
    float fraction = 1.0f;
    RayHit hit;
    ObjectMotionState* stop = nullptr;   // what held the probe, for the trace
    if (physicsEngine->castSphereClosestAllWorlds(start - offset, wanted - offset, radius, group, mask, &filter, hit)) {
        fraction = hit.fraction;
        stop = hit.motionState;
    }
    if (physicsEngine->castRayClosestAllWorlds(start - offset, wanted - offset, group, mask, &filter, hit)) {
        float rayFraction = hit.fraction - radius / startLength;
        if (rayFraction < fraction) {
            fraction = rayFraction;
            stop = hit.motionState;
        }
    }
    if (holder && fraction < 1.0f && stop && stop->getType() == MOTIONSTATE_TYPE_ENTITY) {
        *holder = static_cast<EntityMotionState*>(stop)->getEntity()->getID();
    }
    // as a length from the pivot the caller gave (the start is at most a few centimetres from it)
    return glm::clamp(fraction, 0.0f, 1.0f) * (clearPivot ? startLength : wantedLength);
}

glm::vec3 ViewState::collideCameraBoom(const glm::vec3& pivot, const glm::vec3& wanted, float deltaTime) {
    const float EASE_OUT_TIME = 0.12f;   // s, the time constant of the way back out: 95 % in about 0.35 s
    glm::vec3 boom = wanted - pivot;
    float wantedLength = glm::length(boom);
    if (wantedLength < EPSILON) {
        _boomLength = wantedLength;
        _boomHeldIn = false;
        return wanted;
    }
    glm::vec3 start = pivot;   // the pivot moved out of a wall it was pressed against
    // The cast passes through the frame or vehicle the avatar rides and its parts (a car's walls and rails, a seat's
    // vehicle): nothing of the thing you ride is a wall between you and your own camera, and a sweep that clipped a
    // rail pulled the camera inside the car at every turn of the view (Caity's monorail ride, 2026-10-31). The chase
    // camera passes through the chased entity the same way.
    QVector<QUuid> ride;
    if (auto myAvatar = getMyAvatar()) {
        EntityTreePointer tree = qApp->getEntities()->getTree();
        QUuid id = myAvatar->getParentID();
        const int MAX_DEPTH = 16;
        for (int depth = 0; tree && !id.isNull() && depth < MAX_DEPTH; depth++) {
            ride.append(id);
            EntityItemPointer entity = tree->findEntityByID(id);
            id = entity ? entity->getParentID() : QUuid();
        }
    }
    QUuid holder;
    float allowed = allowedBoomLength(pivot, wanted, ride, &start, &holder);
    boom = wanted - start;
    wantedLength = glm::length(boom);
    if (wantedLength < EPSILON) {
        _boomLength = 0.0f;
        _boomHeldIn = false;
        return start;
    }
    if (_boomLength < 0.0f || allowed <= _boomLength || !_boomHeldIn) {
        _boomLength = allowed;   // in at once (a wall must never show for a frame), and the zoom followed as it is
    } else {
        _boomLength += (allowed - _boomLength) * (1.0f - glm::exp(-deltaTime / EASE_OUT_TIME));
    }
    const float SETTLED = 0.01f;   // m
    _boomHeldIn = _boomLength < wantedLength - SETTLED;
    if (FrameTrace::enabled()) {   // the boom's columns of the frame trace (item 7; traceCamera has the rest)
        FrameTrace::set("boom.wanted_m", wantedLength);
        FrameTrace::set("boom.allowed_m", allowed);
        FrameTrace::set("boom.current_m", _boomLength);
        FrameTrace::setText("boom.holder", holder.isNull() ? QString() : holder.toString());
    }
    return start + (boom / wantedLength) * _boomLength;
}

// The chase camera behind an entity (Camera.setChaseTarget; vehicles.js's for the vehicle you drive), worked out every
// frame from the entity as this frame draws it. vehicles.js placed it from its own update, on the script's thread at 60
// a second, so on a 144 Hz display the camera stood still for two frames in three and then jumped: steps of 0, 23 and
// 34 cm against kart 2's even 12 cm at 17 m/s, the whole view lurching against the kart (physics, 2026-10-26,
// tests-manual/physics-baseline/judder.js). The same chase: behind along the heading (level, or with the entity's pitch
// as the car's was) and above, settling at a rate by the frame's time (the same feel at 30 frames a second as at 144),
// kept out of walls with the entity and its children seen through (moving in at once, back out eased), looking at a
// point on the entity from where it has settled.
bool ViewState::updateChaseCamera(float deltaTime) {
    FancyCamera::ChaseSettings chase = _camera.getChaseSettings();
    if (chase.entityID.isNull()) {
        return false;
    }
    EntityTreePointer tree = qApp->getEntities()->getTree();
    EntityItemPointer entity = tree ? tree->findEntityByID(chase.entityID) : nullptr;
    if (!entity) {
        return false;
    }
    bool fresh = !_chaseValid || chase.version != _chaseVersion;
    glm::vec3 position = entity->getWorldPosition();
    glm::vec3 front = entity->getWorldOrientation() * IDENTITY_FORWARD;
    glm::vec3 flat(front.x, 0.0f, front.z);
    const float LEVEL_ENOUGH = 1.0e-4f;   // the heading's horizontal part, squared: under it the entity points up or down
    if (glm::length2(flat) > LEVEL_ENOUGH) {
        _chaseFlat = glm::normalize(flat);
    } else if (fresh) {
        _chaseFlat = IDENTITY_FORWARD;
    }
    glm::vec3 behind = chase.followPitch ? front : _chaseFlat;
    glm::vec3 wanted = position - chase.back * behind + glm::vec3(0.0f, chase.up, 0.0f);
    if (fresh) {
        _chasePosition = wanted;
        _chaseBoom = -1.0f;
        _chaseHeldIn = false;
        _chaseVersion = chase.version;
        _chaseValid = true;
    } else {
        _chasePosition = glm::mix(_chasePosition, wanted, 1.0f - glm::exp(-deltaTime * chase.rate));
    }

    // camera collision, as Camera.collideBoom's callers do it: in at once, back out with the boom's time constant
    const float EASE_OUT_TIME = 0.12f;   // s
    glm::vec3 pivot = position + glm::vec3(0.0f, chase.pivotHeight, 0.0f);
    glm::vec3 boom = _chasePosition - pivot;
    float length = glm::length(boom);
    glm::vec3 shown = _chasePosition;
    if (length > EPSILON) {
        QUuid holder;
        float allowed = allowedBoomLength(pivot, _chasePosition, QVector<QUuid>{ chase.entityID }, nullptr, &holder);
        if (_chaseBoom < 0.0f || allowed <= _chaseBoom || !_chaseHeldIn) {
            _chaseBoom = allowed;   // in at once, and the chase's own movement followed as it is
        } else {
            _chaseBoom += (allowed - _chaseBoom) * (1.0f - glm::exp(-deltaTime / EASE_OUT_TIME));
        }
        _chaseBoom = std::min(_chaseBoom, length);
        const float SETTLED = 0.01f;   // m
        _chaseHeldIn = _chaseBoom < length - SETTLED;
        shown = pivot + boom * (_chaseBoom / length);
        if (FrameTrace::enabled()) {   // the chase's boom in the frame trace's boom columns (one boom a frame)
            FrameTrace::set("boom.wanted_m", length);
            FrameTrace::set("boom.allowed_m", allowed);
            FrameTrace::set("boom.current_m", _chaseBoom);
            FrameTrace::setText("boom.holder", holder.isNull() ? QString() : holder.toString());
        }
    }
    _camera.setPosition(shown);

    // looking at the point from where the camera has settled (Quat.lookAtSimple), so a wall that holds it in moves it
    // along the line of sight and does not swing the view
    glm::vec3 target = position + chase.lookAhead * _chaseFlat + glm::vec3(0.0f, chase.lookHeight, 0.0f);
    glm::vec3 direction = target - _chasePosition;
    if (glm::length2(direction) > EPSILON) {
        const float MAX_ABS_Y_COMPONENT = 0.9999991f;   // straight up or down: x for up, as Quat.lookAtSimple does
        glm::vec3 up = fabsf(glm::normalize(direction).y) > MAX_ABS_Y_COMPONENT ? Vectors::UNIT_X : Vectors::UNIT_Y;
        _camera.setOrientation(glm::inverse(glm::quat_cast(glm::lookAt(_chasePosition, target, up))));
    }
    return true;
}

// the camera's mode and the View menu, kept in step

void ViewState::cycleCamera() {
    auto menu = Menu::getInstance();
    if (menu->isOptionChecked(MenuOption::FirstPersonLookAt)) {

        menu->setIsOptionChecked(MenuOption::FirstPersonLookAt, false);
        menu->setIsOptionChecked(MenuOption::LookAtCamera, true);

    } else if (menu->isOptionChecked(MenuOption::LookAtCamera)) {

        menu->setIsOptionChecked(MenuOption::LookAtCamera, false);
        if (menu->getActionForOption(MenuOption::SelfieCamera)->isVisible()) {
            menu->setIsOptionChecked(MenuOption::SelfieCamera, true);
        } else {
            menu->setIsOptionChecked(MenuOption::FirstPersonLookAt, true);
        }

    } else if (menu->isOptionChecked(MenuOption::SelfieCamera)) {

        menu->setIsOptionChecked(MenuOption::SelfieCamera, false);
        menu->setIsOptionChecked(MenuOption::FirstPersonLookAt, true);

    }
    cameraMenuChanged(); // handle the menu change
}

void ViewState::cameraModeChanged() {
    switch (_camera.getMode()) {
        case CAMERA_MODE_FIRST_PERSON_LOOK_AT:
            Menu::getInstance()->setIsOptionChecked(MenuOption::FirstPersonLookAt, true);
            break;
        case CAMERA_MODE_LOOK_AT:
            Menu::getInstance()->setIsOptionChecked(MenuOption::LookAtCamera, true);
            break;
        case CAMERA_MODE_SELFIE:
            Menu::getInstance()->setIsOptionChecked(MenuOption::SelfieCamera, true);
            break;
        default:
            // we don't have menu items for the others, so just leave it alone.
            return;
    }
    cameraMenuChanged();
}

void ViewState::changeViewAsNeeded(float boomLength) {
    // Switch between first and third person views as needed
    // This is called when the boom length has changed
    bool boomLengthGreaterThanMinimum = (boomLength > MyAvatar::ZOOM_MIN);

    if (
        (
            _camera.getMode() == CAMERA_MODE_FIRST_PERSON_LOOK_AT ||
            _camera.getMode() == CAMERA_MODE_FIRST_PERSON
        ) && boomLengthGreaterThanMinimum
    ) {
        Menu::getInstance()->setIsOptionChecked(MenuOption::FirstPersonLookAt, false);
        Menu::getInstance()->setIsOptionChecked(MenuOption::LookAtCamera, true);
        cameraMenuChanged();
    } else if (
        (
            _camera.getMode() == CAMERA_MODE_LOOK_AT ||
            _camera.getMode() == CAMERA_MODE_THIRD_PERSON
        ) && !boomLengthGreaterThanMinimum
    ) {
        Menu::getInstance()->setIsOptionChecked(MenuOption::FirstPersonLookAt, true);
        Menu::getInstance()->setIsOptionChecked(MenuOption::LookAtCamera, false);
        cameraMenuChanged();
    }
}

void ViewState::cameraMenuChanged() {
    auto menu = Menu::getInstance();
    if (menu->isOptionChecked(MenuOption::FirstPersonLookAt)) {
        if (_camera.getMode() != CAMERA_MODE_FIRST_PERSON_LOOK_AT) {
            _camera.setMode(CAMERA_MODE_FIRST_PERSON_LOOK_AT);
            getMyAvatar()->setBoomLength(MyAvatar::ZOOM_MIN);
        }
    } else if (menu->isOptionChecked(MenuOption::LookAtCamera)) {
        if (_camera.getMode() != CAMERA_MODE_LOOK_AT) {
            _camera.setMode(CAMERA_MODE_LOOK_AT);
            if (getMyAvatar()->getBoomLength() == MyAvatar::ZOOM_MIN) {
                getMyAvatar()->setBoomLength(MyAvatar::ZOOM_DEFAULT);
            }
        }
    } else if (menu->isOptionChecked(MenuOption::SelfieCamera)) {
        if (_camera.getMode() != CAMERA_MODE_SELFIE) {
            _camera.setMode(CAMERA_MODE_SELFIE);
            if (getMyAvatar()->getBoomLength() == MyAvatar::ZOOM_MIN) {
                getMyAvatar()->setBoomLength(MyAvatar::ZOOM_DEFAULT);
            }
        }
    }
}

void ViewState::rotationModeChanged() const {
    if (!Menu::getInstance()->isOptionChecked(MenuOption::CenterPlayerInView)) {
        getMyAvatar()->setHeadPitch(0);
    }
}

void ViewState::setModeKeepingPrevious(CameraMode mode) {
    _previousMode = _camera.getMode();
    _camera.setMode(mode);
    cameraModeChanged();
}

void ViewState::restorePreviousMode() {
    _camera.setMode(_previousMode);
    cameraModeChanged();
}
