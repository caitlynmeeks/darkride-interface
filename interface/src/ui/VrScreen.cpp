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

#include <memory>

#include <QtCore/QThread>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <AbstractViewStateInterface.h>

#include <EntityScriptingInterface.h>
#include <EntityTreeRenderer.h>
#include <GLMHelpers.h>
#include <NumericalConstants.h>
#include <RenderableWebEntityItem.h>
#include <shared/LocalFileAccessGate.h>
#include <SharedUtil.h>
#include <SpatiallyNestable.h>
#include <controllers/Actions.h>
#include <ui/OffscreenQmlSurface.h>

#include "Application.h"
#include "InterfaceLogging.h"
#include "avatar/AvatarManager.h"
#include "avatar/MyAvatar.h"
#include "scripting/HMDScriptingInterface.h"
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
// A surface only redraws when its page changes, but each redraw syncs on the main thread: the panel you use gets the
// tablet's rate, and the side panels and the bar a rate that costs next to nothing while they animate (timing run, 11-01)
const int MIDDLE_MAX_FPS = 90;
const int SIDE_MAX_FPS = 30;
const int DRAG_INTERVAL_MS = 11;     // about 90 Hz, only while the bar is held
const int REMAKE_DELAY_MS = 500;
// The warm-up (the surface pool; performance's placement, 11-01 20:3x). At headset entry it runs back to back, a step
// per event-loop turn, inside the entry's own gap: the runtime shows its transition until our first frame. A step makes
// one surface (~7-19 ms on the main thread, its first sync ~35 ms follows on its own) or one page (editor-ui's, ~50-70
// ms). What comes later (the pages after sign-in, the radio's slot) is staggered instead, one step per WARM_STEP_MS, and
// never while the avatar rides or stands in a moving frame (the vehicle rule: nothing new shares a ride's frame).
const int WARM_START_DELAY_MS = 0;       // the next turn after entering
const int WARM_STEP_MS = 150;
const int WARM_RETRY_MS = 500;           // on a ride or moving: wait until off it and still
// The settle step: time for the draws (the pages' first sync in their slots) before the surfaces are paused, in ticks.
// Nothing shows a kept surface's picture while the screen is hidden, and a surface draws again only once its last
// picture has been taken (SharedObject::onTimer: one frame queued at a time), so each tick but the last two takes it and
// gives it back to the texture cache; the last 200 ms leave the newest picture, pages and all, for the first show.
const int WARM_SETTLE_TICK_MS = 100;
const int WARM_SETTLE_TICKS = 5;         // 500 ms
const float WARM_MOVING_SPEED = 0.3f;    // m/s
const uint64_t MIN_TOGGLE_INTERVAL_USECS = 300 * USECS_PER_MSEC;

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

// tests: DARKRIDE_VR_SCREEN_WARM_DELAY_MS puts the warm-up where a timing run can see it, in a settled world
int warmStartDelay() {
    static const int value = [] {
        bool ok = false;
        const int fromEnvironment = qEnvironmentVariable("DARKRIDE_VR_SCREEN_WARM_DELAY_MS").toInt(&ok);
        return ok && fromEnvironment >= 0 ? fromEnvironment : WARM_START_DELAY_MS;
    }();
    return value;
}

glm::vec2 toVec2(const QSize& size) {
    return glm::vec2(size.width(), size.height());
}

}   // namespace

bool VrScreen::isOn() {
    static const bool onInDesktop = qEnvironmentVariableIsSet("DARKRIDE_VR_SCREEN");
    return onInDesktop || qApp->isHMDMode();
}

VrScreen::VrScreen() {
    _dragTimer.setInterval(DRAG_INTERVAL_MS);
    connect(&_dragTimer, &QTimer::timeout, this, &VrScreen::dragTick);
    _warmTimer.setSingleShot(true);
    connect(&_warmTimer, &QTimer::timeout, this, &VrScreen::warmStep);
    // entering and leaving the headset (the warm-up, and hiding as it comes off); a Desktop test warms at its first
    // display. Connected now, as the app is made: the first display starts before anything shows the screen.
    connect(qApp, &Application::activeDisplayPluginChanged, this, &VrScreen::displayChanged);
    // the kept surfaces go before QML stops, as any hidden piece's would (connected before any surface's own quit)
    connect(qApp, &QCoreApplication::aboutToQuit, this, &VrScreen::stopKeeping);
    Panels::setScreenWindowCheck([](QQuickWindow* window) {
        auto screen = DependencyManager::get<VrScreen>();
        return screen && screen->ownsWindow(window);
    });
    Panels::setScreenShowInFront(
        [](bool dialogOnly) {
            if (auto screen = DependencyManager::get<VrScreen>()) {
                screen->showInFront(dialogOnly);
            }
        },
        [] {
            if (auto screen = DependencyManager::get<VrScreen>()) {
                screen->dialogOnlyDone();
            }
        });
    // HMD.openTablet and closeTablet: apps call them to bring up what they've just opened
    // A dialog-only screen (a world's dialog) isn't the tablet shown: an app's openTablet brings the whole screen then
    connect(DependencyManager::get<HMDScriptingInterface>().data(), &HMDScriptingInterface::showTabletChanged, this, [this](bool show) {
        if (!isOn()) {
            return;
        }
        if (show) {
            this->show();
        } else if (_shown && !_dialogOnly) {
            hide();
        }
    });
}

void VrScreen::connectOnce() {
    if (_connected) {
        return;
    }
    _connected = true;
    connect(DependencyManager::get<Panels>().data(), &Panels::screenSlotSizeChanged, this, &VrScreen::slotSizeChanged);
    connect(DependencyManager::get<Panels>().data(), &Panels::screenSlotFilled, this, &VrScreen::slotFilled);
    // the pages can't be made before sign-in: once Panels has restored, the ones still missing, staggered
    connect(DependencyManager::get<Panels>().data(), &Panels::restored, this, [this] {
        if (!_keeping) {
            return;
        }
        _warmPages = true;
        _warmAnnounce = true;
        // a warm-up still pending (the entry's) makes them in its own mode; otherwise staggered
        if (!_warmTimer.isActive()) {
            warmLater();
        }
    });
}

void VrScreen::toggle() {
    // a second guard, beside the rule on worlds' mappings: nothing (a stuck button, a mapping) can make the screen rebuild
    // its surfaces in a loop, on a ride or anywhere
    const uint64_t now = usecTimestampNow();
    if (now - _lastToggle < MIN_TOGGLE_INTERVAL_USECS) {
        return;
    }
    _lastToggle = now;
    // the menu button answers a dialog first (Cancel), as Escape does on the desktop; the screen stays
    auto panels = DependencyManager::get<Panels>();
    if (_shown && panels->cancelScreenDialog()) {
        return;
    }
    if (_shown) {
        hide();
    } else {
        show();
    }
}

void VrScreen::show() {
    if (_shown) {
        if (_dialogOnly) {
            showInFront(false);   // the whole screen, around the dialog that's up
        }
        return;
    }
    showPieces(false);
}

void VrScreen::showPieces(bool dialogOnly) {
    if (_shown) {
        return;
    }
    _dialogOnly = dialogOnly;
    connectOnce();
    // only while shown: a world change deletes thousands of entities, and none of them should reach a hidden screen
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    _entityConnections = {
        connect(entities.data(), &EntityScriptingInterface::deletingEntity, this, &VrScreen::entityDeleted),
        connect(entities.data(), &EntityScriptingInterface::mousePressOnEntity, this, &VrScreen::mousePressed),
        connect(entities.data(), &EntityScriptingInterface::mouseReleaseOnEntity, this, &VrScreen::mouseReleased)
    };
    faceHead();
    _shown = true;
    // what isn't warm yet is made now, as before, and kept at the next hide
    _warmTimer.stop();

    const Arrangement places = arrangement();
    makePiece(Middle, places);
    if (!dialogOnly) {
        makeOtherPieces(places);
    }
    tellState();
    if (!dialogOnly) {
        DependencyManager::get<HMDScriptingInterface>()->setShouldShowTablet(true);
    }
    qCDebug(interfaceapp) << "VR screen: shown, yaw" << _yaw << (dialogOnly ? "(dialog only)" : "");
}

void VrScreen::makeOtherPieces(const Arrangement& places) {
    makePiece(Left, places);
    if (DependencyManager::get<Panels>()->screenSlotSize(SLOT_RIGHT).isValid()) {
        makePiece(Right, places);
    }
    makePiece(Bar, places);
}

void VrScreen::showInFront(bool dialogOnly) {
    if (!isOn()) {
        return;   // nothing pops the screen on a plain desktop (editor-ui's guard, again here)
    }
    if (!_shown) {
        showPieces(dialogOnly);
        return;
    }
    endDrag();
    faceHead();
    movePieces();
    if (_dialogOnly && !dialogOnly) {
        _dialogOnly = false;
        makeOtherPieces(arrangement());
        DependencyManager::get<HMDScriptingInterface>()->setShouldShowTablet(true);
    }
    tellState();
}

void VrScreen::dialogOnlyDone() {
    if (_dialogOnly) {
        hide();
    }
}

void VrScreen::faceHead() {
    updateHead();
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    glm::quat orientation = avatar->getHMDSensorOrientation();
    if (!qApp->isHMDMode()) {
        // Desktop tests (DARKRIDE_VR_SCREEN): there's no head pose, so the camera stands in for it
        orientation = glm::inverse(glmExtractRotation(avatar->getSensorToWorldMatrix())) * qApp->getCamera().getOrientation();
    }
    _yaw = yawOf(orientation * glm::vec3(0.0f, 0.0f, -1.0f));
    _pitch = DEFAULT_PITCH_DEGREES;
}

void VrScreen::hide() {
    if (!_shown) {
        return;
    }
    endDrag();
    _shown = false;
    _dialogOnly = false;
    for (const QMetaObject::Connection& connection : _entityConnections) {
        disconnect(connection);
    }
    // the pages first. While the surfaces are kept (in the headset) the pages stay in their slots, unseen, so the next
    // show only resumes them; otherwise they go home before their hosts die
    if (_keeping) {
        DependencyManager::get<Panels>()->hideScreenSlots();
    } else {
        DependencyManager::get<Panels>()->releaseScreenSlots();
    }
    _changing = true;
    for (int piece = 0; piece < PIECE_COUNT; ++piece) {
        deletePiece((Piece)piece);
    }
    _changing = false;
    DependencyManager::get<Keyboard>()->setRaised(false);
    DependencyManager::get<HMDScriptingInterface>()->setShouldShowTablet(false);
    if (_keeping && (!_toWarm.isEmpty() || _warmLoaded || _warmDrawing || !_warmUnpaused.isEmpty())) {
        warmLater();
    }
    qCDebug(interfaceapp) << "VR screen: hidden";
}

bool VrScreen::ownsWindow(QQuickWindow* window) const {
    if (!window) {
        return false;
    }
    // a kept slot surface, hidden or shown: in the headset the pages stay in them while the screen is hidden. Only
    // surfaces VrScreen made are here, never a world's Web entity
    for (auto it = _surfaceUrls.cbegin(); it != _surfaceUrls.cend(); ++it) {
        if (Panels::isScreenSlotUrl(it.value()) && it.key()->getWindow() == window) {
            return true;
        }
    }
    if (!_shown) {
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
    const Pose pose = placedPiece(places, piece);
    const glm::mat4 sensorToWorld = avatar->getSensorToWorldMatrix();
    const float scale = avatar->getSensorToWorldScale();
    const PieceSpec spec = pieceSpec(piece, places);

    EntityItemProperties properties;
    properties.setType(EntityTypes::Web);
    properties.setName(spec.name);
    properties.setSourceUrl(spec.url);
    properties.setParentID(AVATAR_SELF_ID);
    properties.setParentJointIndex(SENSOR_TO_WORLD_MATRIX_INDEX);
    properties.setPosition(transformPoint(sensorToWorld, pose.position));
    properties.setRotation(glmExtractRotation(sensorToWorld) * pose.rotation);
    properties.setDimensions(glm::vec3(spec.metres, PIECE_THICKNESS * scale));
    properties.setDPI((uint16_t)spec.dpi);
    properties.setMaxFPS(spec.fps);
    properties.setPrimitiveMode(PrimitiveMode::SOLID);
    // the screen is the visitor's own, not part of the world: no shadow of it on the floor or a car (and no draw of it in
    // the shadow pass; an entity casts one by default)
    properties.setCanCastShadow(false);
    properties.getGrab().setGrabbable(false);
    properties.setIgnorePickIntersection(false);
    properties.setAlpha(1.0f);
    properties.setVisible(true);

    auto entities = DependencyManager::get<EntityScriptingInterface>();
    {
        AppMadeEntityScope appMade;
        _pieces[piece] = entities->addEntityInternal(properties, entity::HostType::LOCAL);
    }
    // before the renderer exists: it builds its surface at once, and it's made on a later update (addingEntity is queued)
    render::entities::WebEntityRenderer::setAppSurface(_pieces[piece], true);
}

VrScreen::PieceSpec VrScreen::pieceSpec(Piece piece, const Arrangement& places) const {
    static const char* NAMES[] = { "left", "middle", "right" };
    const Pose& arranged = piece == Left ? places.left : piece == Middle ? places.middle : piece == Right ? places.right : places.bar;
    const float scale = DependencyManager::get<AvatarManager>()->getMyAvatar()->getSensorToWorldScale();
    PieceSpec spec;
    glm::vec2 pixels = GRAB_BAR_PIXELS;
    spec.url = GRAB_BAR_URL;
    spec.name = "VR screen grab bar";
    if (piece != Bar) {
        pixels = glm::round(toVec2(DependencyManager::get<Panels>()->screenSlotSize(NAMES[piece])) * textureScale(degreesPerPixel()));
        spec.url = piece == Left ? SLOT_LEFT_URL : piece == Middle ? SLOT_MIDDLE_URL : SLOT_RIGHT_URL;
        spec.name = QString("VR screen ") + NAMES[piece];
    }
    spec.dpi = dpiForWidth(pixels.x, arranged.size.x * scale);
    spec.metres = metresForPixels(pixels, spec.dpi);
    spec.fps = piece == Middle ? MIDDLE_MAX_FPS : SIDE_MAX_FPS;
    return spec;
}

QSize VrScreen::surfaceSize(const PieceSpec& spec) {
    // as WebEntityRenderer::getWindowSize works it out from the entity (its metres x inches x whole-number dpi, the
    // longest side at most 4096), so a warm surface needs no new size, and no new layout, at the show
    const float MAX_WINDOW_SIZE = 4096.0f;
    glm::vec2 window = spec.metres;
    window *= METRES_TO_INCHES * (float)(uint16_t)spec.dpi;
    const float longest = std::max(window.x, window.y);
    if (longest > MAX_WINDOW_SIZE) {
        window *= MAX_WINDOW_SIZE / longest;
    }
    return QSize(window.x, window.y);
}

bool VrScreen::isPieceUrl(const QString& url) {
    return url == SLOT_LEFT_URL || url == SLOT_MIDDLE_URL || url == SLOT_RIGHT_URL || url == GRAB_BAR_URL;
}

QSharedPointer<OffscreenQmlSurface> VrScreen::makeSurface(const QString& url, bool warm) {
    // as the app's acquire operator makes its own QML surfaces (ApplicationStartup.cpp)
    QSharedPointer<OffscreenQmlSurface> surface(new OffscreenQmlSurface(), [](OffscreenQmlSurface* surface) {
        AbstractViewStateInterface::instance()->sendLambdaEvent([surface] {
            delete surface;
        });
    });
    QObject::connect(surface.data(), &hifi::qml::OffscreenSurface::rootContextCreated, [surface = surface.data(), warm] {
        Application::setupQmlSurface(surface->getSurfaceContext(), true);
        // made while hidden: the slot doesn't tell Panels it's ready (it would be refused); VrScreen does at the show
        surface->getSurfaceContext()->setContextProperty("screenSlotWarm", warm);
    });
    {
        AppMadeEntityScope appMade;
        surface->load(url);
    }
    _surfaceUrls.insert(surface.data(), url);
    return surface;
}

QSharedPointer<OffscreenQmlSurface> VrScreen::takeSurface(const QString& url) {
    QSharedPointer<OffscreenQmlSurface> surface = _kept.take(url);
    if (!surface) {
        // not warm yet (a show before the warm-up got to it, or the radio's slot): made now, kept after; its slot tells
        // Panels it's ready itself
        return makeSurface(url, false);
    }
    if (surface == _warmLoaded) {
        _warmLoaded.reset();
    }
    if (surface == _warmDrawing) {
        _warmDrawing.reset();
    }
    _warmUnpaused.removeAll(surface);
    // A slot tells Panels it's ready as its QML is made, which for a warm one was while the screen was hidden (refused).
    // It's the screen's now: Panels takes it on the next turn, by when the Web entity's renderer holds this surface.
    if (Panels::isScreenSlotUrl(url)) {
        auto announce = [](QQuickItem* root) {
            DependencyManager::get<Panels>()->screenSlotReady(root->property("slot").toString(), root);
        };
        if (QQuickItem* root = surface->getRootItem()) {
            announce(root);
        } else {
            // still loading (handed over the moment it was made): once its root item is there
            auto once = std::make_shared<QMetaObject::Connection>();
            *once = QObject::connect(surface.data(), &hifi::qml::OffscreenSurface::rootItemCreated, this, [once, announce](QQuickItem* root) {
                QObject::disconnect(*once);
                announce(root);
            });
        }
    }
    return surface;
}

bool VrScreen::keepSurface(const QSharedPointer<OffscreenQmlSurface>& surface) {
    auto found = _surfaceUrls.find(surface.data());
    if (found == _surfaceUrls.end()) {
        return false;
    }
    const QString url = found.value();
    // out of the headset, a second one for the same address, or not on the main thread: deleted as any other
    if (!_keeping || _kept.contains(url) || QThread::currentThread() != thread()) {
        _surfaceUrls.erase(found);
        return false;
    }
    surface->pause();
    _kept.insert(url, surface);
    return true;
}

void VrScreen::startWarming() {
    connectOnce();
    _keeping = true;
    _warmAnnounce = true;
    _toWarm = { Middle, Left, Bar, Right };
    _warmBackToBack = true;
    _warmPages = true;
    _warmTimer.start(warmStartDelay());
}

void VrScreen::warmLater() {
    // staggered from here on; the timer may be running already
    _warmBackToBack = false;
    _warmAnnounce = true;
    if (_keeping && !_shown && !_warmTimer.isActive()) {
        _warmTimer.start(WARM_STEP_MS);
    }
}

bool VrScreen::warmMustWait() const {
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    return !avatar->getParentID().isNull() || glm::length(avatar->getWorldVelocity()) > WARM_MOVING_SPEED;
}

void VrScreen::warmStep() {
    if (!_keeping || _shown) {
        return;
    }
    if (_warmDrawing) {
        _warmDrawing->pause();
        _warmDrawing.reset();
    }
    if (!_warmBackToBack && warmMustWait()) {
        _warmTimer.start(WARM_RETRY_MS);
        return;
    }
    const int step = _warmBackToBack ? 0 : WARM_STEP_MS;
    if (_warmLoaded) {
        // its render thread has started up by now, off the main thread: its first draw (the main thread waits for the
        // first sync only)
        qCDebug(interfaceapp) << "VR screen: warm-up: first draw";
        _warmLoaded->setMaxFps(_warmFps);
        _warmDrawing = _warmLoaded;
        _warmLoaded.reset();
        _warmTimer.start(step);
        return;
    }
    const Arrangement places = arrangement();
    while (!_toWarm.isEmpty()) {
        const Piece piece = _toWarm.takeFirst();
        if (piece == Right && !DependencyManager::get<Panels>()->screenSlotSize(SLOT_RIGHT).isValid()) {
            continue;   // the radio's off: made if it's on at a show, kept after
        }
        const PieceSpec spec = pieceSpec(piece, places);
        if (_kept.contains(spec.url)) {
            continue;
        }
        qCDebug(interfaceapp) << "VR screen: warm-up: making" << spec.name << surfaceSize(spec)
                              << (_warmBackToBack ? "(at entry)" : "(staggered)");
        auto surface = makeSurface(spec.url, true);
        surface->resize(surfaceSize(spec));
        _kept.insert(spec.url, surface);
        if (_warmBackToBack) {
            // drawn as soon as its render thread is up; paused once they've all had time (the settle step)
            surface->setMaxFps(spec.fps);
            _warmUnpaused.append(surface);
        } else {
            surface->setMaxFps(0);   // no draw while its render thread starts up: the next step allows it
            _warmLoaded = surface;
            _warmFps = spec.fps;
        }
        _warmTimer.start(step);
        return;
    }
    // then the pages the first show will move in, one per step (editor-ui's: People, then the middle's front panel),
    // so the first show doesn't make them (~50-70 ms each). None before sign-in: Panels' restore brings them later
    if (_warmPages && DependencyManager::get<Panels>()->makeNextScreenPage()) {
        _warmTimer.start(step);
        return;
    }
    _warmPages = false;
    if (_warmAnnounce) {
        // the kept slots to Panels, which fills them with their pages on the next turn (screenSlotFilled): their first
        // sync with the pages happens now, before the settle step pauses them
        _warmAnnounce = false;
        auto panels = DependencyManager::get<Panels>();
        for (auto it = _kept.cbegin(); it != _kept.cend(); ++it) {
            QQuickItem* root = Panels::isScreenSlotUrl(it.key()) ? it.value()->getRootItem() : nullptr;
            if (root) {
                panels->screenSlotReady(root->property("slot").toString(), root);
                // drawing until the settle step, so the page moved in syncs now (a paused surface wouldn't until the show)
                it.value()->resume();
                if (!_warmUnpaused.contains(it.value())) {
                    _warmUnpaused.append(it.value());
                }
            }
        }
        startSettling();
        return;
    }
    if (!_warmUnpaused.isEmpty()) {
        if (!_warmSettling) {
            startSettling();
            return;
        }
        if (--_warmSettleTicks > 0) {
            if (_warmSettleTicks > 1) {
                takeWarmPictures();
            }
            _warmTimer.start(WARM_SETTLE_TICK_MS);
            return;
        }
        for (const auto& surface : _warmUnpaused) {
            if (surface && _kept.values().contains(surface)) {
                surface->pause();
            }
        }
        _warmUnpaused.clear();
    }
    _warmSettling = false;
    qCDebug(interfaceapp) << "VR screen: surfaces warm:" << _kept.size();
}

void VrScreen::slotFilled(const QString& slot) {
    if (_shown || !_keeping) {
        return;
    }
    const QString url = slot == "left" ? SLOT_LEFT_URL : slot == "middle" ? SLOT_MIDDLE_URL : slot == SLOT_RIGHT ? SLOT_RIGHT_URL : QString();
    auto surface = _kept.value(url);
    if (!surface) {
        return;
    }
    // a page moved into a hidden slot: drawn once now, so its first sync there isn't at the show, then paused again
    surface->resume();
    if (!_warmUnpaused.contains(surface)) {
        _warmUnpaused.append(surface);
    }
    startSettling();
}

void VrScreen::startSettling() {
    _warmSettling = true;
    _warmSettleTicks = WARM_SETTLE_TICKS;
    takeWarmPictures();
    _warmTimer.start(WARM_SETTLE_TICK_MS);
}

void VrScreen::takeWarmPictures() {
    // the pictures the drawing kept surfaces made, which nothing shows while the screen is hidden: back to the texture
    // cache, so they draw again (OffscreenSurface::getDiscardLambda is the Web entity renderer's own way to give one back)
    static const auto giveBack = OffscreenQmlSurface::getDiscardLambda();
    for (const auto& surface : _warmUnpaused) {
        OffscreenQmlSurface::TextureAndFence picture;
        if (surface && surface->fetchTexture(picture)) {
            giveBack(picture.first, picture.second);
        }
    }
}

void VrScreen::stopKeeping() {
    // the pages kept in the slots go home before their surfaces go (the headset off, or quitting)
    if (_keeping) {
        if (auto panels = DependencyManager::get<Panels>()) {
            panels->releaseScreenSlots();
        }
    }
    _keeping = false;
    _warmAnnounce = false;
    _warmTimer.stop();
    _toWarm.clear();
    _warmLoaded.reset();
    _warmDrawing.reset();
    _warmUnpaused.clear();
    _warmSettling = false;
    _warmPages = false;
    for (auto it = _kept.cbegin(); it != _kept.cend(); ++it) {
        _surfaceUrls.remove(it.value().data());
    }
    _kept.clear();   // their deleter deletes them on a later turn
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
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    _head = qApp->isHMDMode() ? avatar->getHMDSensorPosition()
        : transformPoint(glm::inverse(avatar->getSensorToWorldMatrix()), qApp->getCamera().getPosition());
}

void VrScreen::slotSizeChanged(const QString& slot) {
    if (slot != SLOT_RIGHT) {
        return;
    }
    if (!_shown) {
        // the radio turned on while the screen's hidden: its slot's surface is made ahead too, staggered
        if (_keeping && DependencyManager::get<Panels>()->screenSlotSize(SLOT_RIGHT).isValid() && !_kept.contains(SLOT_RIGHT_URL) &&
            !_toWarm.contains(Right)) {
            _toWarm.append(Right);
            warmLater();
        }
        return;
    }
    if (_dialogOnly) {
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
        stopKeeping();
    }
    _wasHeadset = headset;
    if (!_keeping && isOn()) {
        startWarming();
    }
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
