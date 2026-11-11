//
//  KeyboardFocus.cpp
//  interface/src
//
//  Which entity has the keyboard, and the highlight drawn around it (see KeyboardFocus.h). The code of
//  Application's setKeyboardFocusHighlight, setKeyboardFocusEntity and handleKeyEventForFocusedEntity, of the frame
//  loop's follow of the focused entity and of the shutdown's deletion of the highlight, with Application reached
//  through qApp for the window, the entities, the keyboard-mouse device and the sign-in dialog.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "KeyboardFocus.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QThread>

#include <EntityScriptingInterface.h>
#include <MainWindow.h>
#include <RenderableEntityItem.h>
#include <SharedUtil.h>
#include <input-plugins/KeyboardMouseDevice.h>

#include "Application.h"

static const float HIGHLIGHT_EXPANSION_FACTOR = 1.05f;   // the highlight's size relative to the entity's

void KeyboardFocus::highlight(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& dimensions) {
    if (qApp->getLoginDialogPoppedUp()) {
        return;
    }

    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    if (_highlightID == UNKNOWN_ENTITY_ID || !entityScriptingInterface->isAddedEntity(_highlightID)) {
        EntityItemProperties properties;
        properties.setType(EntityTypes::Box);
        properties.setAlpha(1.0f);
        properties.setColor({ 0xFF, 0xEF, 0x00 });
        properties.setPrimitiveMode(PrimitiveMode::LINES);
        properties.getPulse().setMin(0.5);
        properties.getPulse().setMax(1.0f);
        properties.getPulse().setColorMode(PulseMode::IN_PHASE);
        properties.setIgnorePickIntersection(true);
        _highlightID = entityScriptingInterface->addEntityInternal(properties, entity::HostType::LOCAL);
    }

    // Position focus
    EntityItemProperties properties;
    properties.setPosition(position);
    properties.setRotation(rotation);
    properties.setDimensions(dimensions);
    properties.setVisible(true);
    entityScriptingInterface->editEntity(_highlightID, properties);
}

// Entities.setKeyboardFocusEntity, called by name from a script's thread: the focus changes on the main thread, where
// the entity's renderable and the window are (setFocusedEntity)
void Application::setKeyboardFocusEntity(const QUuid& id) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "setKeyboardFocusEntity", Qt::QueuedConnection, Q_ARG(QUuid, id));
        return;
    }
    _keyboardFocus.setFocusedEntity(id);
}

void KeyboardFocus::setFocusedEntity(const QUuid& id) {
    if (_focused.get() != id) {
        if (qApp->getLoginDialogPoppedUp() && !qApp->getSignInGate().loginDialogID().isNull()) {
            if (id == qApp->getSignInGate().loginDialogID()) {
                emit qApp->getSignInGate().loginDialogFocusEnabled();
            } else if (!_waitingOnRenderable) {
                // that's the only entity we want in focus;
                return;
            }
        }

        _focused.set(id);

        auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
        if (id != UNKNOWN_ENTITY_ID) {
            EntityPropertyFlags desiredProperties;
            desiredProperties += PROP_VISIBLE;
            desiredProperties += PROP_LOCALLY_VISIBLE; // TIVOLI new
            desiredProperties += PROP_SHOW_KEYBOARD_FOCUS_HIGHLIGHT;
            auto properties = entityScriptingInterface->getEntityProperties(id);
            if (properties.getVisible()) {
                auto entities = qApp->getEntities();
                auto entityId = _focused.get();
                auto entityItemRenderable = entities->renderableForEntityId(entityId);
                if (!entityItemRenderable) {
                    _waitingOnRenderable = true;
                } else if (entityItemRenderable->wantsKeyboardFocus()) {
                    entities->setProxyWindow(entityId, qApp->getWindow()->windowHandle());
                    auto keyboardMouseDevice = qApp->getKeyboardMouseDevice();
                    if (keyboardMouseDevice->isActive()) {
                        keyboardMouseDevice->pluginFocusOutEvent();
                    }
                    _lastAcceptedKeyPress = usecTimestampNow();

                    if (properties.getShowKeyboardFocusHighlight()) {
                        if (auto entity = entities->getEntity(entityId)) {
                            highlight(entity->getWorldPosition(), entity->getWorldOrientation(),
                                entity->getScaledDimensions() * HIGHLIGHT_EXPANSION_FACTOR);
                            return;
                        }
                    }
                }
            }
        }

        EntityItemProperties properties;
        properties.setVisible(false);
        entityScriptingInterface->editEntity(_highlightID, properties);
    }
}

bool KeyboardFocus::deliverKeyEvent(QEvent* event) {
    if (_focused.get() != UNKNOWN_ENTITY_ID) {
        switch (event->type()) {
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
                {
                    auto eventHandler = qApp->getEntities()->getEventHandler(_focused.get());
                    if (eventHandler) {
                        event->setAccepted(false);
                        QCoreApplication::sendEvent(eventHandler, event);
                        if (event->isAccepted()) {
                            _lastAcceptedKeyPress = usecTimestampNow();
                            return true;
                        }
                    }
                    break;
                }
            default:
                break;
        }
    }

    return false;
}

void KeyboardFocus::update() {
    { // Update keyboard focus highlight
        if (!_focused.get().isInvalidID()) {
            const quint64 LOSE_FOCUS_AFTER_ELAPSED_TIME = 30 * USECS_PER_SECOND; // if idle for 30 seconds, drop focus
            quint64 elapsedSinceAcceptedKeyPress = usecTimestampNow() - _lastAcceptedKeyPress;
            if (elapsedSinceAcceptedKeyPress > LOSE_FOCUS_AFTER_ELAPSED_TIME) {
                setFocusedEntity(UNKNOWN_ENTITY_ID);
            } else {
                if (auto entity = qApp->getEntities()->getTree()->findEntityByID(_focused.get())) {
                    EntityItemProperties properties;
                    properties.setPosition(entity->getWorldPosition());
                    properties.setRotation(entity->getWorldOrientation());
                    properties.setDimensions(entity->getScaledDimensions() * HIGHLIGHT_EXPANSION_FACTOR);
                    DependencyManager::get<EntityScriptingInterface>()->editEntity(_highlightID, properties);
                }
            }
        }
    }

    {
        if (_waitingOnRenderable && qApp->getEntities()->renderableForEntityId(_focused.get())) {
            QUuid entityId = _focused.get();
            setFocusedEntity(UNKNOWN_ENTITY_ID);
            _waitingOnRenderable = false;
            setFocusedEntity(entityId);
        }
    }
}

void KeyboardFocus::deleteHighlight() {
    if (_highlightID != UNKNOWN_ENTITY_ID) {
        DependencyManager::get<EntityScriptingInterface>()->deleteEntity(_highlightID);
        _highlightID = UNKNOWN_ENTITY_ID;
    }
}
