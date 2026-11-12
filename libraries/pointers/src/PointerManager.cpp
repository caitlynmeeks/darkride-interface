//
//  Created by Bradley Austin Davis on 2017/10/16
//  Copyright 2013-2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "PointerManager.h"

#include "PickManager.h"

#include <ScriptCaller.h>

#include "WorldPicks.h"

std::shared_ptr<Pointer> PointerManager::find(unsigned int uid) const {
    return resultWithReadLock<std::shared_ptr<Pointer>>([&] {
        auto itr = _pointers.find(uid);
        if (itr != _pointers.end() && WorldPicks::callerMayName(_worldPointers, uid)) {
            return itr->second;
        }
        return std::shared_ptr<Pointer>();
    });
}

unsigned int PointerManager::addPointer(std::shared_ptr<Pointer> pointer) {
    unsigned int result = PointerEvent::INVALID_POINTER_ID;
    if (ScriptCaller::isRetired()) {
        ScriptCaller::logRetiredRefusal("Pointers", "createPointer");
        return result;
    }
    if (pointer->getRayUID() != PickManager::INVALID_PICK_ID) {
        withWriteLock([&] {
            // Don't let the pointer IDs overflow
            if (_nextPointerID < UINT32_MAX) {
                result = _nextPointerID++;
                _pointers[result] = pointer;
                if (int world = ScriptCaller::world()) {
                    _worldPointers[result] = world;
                    pointer->setMadeByWorld();
                }
            }
        });
    }
    return result;
}

void PointerManager::removePointer(unsigned int uid) {
    std::shared_ptr<Pointer> removed;  // destroyed outside the lock: its destructor removes its pick and visuals
    withWriteLock([&] {
        if (!WorldPicks::callerMayName(_worldPointers, uid)) {
            return;
        }
        auto it = _pointers.find(uid);
        if (it != _pointers.end()) {
            removed = it->second;
            _pointers.erase(it);
        }
        _worldPointers.erase(uid);
    });
}

bool PointerManager::isWorldPointer(unsigned int uid) const {
    return resultWithReadLock<bool>([&] { return _worldPointers.count(uid) > 0; });
}

int PointerManager::removeWorldPointers(int world) {
    std::vector<std::shared_ptr<Pointer>> removed;  // destroyed outside the lock, as in removePointer
    withWriteLock([&] {
        for (auto tag = _worldPointers.begin(); tag != _worldPointers.end();) {
            if (!ScriptCaller::belongsTo(tag->second, world)) {
                ++tag;
                continue;
            }
            auto it = _pointers.find(tag->first);
            if (it != _pointers.end()) {
                removed.push_back(it->second);
                _pointers.erase(it);
            }
            tag = _worldPointers.erase(tag);
        }
    });
    return (int)removed.size();
}

void PointerManager::enablePointer(unsigned int uid) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->enable();
    }
}

void PointerManager::disablePointer(unsigned int uid) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->disable();
    }
}

bool PointerManager::isPointerEnabled(unsigned int uid) const {
    auto pointer = find(uid);
    if (pointer) {
        return pointer->isEnabled();
    }
    return false;
}

QVector<unsigned int> PointerManager::getPointers() const {
    QVector<unsigned int> pointers;
    withReadLock([&] {
        for (auto it = _pointers.cbegin(); it != _pointers.cend(); ++it) {
            if (WorldPicks::callerMayName(_worldPointers, it->first)) {
                pointers.push_back(it->first);
            }
        }
    });
    return pointers;
}

void PointerManager::setRenderState(unsigned int uid, const std::string& renderState) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->setRenderState(renderState);
    }
}

void PointerManager::editRenderState(unsigned int uid, const std::string& state, const QVariant& startProps, const QVariant& pathProps, const QVariant& endProps) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->editRenderState(state, startProps, pathProps, endProps);
    }
}

PickResultPointer PointerManager::getPrevPickResult(unsigned int uid) const {
    PickResultPointer result;
    auto pointer = find(uid);
    if (pointer) {
        result = pointer->getPrevPickResult();
    }
    return result;
}

QVariantMap PointerManager::getPointerProperties(unsigned int uid) const {
    auto pointer = find(uid);
    if (pointer) {
        return pointer->toVariantMap();
    } else {
        return QVariantMap();
    }
}

QVariantMap PointerManager::getPointerScriptParameters(unsigned int uid) const {
    auto pointer = find(uid);
    if (pointer) {
        return pointer->getScriptParameters();
    }
    return QVariantMap();
}

void PointerManager::update() {
    auto cachedPointers = resultWithReadLock<std::unordered_map<unsigned int, std::shared_ptr<Pointer>>>([&] {
        return _pointers;
    });

    for (const auto& pointerPair : cachedPointers) {
        pointerPair.second->update(pointerPair.first);
    }
}

void PointerManager::setPrecisionPicking(unsigned int uid, bool precisionPicking) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->setPrecisionPicking(precisionPicking);
    }
}

void PointerManager::setIgnoreItems(unsigned int uid, const QVector<QUuid>& ignoreEntities) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->setIgnoreItems(ignoreEntities);
    }
}

void PointerManager::setIncludeItems(unsigned int uid, const QVector<QUuid>& includeEntities) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->setIncludeItems(includeEntities);
    }
}

void PointerManager::setLength(unsigned int uid, float length) const {
    auto pointer = find(uid);
    if (pointer) {
        pointer->setLength(length);
    }
}

void PointerManager::setLockEndUUID(unsigned int uid, const QUuid& objectID, bool isAvatar, const glm::mat4& offsetMat) const {
    auto pointer = find(uid);
    // a world's pointer doesn't lock onto an entity the world may not read (an avatar's id is no entity's)
    if (pointer && (isAvatar || WorldPicks::callerMayRead(objectID))) {
        pointer->setLockEndUUID(objectID, isAvatar, offsetMat);
    }
}

bool PointerManager::isLeftHand(unsigned int uid) {
    auto pointer = find(uid);
    if (pointer) {
        return pointer->isLeftHand();
    }
    return false;
}

bool PointerManager::isRightHand(unsigned int uid) {
    auto pointer = find(uid);
    if (pointer) {
        return pointer->isRightHand();
    }
    return false;
}

bool PointerManager::isMouse(unsigned int uid) {
    auto pointer = find(uid);
    if (pointer) {
        return pointer->isMouse();
    }
    return false;
}
