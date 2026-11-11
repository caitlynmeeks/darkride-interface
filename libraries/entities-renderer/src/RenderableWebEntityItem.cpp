//
//  Created by Bradley Austin Davis on 2015/05/12
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderableWebEntityItem.h"
#include <atomic>

#include <QtCore/QTimer>
#include <QtGui/QOpenGLContext>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/QPointingDevice>
#else
#include <QtGui/QTouchDevice>
#endif
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQml/QQmlContext>


#include <GeometryCache.h>
#include <material-networking/TextureCache.h>
#include <PathUtils.h>
#include <PointerEvent.h>
#include <gl/GLHelpers.h>
#include <ui/OffscreenQmlSurface.h>
#include <ui/TabletScriptingInterface.h>
#include <EntityScriptingInterface.h>
#include <shared/LocalFileAccessGate.h>

#include "EntitiesRendererLogging.h"
#include <NetworkingConstants.h>

using namespace render;
using namespace render::entities;

const QString WebEntityRenderer::QML = "Web3DSurface.qml";
const char* WebEntityRenderer::URL_PROPERTY = "url";

std::function<void(QString, bool, bool, QSharedPointer<OffscreenQmlSurface>&, bool&)> WebEntityRenderer::_acquireWebSurfaceOperator = nullptr;
std::function<void(QSharedPointer<OffscreenQmlSurface>&, bool&, std::vector<QMetaObject::Connection>&)> WebEntityRenderer::_releaseWebSurfaceOperator = nullptr;

static int MAX_WINDOW_SIZE = 4096;
const float METERS_TO_INCHES = 39.3701f;
static float OPAQUE_ALPHA_THRESHOLD = 0.999f;

// If a web-view hasn't been rendered for 30 seconds, de-allocate the framebuffer
static uint64_t MAX_NO_RENDER_INTERVAL = 30 * USECS_PER_SECOND;

// Don't allow more than 20 concurrent web views
static std::atomic<uint32_t> _currentWebCount(0);
static const uint32_t MAX_CONCURRENT_WEB_VIEWS = 20;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// Qt 6: a pointing device is made with its properties, once the application exists (it registers itself)
static OffscreenTouchDevice& touchDevice() {
    static QPointingDevice* device = new QPointingDevice("WebEntityRendererTouchDevice", 0, QInputDevice::DeviceType::TouchScreen,
                                                        QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 4, 0);
    return *device;
}
#else
static QTouchDevice _touchDevice;
static QTouchDevice& touchDevice() { return _touchDevice; }
#endif

QStringList WebEntityRenderer::_trustedQmlUrls;
QSet<QUuid> WebEntityRenderer::_appSurfaces;

void WebEntityRenderer::setAppSurface(const QUuid& entityID, bool appSurface) {
    if (appSurface) {
        _appSurfaces.insert(entityID);
    } else {
        _appSurfaces.remove(entityID);
    }
}

// a web page's address: what a Web entity that is not the app's own may show
static bool isWebPageUrl(const QString& urlString) {
    const QString scheme = QUrl(urlString).scheme().toLower();
    return scheme == HIFI_URL_SCHEME_HTTP || scheme == HIFI_URL_SCHEME_HTTPS || scheme == URL_SCHEME_DATA ||
           scheme == HIFI_URL_SCHEME_ABOUT;
}

WebEntityRenderer::ContentType WebEntityRenderer::getContentType(const QString& urlString) {
    if (urlString.isEmpty()) {
        return ContentType::NoContent;
    }

    const QUrl url(urlString);
    auto path = url.path().toLower();
    auto scheme = url.scheme();

    if (
        scheme == HIFI_URL_SCHEME_ABOUT ||
        scheme == HIFI_URL_SCHEME_HTTP ||
        scheme == HIFI_URL_SCHEME_HTTPS ||
        scheme == URL_SCHEME_DATA ||
        path.endsWith(".html") ||
        path.endsWith(".htm")
    ) {
        return ContentType::HtmlContent;
    }

    return ContentType::QmlContent;
}

WebEntityRenderer::WebEntityRenderer(const EntityItemPointer& entity) : Parent(entity) {
    static std::once_flag once;
    std::call_once(once, [&]{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        _touchDevice.setCapabilities(QTouchDevice::Position);
        _touchDevice.setType(QTouchDevice::TouchScreen);
        _touchDevice.setName("WebEntityRendererTouchDevice");
        _touchDevice.setMaximumTouchPoints(4);
#endif
    });
    _geometryId = DependencyManager::get<GeometryCache>()->allocateID();

    _texture = gpu::Texture::createExternal(OffscreenQmlSurface::getDiscardLambda());
    _texture->setSource(__FUNCTION__);
    // so a material can show this page: "albedoMap": "{this entity's id}" (one page on many screens)
    Texture::registerEntityTexture(entity->getID(), _texture);

    _contentType = ContentType::HtmlContent;
    // The app's own QML surfaces (the VR screen's pieces, listed by setAppSurface before their renderer is made) get theirs
    // at the first update: a placeholder web view made here for them was only thrown away there, and at the VR screen's
    // first show (an empty web-view cache) it made two new web views, 2.6 ms each on the main thread, plus the empty
    // address's "Invalid empty URL" at every show
    if (!_appSurfaces.contains(entity->getID())) {
        buildWebSurface(entity, "");
    }

    _timer.setInterval(MSECS_PER_SECOND);
    connect(&_timer, &QTimer::timeout, this, &WebEntityRenderer::onTimeout);
}

WebEntityRenderer::~WebEntityRenderer() {
    Texture::unregisterEntityTexture(_entity->getID(), _texture);
    destroyWebSurface();

    auto geometryCache = DependencyManager::get<GeometryCache>();
    if (geometryCache) {
        geometryCache->releaseID(_geometryId);
    }
}

bool WebEntityRenderer::isTransparent() const {
    float fadeRatio = _isFading ? Interpolate::calculateFadeRatio(_fadeStartTime) : 1.0f;
    return _transparentBackground || fadeRatio < OPAQUE_ALPHA_THRESHOLD || _alpha < 1.0f || _pulseProperties.getAlphaMode() != PulseMode::NONE;
}

bool WebEntityRenderer::needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const {
    if (resultWithReadLock<bool>([&] {
        if (_webSurface && uvec2(getWindowSize(entity)) != toGlm(_webSurface->size())) {
            return true;
        }

        if (_contextPosition != entity->getWorldPosition()) {
            return true;
        }

        return false;
    })) {
        return true;
    }

    return false;
}

bool WebEntityRenderer::needsRenderUpdate() const {
    if (resultWithReadLock<bool>([this] {
        return !_webSurface;
    })) {
        return true;
    }

    return Parent::needsRenderUpdate();
}

void WebEntityRenderer::onTimeout() {
    uint64_t lastRenderTime;
    if (!resultWithReadLock<bool>([&] {
        lastRenderTime = _lastRenderTime;
        return (_lastRenderTime != 0 && (bool)_webSurface);
    })) {
        return;
    }

    if (usecTimestampNow() - lastRenderTime > MAX_NO_RENDER_INTERVAL) {
        destroyWebSurface();
    }
}

void WebEntityRenderer::doRenderUpdateSynchronousTyped(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) {
    // If the content type has changed, or the old content type was QML, we need to
    // destroy the existing surface (because surfaces don't support changing the root
    // object, so subsequent loads of content just overlap the existing content
    bool urlChanged = false;
    auto newSourceURL = entity->getSourceUrl();
    {
        auto newContentType = getContentType(newSourceURL);
        // The app's own Web entities are the local ones its scripts made (a client script's thread; the tablet) and
        // the local ones it makes from C++ for its own QML surfaces (the login dialog, the avatar inputs bar, the
        // bubble icon, listed by the app at start). Every other Web entity, a world's, another avatar's, or a local
        // one a world's entity script made, shows a web page (http, https, data, about) or nothing: not QML, the
        // app's own UI language with the app's objects in reach, and not a page from the visitor's own files. QML only
        // on an entity the app made: only the app may show its own interface.
        const bool trusted = entity->isLocalEntity() && !entity->isMadeByWorld() &&
            (entity->getLocalSafeContext() || _trustedQmlUrls.contains(newSourceURL));
        const bool refused = !trusted && newContentType != ContentType::NoContent && !isWebPageUrl(newSourceURL);
        if (refused) {
            newContentType = ContentType::NoContent;
        }
        ContentType currentContentType;
        withReadLock([&] {
            urlChanged = newSourceURL.isEmpty() || newSourceURL != _tryingToBuildURL;
        });
        currentContentType = _contentType;

        if (urlChanged) {
            if (refused) {
                qCWarning(entitiesrenderer) << "Web entity" << entity->getID() << "is not the app's own; only a web page is shown, not:" << newSourceURL;
            }
            if (newContentType != ContentType::HtmlContent || currentContentType != ContentType::HtmlContent) {
                destroyWebSurface();
            }
            _contentType = newContentType;
        }
    }

    withWriteLock([&] {
        _inputMode = entity->getInputMode();
        _dpi = entity->getDPI();
        _color = entity->getColor();
        _alpha = entity->getAlpha();
        _pulseProperties = entity->getPulseProperties();
        _billboardMode = entity->getBillboardMode();

        if (_contentType == ContentType::NoContent) {
            _tryingToBuildURL = newSourceURL;
            _sourceURL = newSourceURL;
            return;
        }

        // This work must be done on the main thread
        bool localSafeContext = entity->getLocalSafeContext();
        if (!_webSurface) {
            if (localSafeContext) {
                ::hifi::scripting::setLocalAccessSafeThread(true);
            }
            buildWebSurface(entity, newSourceURL);
            ::hifi::scripting::setLocalAccessSafeThread(false);
        }

        if (_webSurface) {
            if (_webSurface->getRootItem()) {
                if (_contentType == ContentType::HtmlContent && _sourceURL != newSourceURL) {
                    if (localSafeContext) {
                        ::hifi::scripting::setLocalAccessSafeThread(true);
                    }
                    _webSurface->getRootItem()->setProperty(URL_PROPERTY, newSourceURL);
                    ::hifi::scripting::setLocalAccessSafeThread(false);
                    _sourceURL = newSourceURL;
                } else if (_contentType != ContentType::HtmlContent) {
                    _sourceURL = newSourceURL;
                }

                {
                    auto scriptURL = entity->getScriptURL();
                    if (_scriptURL != scriptURL) {                
                        _webSurface->getRootItem()->setProperty("scriptURL", scriptURL);
                        _scriptURL = scriptURL;
                    }
                }

                {
                    auto transparentBackground = entity->getTransparentBackground();
                    if (_transparentBackground != transparentBackground) {
                        _webSurface->getRootItem()->setProperty("transparentBackground", false); // otherwise it doesnt always update
                        _webSurface->getRootItem()->setProperty("transparentBackground", transparentBackground);
                        _transparentBackground = transparentBackground;
                    }
                }

                {
                    auto maxFPS = entity->getMaxFPS();
                    if (_maxFPS != maxFPS) {
                        _webSurface->setMaxFps(maxFPS);
                        _maxFPS = maxFPS;
                    }
                }

                {
                    auto contextPosition = entity->getWorldPosition();
                    if (_contextPosition != contextPosition) {
                        // not on the app's own surfaces: nothing there reads it, and a surface carried on a ride would
                        // write it every frame and sync every frame (performance, the VR screen's timing run)
                        if (_countedWebView) {
                            _webSurface->getSurfaceContext()->setContextProperty("globalPosition", vec3toVariant(contextPosition));
                        }
                        _contextPosition = contextPosition;
                    }
                }

                {
                    auto userData = entity->getUserData();
                    if (_userData != userData) {
                        _userData = userData;

                        auto userDataObject = QJsonDocument::fromJson(userData.toUtf8()).object();
                        _sideBySide = userDataObject.contains("sbs") ?
                            userDataObject["sbs"].toBool() : false;
                    }
                }
            }

            void* key = (void*)this;
            AbstractViewStateInterface::instance()->pushPostUpdateLambda(key, [this, entity] {
                withWriteLock([&] {
                    glm::vec2 windowSize = getWindowSize(entity);
                    _webSurface->resize(QSize(windowSize.x, windowSize.y));
                    _renderTransform = getModelTransform();
                    _renderTransform.setScale(1.0f);
                    _renderTransform.postScale(entity->getScaledDimensions());
                });
            });
        }
    });
}

Item::Bound WebEntityRenderer::getBound() {
    auto bound = Parent::getBound();
    if (_billboardMode != BillboardMode::NONE) {
        glm::vec3 dimensions = bound.getScale();
        float max = glm::max(dimensions.x, glm::max(dimensions.y, dimensions.z));
        const float SQRT_2 = 1.41421356237f;
        bound.setScaleStayCentered(glm::vec3(SQRT_2 * max));
    }
    return bound;
}

void WebEntityRenderer::doRender(RenderArgs* args) {

    // const bool hasChanged = evaluateEntityZoneCullState(_entity);

    PerformanceTimer perfTimer("WebEntityRenderer::render");
    withWriteLock([&] {
        _lastRenderTime = usecTimestampNow();
    });

    // Try to update the texture
    OffscreenQmlSurface::TextureAndFence newTextureAndFence;
    bool newTextureAvailable = false;
    if (!resultWithReadLock<bool>([&] {
        if (!_webSurface) {
            return false;
        }

        newTextureAvailable = _webSurface->fetchTexture(newTextureAndFence);
        return true;
    })) {
        return;
    }

    if (newTextureAvailable) {
        _texture->setExternalTexture(newTextureAndFence.first, newTextureAndFence.second);
    }

    static const glm::vec2 texMin(0.0f), texMax(1.0f), topLeft(-0.5f), bottomRight(0.5f);

    gpu::Batch& batch = *args->_batch;
    glm::vec4 color;
    Transform transform;
    bool forward;
    bool transparent;
    withReadLock([&] {
        float fadeRatio = _isFading ? Interpolate::calculateFadeRatio(_fadeStartTime) : 1.0f;
        color = glm::vec4(toGlm(_color), _alpha * fadeRatio);
        color = EntityRenderer::calculatePulseColor(color, _pulseProperties, _created);
        transform = _renderTransform;
        forward = _renderLayer != RenderLayer::WORLD || args->_renderMethod == render::Args::FORWARD;
        transparent = isTransparent();
    });

    if (color.a == 0.0f) {
        return;
    }

    batch.setResourceTexture(0, _texture);

    applyBillboard(transform, _billboardMode, args);   // turned to the camera; mirrored with the selfie picture
    batch.setModelTransform(transform);

    // Turn off jitter for these entities
    batch.pushProjectionJitter();
    DependencyManager::get<GeometryCache>()->bindWebBrowserProgram(batch, transparent, forward, _sideBySide);
    DependencyManager::get<GeometryCache>()->renderQuad(batch, topLeft, bottomRight, texMin, texMax, color, _geometryId);
    batch.popProjectionJitter();
    batch.setResourceTexture(0, nullptr);
}

void WebEntityRenderer::buildWebSurface(const EntityItemPointer& entity, const QString& newSourceURL) {
    
    const bool appSurface = _appSurfaces.contains(entity->getID());
    auto entityTreeRenderer = DependencyManager::get<EntityTreeRenderer>();
    if (!appSurface && entityTreeRenderer->getDisableWebEntities()) return;

    if (!appSurface && _currentWebCount >= MAX_CONCURRENT_WEB_VIEWS) {
        qWarning() << "Too many concurrent web views to create new view";
        return;
    }

    _countedWebView = !appSurface;
    if (_countedWebView) {
        ++_currentWebCount;
    }
    const bool appOwned = entity->isLocalEntity() && !entity->isMadeByWorld();
    WebEntityRenderer::acquireWebSurface(newSourceURL, _contentType == ContentType::HtmlContent, appOwned, _webSurface, _cachedWebSurface);
    _fadeStartTime = usecTimestampNow();
    _webSurface->resume();

    _connections.push_back(QObject::connect(this, &WebEntityRenderer::scriptEventReceived, _webSurface.data(), &OffscreenQmlSurface::emitScriptEvent));
    _connections.push_back(QObject::connect(this, &WebEntityRenderer::sendToQmlReceived, _webSurface.data(), &OffscreenQmlSurface::sendToQml));

    const EntityItemID entityItemID = entity->getID();
    
    _connections.push_back(QObject::connect(_webSurface.data(), &OffscreenQmlSurface::webEventReceived, this, &WebEntityRenderer::webEventReceived));
    _connections.push_back(QObject::connect(_webSurface.data(), &OffscreenQmlSurface::webEventReceived, this, [entityItemID](const QVariant& message) {
        emit DependencyManager::get<EntityScriptingInterface>()->webEventReceived(entityItemID, message);
    }));

    _connections.push_back(QObject::connect(_webSurface.data(), &OffscreenQmlSurface::fromQml, this, &WebEntityRenderer::fromQml));
    _connections.push_back(QObject::connect(_webSurface.data(), &OffscreenQmlSurface::fromQml, this, [entityItemID](const QVariant& message) {
        emit DependencyManager::get<EntityScriptingInterface>()->fromQml(entityItemID, message);
    }));

    _tryingToBuildURL = newSourceURL;
}

void WebEntityRenderer::destroyWebSurface() {
    QSharedPointer<OffscreenQmlSurface> webSurface;
    withWriteLock([&] {
        webSurface.swap(_webSurface);
        _contentType = ContentType::NoContent;

        if (webSurface) {
            if (_countedWebView) {
                --_currentWebCount;
                _countedWebView = false;
            }
            WebEntityRenderer::releaseWebSurface(webSurface, _cachedWebSurface, _connections);
        }
    });
}

glm::vec2 WebEntityRenderer::getWindowSize(const TypedEntityPointer& entity) const {
    glm::vec2 dims = glm::vec2(entity->getScaledDimensions());
    dims *= METERS_TO_INCHES * _dpi;

    // ensure no side is never larger then MAX_WINDOW_SIZE
    float max = (dims.x > dims.y) ? dims.x : dims.y;
    if (max > MAX_WINDOW_SIZE) {
        dims *= MAX_WINDOW_SIZE / max;
    }

    return dims;
}

void WebEntityRenderer::hoverEnterEntity(const PointerEvent& event) {
    if (_inputMode == WebInputMode::MOUSE) {
        handlePointerEvent(event);
        return;
    }

    withReadLock([&] {
        if (_webSurface) {
            PointerEvent webEvent = event;
            webEvent.setPos2D(event.getPos2D() * (METERS_TO_INCHES * _dpi));
            _webSurface->hoverBeginEvent(webEvent, touchDevice());
        }
    });
}

void WebEntityRenderer::hoverLeaveEntity(const PointerEvent& event) {
    if (_inputMode == WebInputMode::MOUSE) {
        PointerEvent endEvent(PointerEvent::Release, event.getID(), event.getPos2D(), event.getPos3D(), event.getNormal(), event.getDirection(),
            event.getButton(), event.getButtons(), event.getKeyboardModifiers());
        handlePointerEvent(endEvent);
        // QML onReleased is only triggered if a click has happened first.  We need to send this "fake" mouse move event to properly trigger an onExited.
        PointerEvent endMoveEvent(PointerEvent::Move, event.getID());
        handlePointerEvent(endMoveEvent);
        return;
    }

    withReadLock([&] {
        if (_webSurface) {
            PointerEvent webEvent = event;
            webEvent.setPos2D(event.getPos2D() * (METERS_TO_INCHES * _dpi));
            _webSurface->hoverEndEvent(webEvent, touchDevice());
        }
    });
}

void WebEntityRenderer::handlePointerEvent(const PointerEvent& event) {
    if (_inputMode == WebInputMode::NONE) return;
    
    withReadLock([&] {
        if (!_webSurface) return;

        if (_inputMode == WebInputMode::TOUCH) {
            handlePointerEventAsTouch(event);
        } else {
            handlePointerEventAsMouse(event);
        }
    });
}

void WebEntityRenderer::handlePointerEventAsTouch(const PointerEvent& event) {
    PointerEvent webEvent = event;
    webEvent.setPos2D(event.getPos2D() * (METERS_TO_INCHES * _dpi));
    _webSurface->handlePointerEvent(webEvent, touchDevice());
}

void WebEntityRenderer::handlePointerEventAsMouse(const PointerEvent& event) {
    glm::vec2 windowPos = event.getPos2D() * (METERS_TO_INCHES * _dpi);
    QPointF windowPoint(windowPos.x, windowPos.y);

    Qt::MouseButtons buttons = Qt::NoButton;
    if (event.getButtons() & PointerEvent::PrimaryButton) {
        buttons |= Qt::LeftButton;
    }

    Qt::MouseButton button = Qt::NoButton;
    if (event.getButton() == PointerEvent::PrimaryButton) {
        button = Qt::LeftButton;
    }

    QEvent::Type type;
    switch (event.getType()) {
        case PointerEvent::Press:
            type = QEvent::MouseButtonPress;
            break;
        case PointerEvent::Release:
            type = QEvent::MouseButtonRelease;

            // you cant release "no button" or it will never click
            // this happens in vr and needs to be fixed elsewhere i think
            if (button == Qt::NoButton)
                button = Qt::LeftButton;

            break;
        case PointerEvent::Move:
            type = QEvent::MouseMove;
            break;
        default:
            return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // as the surface's own mouse device, never as the real mouse (OffscreenQmlSurface::mouseDevice)
    QMouseEvent mouseEvent(type, windowPoint, windowPoint, windowPoint, button, buttons, event.getKeyboardModifiers(),
                           _webSurface->mouseDevice());
#else
    QMouseEvent mouseEvent(type, windowPoint, windowPoint, windowPoint, button, buttons, event.getKeyboardModifiers());
#endif
    QCoreApplication::sendEvent(_webSurface->getWindow(), &mouseEvent);
}

void WebEntityRenderer::setProxyWindow(QWindow* proxyWindow) {
    withReadLock([&] {
        if (_webSurface) {
            _webSurface->setProxyWindow(proxyWindow);
        }
    });
}

QObject* WebEntityRenderer::getEventHandler() {
    return resultWithReadLock<QObject*>([&]() -> QObject* {
        if (!_webSurface) {
            return nullptr;
        }
        return _webSurface->getEventHandler();
    });
}

void WebEntityRenderer::emitScriptEvent(const QVariant& message) {
    emit scriptEventReceived(message);
}

void WebEntityRenderer::sendToQml(const QVariant& message) {
    emit sendToQmlReceived(message);
}
