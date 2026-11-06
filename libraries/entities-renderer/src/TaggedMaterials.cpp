//
//  TaggedMaterials.cpp
//  libraries/entities-renderer/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TaggedMaterials.h"

#include <DependencyManager.h>

#include "EntityTreeRenderer.h"

std::mutex TaggedMaterials::_lock;
TaggedMaterials::Materials TaggedMaterials::_materials;
std::map<QUuid, QSet<QString>> TaggedMaterials::_modelTags;
std::map<QString, QSet<QUuid>> TaggedMaterials::_tagModels;
std::atomic<uint32_t> TaggedMaterials::_version { 0 };

void TaggedMaterials::setMaterial(const QUuid& materialEntityID, const Material& material) {
    QSet<QUuid> toMark;
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto it = _materials.find(materialEntityID);
        if (it != _materials.end()) {
            const Material& old = it->second;
            if (old.tag == material.tag && old.layer.material == material.layer.material &&
                old.layer.priority == material.layer.priority && old.parts == material.parts) {
                return;
            }
            // the models it leaves, as well as those it comes to
            auto oldModels = _tagModels.find(old.tag);
            if (oldModels != _tagModels.end()) {
                toMark += oldModels->second;
            }
            it->second = material;
        } else {
            _materials.emplace(materialEntityID, material);
        }
        auto models = _tagModels.find(material.tag);
        if (models != _tagModels.end()) {
            toMark += models->second;
        }
        _version++;
    }
    markModelsChanged(toMark);
}

void TaggedMaterials::removeMaterial(const QUuid& materialEntityID) {
    QSet<QUuid> toMark;
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto it = _materials.find(materialEntityID);
        if (it == _materials.end()) {
            return;
        }
        auto models = _tagModels.find(it->second.tag);
        if (models != _tagModels.end()) {
            toMark = models->second;
        }
        _materials.erase(it);
        _version++;
    }
    markModelsChanged(toMark);
}

void TaggedMaterials::setModelTags(const QUuid& modelEntityID, const QSet<QString>& tags) {
    std::lock_guard<std::mutex> lock(_lock);
    QSet<QString>& current = _modelTags[modelEntityID];
    for (const auto& tag : current) {
        if (!tags.contains(tag)) {
            auto models = _tagModels.find(tag);
            if (models != _tagModels.end()) {
                models->second.remove(modelEntityID);
                if (models->second.isEmpty()) {
                    _tagModels.erase(models);
                }
            }
        }
    }
    for (const auto& tag : tags) {
        _tagModels[tag].insert(modelEntityID);
    }
    current = tags;
    if (current.isEmpty()) {
        _modelTags.erase(modelEntityID);
    }
}

void TaggedMaterials::removeModel(const QUuid& modelEntityID) {
    setModelTags(modelEntityID, QSet<QString>());
}

TaggedMaterials::Materials TaggedMaterials::getMaterialsFor(const QSet<QString>& tags) {
    Materials result;
    if (tags.isEmpty()) {
        return result;
    }
    std::lock_guard<std::mutex> lock(_lock);
    for (const auto& entry : _materials) {
        if (tags.contains(entry.second.tag)) {
            result.emplace(entry.first, entry.second);
        }
    }
    return result;
}

void TaggedMaterials::markModelsChanged(const QSet<QUuid>& modelEntityIDs) {
    if (modelEntityIDs.isEmpty()) {
        return;
    }
    // their renderers update with the next changes, and each looks up the materials for its tags
    auto entityTreeRenderer = DependencyManager::get<EntityTreeRenderer>();
    if (!entityTreeRenderer) {
        return;
    }
    for (const auto& id : modelEntityIDs) {
        entityTreeRenderer->onEntityChanged(id);
    }
}
