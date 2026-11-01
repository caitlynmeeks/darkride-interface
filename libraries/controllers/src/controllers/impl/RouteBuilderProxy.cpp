//
//  Created by Bradley Austin Davis 2015/10/09
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "RouteBuilderProxy.h"

#include <QtCore/QDebug>

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>

#include <GLMHelpers.h>

#include "MappingBuilderProxy.h"
#include "../ScriptingInterface.h"
#include "../Logging.h"

#include "filters/ClampFilter.h"
#include "filters/ConstrainToIntegerFilter.h"
#include "filters/ConstrainToPositiveIntegerFilter.h"
#include "filters/DeadZoneFilter.h"
#include "filters/HysteresisFilter.h"
#include "filters/InvertFilter.h"
#include "filters/NotFilter.h"
#include "filters/PulseFilter.h"
#include "filters/ScaleFilter.h"
#include <ScriptCaller.h>

#include "endpoints/ActionEndpoint.h"
#include "endpoints/CallbackEndpoint.h"
#include "endpoints/JSEndpoint.h"
#include "endpoints/ScriptEndpoint.h"
#include "../UserInputMapper.h"
#include <unordered_set>
#include "filters/TranslateFilter.h"
#include "filters/TransformFilter.h"
#include "filters/PostTransformFilter.h"
#include "filters/RotateFilter.h"
#include "filters/LowVelocityFilter.h"
#include "filters/ExponentialSmoothingFilter.h"
#include "conditionals/AndConditional.h"

using namespace controller;

void RouteBuilderProxy::toQml(const QJSValue& destination) {
    qCDebug(controllers) << "Completing route " << destination.toString();
    auto destinationEndpoint = _parent.endpointFor(destination);
    return to(destinationEndpoint);
}

void RouteBuilderProxy::to(const QScriptValue& destination) {
    qCDebug(controllers) << "Completing route " << destination.toString();
    auto destinationEndpoint = _parent.endpointFor(destination);
    return to(destinationEndpoint);
}

void RouteBuilderProxy::toValue(const QVariant& destination) {
    to(_parent.endpointFor(destination));
}

QObject* RouteBuilderProxy::whenValue(const QVariant& expression) {
    auto newConditional = _parent.conditionalFor(expression);
    if (_route->conditional) {
        _route->conditional = ConditionalPointer(new AndConditional(_route->conditional, newConditional));
    } else {
        _route->conditional = newConditional;
    }
    return this;
}

bool RouteBuilderProxy::worldMayRouteTo(const Endpoint::Pointer& destination) {
    if (!destination) {
        return false;
    }
    if (std::dynamic_pointer_cast<JSEndpoint>(destination) || std::dynamic_pointer_cast<ScriptEndpoint>(destination) ||
        std::dynamic_pointer_cast<CallbackEndpoint>(destination)) {
        return true;   // the world's own function: it reads the input itself
    }
    if (!std::dynamic_pointer_cast<ActionEndpoint>(destination) ||
        destination->getInput().getDevice() != UserInputMapper::ACTIONS_DEVICE) {
        return false;  // a device's endpoint (Standard, Hardware): the visitor's own controller scripts read those
    }
    static const std::unordered_set<int> ALLOWED = [] {
        std::unordered_set<int> allowed;
        for (int action = toInt(Action::TRANSLATE_X); action <= toInt(Action::TRANSLATE_CAMERA_Z); ++action) {
            allowed.insert(action);    // movement, the combined axes
        }
        for (int action = toInt(Action::LEFT_HAND); action <= toInt(Action::HEAD); ++action) {
            allowed.insert(action);    // poses
        }
        for (Action action : { Action::ACTION1, Action::ACTION2, Action::SHIFT, Action::SPRINT,
                               Action::LONGITUDINAL_BACKWARD, Action::LONGITUDINAL_FORWARD, Action::LATERAL_LEFT,
                               Action::LATERAL_RIGHT, Action::VERTICAL_DOWN, Action::VERTICAL_UP, Action::YAW_LEFT,
                               Action::YAW_RIGHT, Action::PITCH_DOWN, Action::PITCH_UP, Action::BOOM_IN, Action::BOOM_OUT }) {
            allowed.insert(toInt(action));
        }
        for (int action = toInt(Action::LEFT_ARM); action < toInt(Action::NUM_ACTIONS); ++action) {
            if (action != toInt(Action::SPRINT)) {
                allowed.insert(action);  // the arms, fingers, legs, tracked objects, eyes, face and head turn
            }
        }
        return allowed;
    }();
    return ALLOWED.count(destination->getInput().getChannel()) > 0;
}

void RouteBuilderProxy::to(const Endpoint::Pointer& destination) {
    // a world's mapping, or any route a world's script builds, doesn't drive the app's own UI, nor the visitor's own
    // controller endpoints
    if ((_mapping->madeByWorld || ScriptCaller::isWorld()) && !worldMayRouteTo(destination)) {
        if (!_mapping->refusalLogged) {
            _mapping->refusalLogged = true;
            QString target = "nothing";
            if (destination && destination->getInput().getDevice() == UserInputMapper::ACTIONS_DEVICE) {
                target = "Actions." + _parent.getActionName(Action(destination->getInput().getChannel()));
            } else if (destination) {
                target = "a device's input";
            }
            qCWarning(controllers) << "A world's mapping" << _mapping->name << "may not route to" << target
                                   << "(the app's own controls; a world's mapping goes to its functions and the movement, vehicle and pose actions)";
        }
        deleteLater();
        return;
    }
    _route->destination = destination;
    _mapping->routes.push_back(_route);
    deleteLater();
}

QObject* RouteBuilderProxy::debug(bool enable) {
    _route->debug = enable;
    return this;
}

QObject* RouteBuilderProxy::peek(bool enable) {
    _route->peek = enable;
    return this;
}

QObject* RouteBuilderProxy::when(const QScriptValue& expression) {
    // FIXME: Support "!" conditional in simple expression and array expression.
    // Note that "!" is supported when parsing a JSON file, in UserInputMapper::parseConditional().
    auto newConditional = _parent.conditionalFor(expression);
    if (_route->conditional) {
        _route->conditional = ConditionalPointer(new AndConditional(_route->conditional, newConditional));
    } else {
        _route->conditional = newConditional;
    }
    return this;
}

QObject* RouteBuilderProxy::whenQml(const QJSValue& expression) {
    auto newConditional = _parent.conditionalFor(expression);
    if (_route->conditional) {
        _route->conditional = ConditionalPointer(new AndConditional(_route->conditional, newConditional));
    } else {
        _route->conditional = newConditional;
    }
    return this;
}

QObject* RouteBuilderProxy::clamp(float min, float max) {
    addFilter(std::make_shared<ClampFilter>(min, max));
    return this;
}

QObject* RouteBuilderProxy::scale(float multiplier) {
    addFilter(std::make_shared<ScaleFilter>(multiplier));
    return this;
}

QObject* RouteBuilderProxy::invert() {
    addFilter(std::make_shared<InvertFilter>());
    return this;
}

QObject* RouteBuilderProxy::hysteresis(float min, float max) {
    addFilter(std::make_shared<HysteresisFilter>(min, max));
    return this;
}

QObject* RouteBuilderProxy::deadZone(float min) {
    addFilter(std::make_shared<DeadZoneFilter>(min));
    return this;
}

QObject* RouteBuilderProxy::translate(glm::vec3 translate) {
    addFilter(std::make_shared<TranslateFilter>(translate));
    return this;
}

QObject* RouteBuilderProxy::transform(glm::mat4 transform) {
    addFilter(std::make_shared<TransformFilter>(transform));
    return this;
}

QObject* RouteBuilderProxy::postTransform(glm::mat4 transform) {
    addFilter(std::make_shared<PostTransformFilter>(transform));
    return this;
}

QObject* RouteBuilderProxy::rotate(glm::quat rotation) {
    addFilter(std::make_shared<RotateFilter>(rotation));
    return this;
}

QObject* RouteBuilderProxy::lowVelocity(float rotationConstant, float translationConstant) {
    addFilter(std::make_shared<LowVelocityFilter>(rotationConstant, translationConstant));
    return this;
}

QObject* RouteBuilderProxy::exponentialSmoothing(float rotationConstant, float translationConstant) {
    addFilter(std::make_shared<ExponentialSmoothingFilter>(rotationConstant, translationConstant));
    return this;
}

QObject* RouteBuilderProxy::constrainToInteger() {
    addFilter(std::make_shared<ConstrainToIntegerFilter>());
    return this;
}

QObject* RouteBuilderProxy::constrainToPositiveInteger() {
    addFilter(std::make_shared<ConstrainToPositiveIntegerFilter>());
    return this;
}

QObject* RouteBuilderProxy::pulse(float interval) {
    addFilter(std::make_shared<PulseFilter>(interval));
    return this;
}

QObject* RouteBuilderProxy::logicalNot() {
    addFilter(std::make_shared<NotFilter>());
    return this;
}

void RouteBuilderProxy::addFilter(Filter::Pointer filter) {
    _route->filters.push_back(filter);
}

