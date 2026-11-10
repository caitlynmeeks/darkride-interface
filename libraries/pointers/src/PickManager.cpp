//
//  Created by Sam Gondelman 10/19/2017
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "PickManager.h"

#include <ScriptCaller.h>

#include "WorldPicks.h"
#include "PerfStat.h"
#include "Profile.h"

PickManager::PickManager() {
    setShouldPickHUDOperator([]() { return false; });
    setCalculatePos2DFromHUDOperator([](const glm::vec3& intersection) { return glm::vec2(NAN); });
}

unsigned int PickManager::addPick(PickQuery::PickType type, const std::shared_ptr<PickQuery> pick) {
    unsigned int id = INVALID_PICK_ID;
    if (ScriptCaller::isRetired()) {
        ScriptCaller::logRetiredRefusal("Picks", "createPick");
        return id;
    }
    withWriteLock([&] {
        // Don't let the pick IDs overflow
        if (_nextPickID < UINT32_MAX) {
            id = _nextPickID++;
            _picks[type][id] = pick;
            _typeMap[id] = type;
            _totalPickCounts[type]++;
            if (int world = ScriptCaller::world()) {
                _worldPicks[id] = world;
                pick->setMadeByWorld();
            }
        }
    });
    return id;
}

std::shared_ptr<PickQuery> PickManager::findPick(unsigned int uid) const {
    return resultWithReadLock<std::shared_ptr<PickQuery>>([&] {
        auto type = _typeMap.find(uid);
        if (type != _typeMap.end() && WorldPicks::callerMayName(_worldPicks, uid)) {
            return _picks.find(type->second)->second.find(uid)->second;
        }
        return std::shared_ptr<PickQuery>();
    });
}

bool PickManager::callerMayName(unsigned int uid) const {
    return resultWithReadLock<bool>([&] { return WorldPicks::callerMayName(_worldPicks, uid); });
}

void PickManager::removePick(unsigned int uid) {
    withWriteLock([&] {
        if (!WorldPicks::callerMayName(_worldPicks, uid)) {
            return;
        }
        auto typeIt = _typeMap.find(uid);
        if (typeIt != _typeMap.end()) {
            _picks[typeIt->second].erase(uid);
            _totalPickCounts[typeIt->second]--;
            _typeMap.erase(typeIt);
        }
        _worldPicks.erase(uid);
    });
}

int PickManager::removeWorldPicks(int world) {
    int removed = 0;
    withWriteLock([&] {
        for (auto tag = _worldPicks.begin(); tag != _worldPicks.end();) {
            if (!ScriptCaller::belongsTo(tag->second, world)) {
                ++tag;
                continue;
            }
            auto typeIt = _typeMap.find(tag->first);
            if (typeIt != _typeMap.end()) {
                _picks[typeIt->second].erase(tag->first);
                _totalPickCounts[typeIt->second]--;
                _typeMap.erase(typeIt);
                removed++;
            }
            tag = _worldPicks.erase(tag);
        }
    });
    return removed;
}

QVariantMap PickManager::getPickProperties(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        return pick->toVariantMap();
    }
    return QVariantMap();
}

QVariantMap PickManager::getPickScriptParameters(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        return pick->getScriptParameters();
    }
    return QVariantMap();
}

QVector<unsigned int> PickManager::getPicks() const {
    QVector<unsigned int> picks;
    withReadLock([&] {
        for (auto typeIt = _picks.cbegin(); typeIt != _picks.cend(); ++typeIt) {
            auto& picksForType = typeIt->second;
            for (auto pickIt = picksForType.cbegin(); pickIt != picksForType.cend(); ++pickIt) {
                if (WorldPicks::callerMayName(_worldPicks, pickIt->first)) {
                    picks.push_back(pickIt->first);
                }
            }
        }
    });
    return picks;
}

PickResultPointer PickManager::getPrevPickResult(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        return pick->getPrevPickResult();
    }
    return PickResultPointer();
}

void PickManager::enablePick(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        pick->enable();
    }
}

void PickManager::disablePick(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        pick->disable();
    }
}

bool PickManager::isPickEnabled(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        return pick->isEnabled();
    }
    return false;
}

void PickManager::setPrecisionPicking(unsigned int uid, bool precisionPicking) const {
    auto pick = findPick(uid);
    if (pick) {
        pick->setPrecisionPicking(precisionPicking);
    }
}

void PickManager::setIgnoreItems(unsigned int uid, const QVector<QUuid>& ignore) const {
    auto pick = findPick(uid);
    if (pick) {
        pick->setIgnoreItems(WorldPicks::readableByCaller(ignore));
    }
}

void PickManager::setIncludeItems(unsigned int uid, const QVector<QUuid>& include) const {
    auto pick = findPick(uid);
    if (pick) {
        pick->setIncludeItems(WorldPicks::readableByCaller(include));
    }
}

Transform PickManager::getParentTransform(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        auto parentTransform = pick->parentTransform;
        if (parentTransform) {
            return parentTransform->getTransform();
        }
    }
    return Transform();
}

Transform PickManager::getResultTransform(unsigned int uid) const {
    auto pick = findPick(uid);
    if (pick) {
        return pick->getResultTransform();
    }
    return Transform();
}

void PickManager::update() {
    uint64_t expiry = usecTimestampNow() + _perFrameTimeBudget;
    if (getLasersEnabled()) expiry = usecTimestampNow() + _perFrameLaserTimeBudget;
    std::unordered_map<PickQuery::PickType, std::unordered_map<unsigned int, std::shared_ptr<PickQuery>>> cachedPicks;
    withReadLock([&] {
        cachedPicks = _picks;
    });

    bool shouldPickHUD = _shouldPickHUDOperator();
    // Each type will update at least one pick, regardless of the expiry
    // {
    //     PROFILE_RANGE_EX(picks, "StylusPicks", 0xffff0000, (uint64_t)_totalPickCounts[PickQuery::Stylus]);
    //     PerformanceTimer perfTimer("StylusPicks");
    //     _updatedPickCounts[PickQuery::Stylus] = _stylusPickCacheOptimizer.update(cachedPicks[PickQuery::Stylus], _nextPickToUpdate[PickQuery::Stylus], expiry, false);
    // }
    {
        PROFILE_RANGE_EX(picks, "RayPicks", 0xffff0000, (uint64_t)_totalPickCounts[PickQuery::Ray]);
        PerformanceTimer perfTimer("RayPicks");
        _updatedPickCounts[PickQuery::Ray] = _rayPickCacheOptimizer.update(cachedPicks[PickQuery::Ray], _nextPickToUpdate[PickQuery::Ray], expiry, shouldPickHUD);
    }
    {
        PROFILE_RANGE_EX(picks, "ParabolaPicks", 0xffff0000, (uint64_t)_totalPickCounts[PickQuery::Parabola]);
        PerformanceTimer perfTimer("ParabolaPicks");
        _updatedPickCounts[PickQuery::Parabola] = _parabolaPickCacheOptimizer.update(cachedPicks[PickQuery::Parabola], _nextPickToUpdate[PickQuery::Parabola], expiry, shouldPickHUD);
    }
    {
        PROFILE_RANGE_EX(picks, "CollisionPicks", 0xffff0000, (uint64_t)_totalPickCounts[PickQuery::Collision]);
        PerformanceTimer perfTimer("CollisionPicks");
        _updatedPickCounts[PickQuery::Collision] = _collisionPickCacheOptimizer.update(cachedPicks[PickQuery::Collision], _nextPickToUpdate[PickQuery::Collision], expiry, false);
    }
}

bool PickManager::isLeftHand(unsigned int uid) {
    auto pick = findPick(uid);
    if (pick) {
        return pick->isLeftHand();
    }
    return false;
}

bool PickManager::isRightHand(unsigned int uid) {
    auto pick = findPick(uid);
    if (pick) {
        return pick->isRightHand();
    }
    return false;
}

bool PickManager::isMouse(unsigned int uid) {
    auto pick = findPick(uid);
    if (pick) {
        return pick->isMouse();
    }
    return false;
}
