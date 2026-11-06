//
//  TaggedMaterials.h
//  libraries/entities-renderer/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_TaggedMaterials_h
#define hifi_TaggedMaterials_h

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QUuid>

#include <graphics/Material.h>

// MATERIALS BY TAG: a Material entity with no parent whose userData names a tag, { "applyTo": { "tag": "isTree" } },
// applies to every Model entity that carries the tag, on the parts its parentMaterialName names, rather than to a
// parent (MaterialEntityRenderer). A model carries a tag set to true in the extras of any of its glTF objects (a
// Blender custom property), unless its entity's userData names the tag, which then decides (true or false).
//
// This index keeps the tagged materials and which models carry which tag. It is driven by events only: a Material
// entity's tag or material changes (setMaterial, removeMaterial), or a model's tags change, which includes its model
// arriving (setModelTags, removeModel). A material's change marks changed only the models that carry its tag, its
// old one and its new one; nothing here runs per frame. Each ModelEntityRenderer keeps the materials that apply to it
// (applyTaggedMaterials). Any thread
class TaggedMaterials {
public:
    struct Material {
        QString tag;
        graphics::MaterialLayer layer;
        std::string parts;
    };
    using Materials = std::map<QUuid, Material>;

    // a Material entity applied by tag, new or changed; the same as before marks nothing
    static void setMaterial(const QUuid& materialEntityID, const Material& material);
    static void removeMaterial(const QUuid& materialEntityID);

    // the tags a model carries, when its model arrives or they change, and its leaving
    static void setModelTags(const QUuid& modelEntityID, const QSet<QString>& tags);
    static void removeModel(const QUuid& modelEntityID);

    // the materials whose tag is one of these
    static Materials getMaterialsFor(const QSet<QString>& tags);
    // changes with every change to the materials: a model with tags whose version is behind looks again
    static uint32_t getVersion() { return _version; }

private:
    static void markModelsChanged(const QSet<QUuid>& modelEntityIDs);

    static std::mutex _lock;
    static Materials _materials;
    static std::map<QUuid, QSet<QString>> _modelTags;
    static std::map<QString, QSet<QUuid>> _tagModels;
    static std::atomic<uint32_t> _version;
};

#endif // hifi_TaggedMaterials_h
