//
//  RayPickScriptingInterface.cpp
//  interface/src/raypick
//
//  Created by Sam Gondelman 8/15/2017
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RayPickScriptingInterface.h"

#include <WorldPicks.h>
#include <QVariant>
#include "GLMHelpers.h"

#include <PickManager.h>

unsigned int RayPickScriptingInterface::createRayPick(const QVariant& properties) {
    return DependencyManager::get<PickScriptingInterface>()->createPick(PickQuery::PickType::Ray, properties);
}

void RayPickScriptingInterface::enableRayPick(unsigned int uid) {
    DependencyManager::get<PickManager>()->enablePick(uid);
}

void RayPickScriptingInterface::disableRayPick(unsigned int uid) {
    DependencyManager::get<PickManager>()->disablePick(uid);
}

void RayPickScriptingInterface::removeRayPick(unsigned int uid) {
    DependencyManager::get<PickManager>()->removePick(uid);
}

QVariantMap RayPickScriptingInterface::getPrevRayPickResult(unsigned int uid) {
    // as the caller may read it: for a world's script, a hit on an entity it may not read is no hit
    return WorldPicks::resultForCaller(DependencyManager::get<PickManager>()->getPrevPickResult(uid));
}

void RayPickScriptingInterface::setPrecisionPicking(unsigned int uid, bool precisionPicking) {
    DependencyManager::get<PickManager>()->setPrecisionPicking(uid, precisionPicking);
}

void RayPickScriptingInterface::setIgnoreItemsVector(unsigned int uid, const QVector<QUuid>& ignoreItems) {
    DependencyManager::get<PickManager>()->setIgnoreItems(uid, ignoreItems);
}

void RayPickScriptingInterface::setIncludeItemsVector(unsigned int uid, const QVector<QUuid>& includeItems) {
    DependencyManager::get<PickManager>()->setIncludeItems(uid, includeItems);
}

bool RayPickScriptingInterface::isLeftHand(unsigned int uid) {
    return DependencyManager::get<PickManager>()->isLeftHand(uid);
}

bool RayPickScriptingInterface::isRightHand(unsigned int uid) {
    return DependencyManager::get<PickManager>()->isRightHand(uid);
}

bool RayPickScriptingInterface::isMouse(unsigned int uid) {
    return DependencyManager::get<PickManager>()->isMouse(uid);
}