//
//  VrScreen.cpp
//  interface/src/ui
//
//  See VrScreen.h.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "VrScreen.h"

#include <QtQuick/QQuickWindow>

#include <EntityScriptingInterface.h>
#include <EntityTreeRenderer.h>
#include <GLMHelpers.h>
#include <NumericalConstants.h>
#include <RenderableWebEntityItem.h>
#include <shared/LocalFileAccessGate.h>
#include <SpatiallyNestable.h>
#include <controllers/Actions.h>

#include "Application.h"
#include "InterfaceLogging.h"
#include "avatar/AvatarManager.h"
#include "avatar/MyAvatar.h"
#include "ui/Keyboard.h"
#include "ui/Panels.h"

using namespace vrscreen;

namespace {

const QString SLOT_LEFT_URL = "qrc:///qml/darkride/ScreenSlotLeft.qml";
const QString SLOT_MIDDLE_URL = "qrc:///qml/darkride/ScreenSlotMiddle.qml";
const QString SLOT_RIGHT_URL = "qrc:///qml/darkride/ScreenSlotRight.qml";
const QString GRAB_BAR_URL = "qrc:///qml/darkride/ScreenGrabBar.qml";
const glm::vec2 GRAB_BAR_PIXELS { 240.0f, 35.0f };
const char* SLOT_RIGHT = "right";
const float PIECE_THICKNESS = 0.01f;
const int WEB_MAX_FPS = 90;          // as the tablet; a surface only redraws when its page changes
const int DRAG_INTERVAL_MS = 11;     // about 90 Hz, only while the bar is held
const int REMAKE_DELAY_MS = 500;

// The app's own QML, made from C++. A Web entity reads this at creation (WebEntityItem.cpp); it's up only for the call
// that creates the entity, with no events processed inside, so nothing else can be made under it.
class AppMadeEntityScope {
public:
    AppMadeEntityScope() { ::hifi::scripting::setLocalAccessSafeThread(true); }
    ~AppMadeEntityScope() { ::hifi::scripting::setLocalAccessSafeThread(false); }
};

float degreesPerPixel() {
    static const float value = [] {
        bool ok = false;
        const float fromEnvironment = qEnvironmentVariable("DARKRIDE_VR_SCREEN_DEGREES_PER_PIXEL").toFloat(&ok);
        return ok && fromEnvironment > 0.0f ? fromEnvironment : DEFAULT_DEGREES_PER_PIXEL;
    }();
    return value;
}

glm::vec2 toVec2(const QSize& size) {
    return glm::vec2(size.width(), size.height());
}

}   // namespace

bool VrScreen::isOn() {
    static const bool on = qEnvironmentVariableIsSet("DARKRIDE_VR_SCREEN");
    return on;
}

VrScreen::VrScreen() {
    _dragTimer.setInterval(DRAG_INTERVAL_MS);
    connect(&_dragTimer, &QTimer::timeout, this, &VrScreen::dragTick);
    if (isOn()) {
        Panels::setScreenWindowCheck([](QQuickWindow* window) {
            auto screen = DependencyManager::get<VrScreen>();
            return screen && screen->ownsWindow(window);
        });
    }
}

void VrScreen::connectOnce() {
    if (_connected) {
        return;
    }
    _connected = true;
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    connect(entities.data(), &EntityScriptingInterface::deletingEntity, this, &VrScreen::entityDeleted);
    connect(entities.data(), &EntityScriptingInterface::mousePressOnEntity, this, &VrScreen::mousePressed);
    connect(entities.data(), &EntityScriptingInterface::mouseReleaseOnEntity, this, &VrScreen::mouseReleased);
    connect(DependencyManager::get<Panels>().data(), &Panels::screenSlotSizeChanged, this, &VrScreen::slotSizeChanged);
    connect(qApp, &Application::activeDisplayPluginChanged, this, &VrScreen::displayChanged);
    _wasHeadset = qApp->isHMDMode();
}

void VrScreen::toggle() {
    if (_shown) {
        hide();
    } else {
        show();
    }
}

void VrScreen::show() {
    if (_shown) {
        return;
    }
    connectOnce();
    updateHead();
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    const glm::vec3 facing = avatar->getHMDSensorOrientation() * glm::vec3(0.0f, 0.0f, -1.0f);
    _yaw = yawOf(facing);
    _pitch = DEFAULT_PITCH_DEGREES;
    _shown = true;

    const Arrangement places = arrangement();
    makePiece(Left, places);
    makePiece(Middle, places);
    if (DependencyManager::get<Panels>()->screenSlotSize(SLOT_RIGHT).isValid()) {
        makePiece(Right, places);
    }
    makePiece(Bar, places);
    tellState();
    qCDebug(interfaceapp) << "VR screen: shown, yaw" << _yaw;
}

void VrScreen::hide() {
    if (!_shown) {
        return;
    }
    endDrag();
    _shown = false;
    // the pages first: a page's host dying would take the page with it
    DependencyManager::get<Panels>()->releaseScreenSlots();
    _changing = true;
    for (int piece = 0; piece < PIECE_COUNT; ++piece) {
        deletePiece((Piece)piece);
    }
    _changing = false;
    DependencyManager::get<Keyboard>()->setRaised(false);
    qCDebug(interfaceapp) << "VR screen: hidden";
}

bool VrScreen::ownsWindow(QQuickWindow* window) const {
    if (!window || !_shown) {
        return false;
    }
    auto entities = qApp->getEntities();
    if (!entities) {
        return false;
    }
    for (Piece piece : { Left, Middle, Right }) {
        if (_pieces[piece].isNull()) {
            continue;
        }
        auto web = std::dynamic_pointer_cast<render::entities::WebEntityRenderer>(
            entities->renderableForEntityId(EntityItemID(_pieces[piece])));
        if (web && web->getEventHandler() == window) {
            return true;
        }
    }
    return false;
}

Arrangement VrScreen::arrangement() const {
    auto panels = DependencyManager::get<Panels>();
    SlotPixels pixels;
    pixels.left = toVec2(panels->screenSlotSize("left"));
    pixels.middle = toVec2(panels->screenSlotSize("middle"));
    pixels.right = toVec2(panels->screenSlotSize(SLOT_RIGHT));
    return arrange(pixels, degreesPerPixel());
}

Pose VrScreen::placedPiece(const Arrangement& places, Piece piece) const {
    const Pose& arranged = piece == Left ? places.left : piece == Middle ? places.middle : piece == Right ? places.right : places.bar;
    return placed(arranged, _head, _yaw, _pitch);
}

void VrScreen::makePiece(Piece piece, const Arrangement& places) {
    if (!_pieces[piece].isNull()) {
        return;
    }
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    auto panels = DependencyManager::get<Panels>();
    const Pose pose = placedPiece(places, piece);
    const glm::mat4 sensorToWorld = avatar->getSensorToWorldMatrix();
    const float scale = avatar->getSensorToWorldScale();

    static const char* NAMES[] = { "left", "middle", "right" };
    glm::vec2 pixels = GRAB_BAR_PIXELS;
    QString url = GRAB_BAR_URL;
    QString name = "VR screen grab bar";
    if (piece != Bar) {
        pixels = toVec2(panels->screenSlotSize(NAMES[piece]));
        url = piece == Left ? SLOT_LEFT_URL : piece == Middle ? SLOT_MIDDLE_URL : SLOT_RIGHT_URL;
        name = QString("VR screen ") + NAMES[piece];
    }
    const float dpi = dpiForWidth(pixels.x, pose.size.x * scale);
    const glm::vec2 metres = metresForPixels(pixels, dpi);

    EntityItemProperties properties;
    properties.setType(EntityTypes::Web);
    properties.setName(name);
    properties.setSourceUrl(url);
    properties.setParentID(AVATAR_SELF_ID);
    properties.setParentJointIndex(SENSOR_TO_WORLD_MATRIX_INDEX);
    properties.setPosition(transformPoint(sensorToWorld, pose.position));
    properties.setRotation(glmExtractRotation(sensorToWorld) * pose.rotation);
    properties.setDimensions(glm::vec3(metres, PIECE_THICKNESS * scale));
    properties.setDPI((uint16_t)dpi);
    properties.setMaxFPS(WEB_MAX_FPS);
    properties.setPrimitiveMode(PrimitiveMode::SOLID);
    properties.getGrab().setGrabbable(false);
    properties.setIgnorePickIntersection(false);
    properties.setAlpha(1.0f);
    properties.setVisible(true);

    auto entities = DependencyManager::get<EntityScriptingInterface>();
    {
        AppMadeEntityScope appMade;
        _pieces[piece] = entities->addEntityInternal(properties, entity::HostType::LOCAL);
    }
    render::entities::WebEntityRenderer::setAppSurface(_pieces[piece], true);
}

void VrScreen::deletePiece(Piece piece) {
    if (_pieces[piece].isNull()) {
        return;
    }
    const QUuid id = _pieces[piece];
    _pieces[piece] = QUuid();
    render::entities::WebEntityRenderer::setAppSurface(id, false);
    DependencyManager::get<EntityScriptingInterface>()->deleteEntity(id);
}

void VrScreen::movePieces() {
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    const glm::mat4 sensorToWorld = avatar->getSensorToWorldMatrix();
    const glm::quat sensorToWorldRotation = glmExtractRotation(sensorToWorld);
    const Arrangement places = arrangement();
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    for (int piece = 0; piece < PIECE_COUNT; ++piece) {
        if (_pieces[piece].isNull()) {
            continue;
        }
        const Pose pose = placedPiece(places, (Piece)piece);
        EntityItemProperties properties;
        properties.setPosition(transformPoint(sensorToWorld, pose.position));
        properties.setRotation(sensorToWorldRotation * pose.rotation);
        entities->editEntity(_pieces[piece], properties);
    }
}

void VrScreen::updateHead() {
    _head = DependencyManager::get<AvatarManager>()->getMyAvatar()->getHMDSensorPosition();
}

void VrScreen::slotSizeChanged(const QString& slot) {
    if (!_shown || slot != SLOT_RIGHT) {
        return;
    }
    // the radio on or off: the right slot's entity comes and goes, and nothing else moves. By the time it's off, its
    // page has gone back to its dock (Panels' close path)
    if (DependencyManager::get<Panels>()->screenSlotSize(SLOT_RIGHT).isValid()) {
        makePiece(Right, arrangement());
    } else {
        _changing = true;
        deletePiece(Right);
        _changing = false;
    }
}

void VrScreen::entityDeleted(const EntityItemID& id) {
    if (_changing || !_shown) {
        return;
    }
    bool ours = false;
    for (const QUuid& piece : _pieces) {
        ours = ours || piece == id;
    }
    if (!ours) {
        return;
    }
    // someone else's delete (the whole entity tree cleared): the pages out at once, then the screen again where it was
    qCDebug(interfaceapp) << "VR screen: a piece was deleted from outside; bringing the screen back";
    const float yaw = _yaw, pitch = _pitch;
    const glm::vec3 head = _head;
    hide();
    QTimer::singleShot(REMAKE_DELAY_MS, this, [this, yaw, pitch, head] {
        if (_shown) {
            return;
        }
        show();
        _yaw = yaw;
        _pitch = pitch;
        _head = head;
        movePieces();
        tellState();
    });
}

void VrScreen::displayChanged() {
    const bool headset = qApp->isHMDMode();
    if (_wasHeadset && !headset) {
        hide();
    }
    _wasHeadset = headset;
}

void VrScreen::mousePressed(const EntityItemID& id, const PointerEvent& event) {
    if (_shown && !_dragging && id == EntityItemID(_pieces[Bar]) && event.getButton() == PointerEvent::PrimaryButton) {
        beginDrag();
    }
}

void VrScreen::mouseReleased(const EntityItemID& id, const PointerEvent& event) {
    if (_dragging && id == EntityItemID(_pieces[Bar]) && event.getButton() == PointerEvent::PrimaryButton) {
        endDrag();
    }
}

void VrScreen::beginDrag() {
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    const controller::Pose left = avatar->getControllerPoseInSensorFrame(controller::Action::LEFT_HAND);
    const controller::Pose right = avatar->getControllerPoseInSensorFrame(controller::Action::RIGHT_HAND);
    if (!left.isValid() && !right.isValid()) {
        return;
    }
    // the hand whose laser is nearest the bar (a hand's laser runs along its pose's up axis)
    const glm::vec3 bar = placedPiece(arrangement(), Bar).position;
    auto missBy = [&](const controller::Pose& hand) {
        const glm::vec3 direction = hand.getRotation() * Vectors::UP;
        const glm::vec3 toBar = bar - hand.getTranslation();
        return glm::length(toBar - direction * glm::dot(toBar, direction));
    };
    _dragRightHand = right.isValid() && (!left.isValid() || missBy(right) <= missBy(left));
    const glm::vec3 direction = (_dragRightHand ? right : left).getRotation() * Vectors::UP;
    _dragStartHandYaw = yawOf(direction);
    _dragStartHandPitch = pitchOf(direction);
    _dragStartYaw = _yaw;
    _dragStartPitch = _pitch;
    _dragging = true;
    _dragTimer.start();
}

void VrScreen::dragTick() {
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    const controller::Pose hand = avatar->getControllerPoseInSensorFrame(
        _dragRightHand ? controller::Action::RIGHT_HAND : controller::Action::LEFT_HAND);
    if (!hand.isValid()) {
        endDrag();
        return;
    }
    const glm::vec3 direction = hand.getRotation() * Vectors::UP;
    _yaw = _dragStartYaw + yawDifference(yawOf(direction), _dragStartHandYaw);
    _pitch = glm::clamp(_dragStartPitch + pitchOf(direction) - _dragStartHandPitch, MIN_PITCH_DEGREES, MAX_PITCH_DEGREES);
    movePieces();
}

void VrScreen::endDrag() {
    if (!_dragging) {
        return;
    }
    _dragging = false;
    _dragTimer.stop();
    // it turns to face you where you are now, and stays
    updateHead();
    movePieces();
    tellState();
}

void VrScreen::tellState() {
    DependencyManager::get<Panels>()->setScreenState(isParked(_pitch) ? "parked" : "down");
}
