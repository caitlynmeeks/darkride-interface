//
//  Created by Sam Gondelman on 1/18/2018
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderableMaterialEntityItem_h
#define hifi_RenderableMaterialEntityItem_h

#include "RenderableEntityItem.h"

#include <MaterialEntityItem.h>

#include <procedural/ProceduralMaterialCache.h>

class NetworkMaterial;

namespace render { namespace entities { 

class MaterialEntityRenderer : public TypedEntityRenderer<MaterialEntityItem> {
    using Parent = TypedEntityRenderer<MaterialEntityItem>;
    using Pointer = std::shared_ptr<MaterialEntityRenderer>;
public:
    // whether what this material draws has loaded: its textures, and for a procedural material its shader and every
    // image channel (Procedural::isReady). The arrival waits for it (SafeLanding): a ground painted by a material
    // entity was drawn black while its 27 MB splatmap still loaded, its model being "drawn" long before (10-21)
    bool isMaterialReady() const;

    MaterialEntityRenderer(const EntityItemPointer& entity) : Parent(entity) {}
    ~MaterialEntityRenderer() { deleteMaterial(_parentID, _parentMaterialName); }

private:
    virtual bool needsRenderUpdate() const override;
    virtual bool needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const override;
    virtual void doRenderUpdateAsynchronousTyped(const TypedEntityPointer& entity) override;
    virtual void doRender(RenderArgs* args) override;

    ItemKey getKey() override;
    ShapeKey getShapeKey() override;

    QString _materialURL;
    QString _materialData;
    QString _parentMaterialName;
    quint16 _priority;
    QUuid _parentID;

    MaterialMappingMode _materialMappingMode { UNSET_MATERIAL_MAPPING_MODE };
    bool _materialRepeat { false };
    glm::vec2 _materialMappingPos;
    glm::vec2 _materialMappingScale;
    float _materialMappingRot;
    Transform _transform;
    glm::vec3 _dimensions;

    bool _texturesLoaded { false };
    bool _retryApply { false };

    // with no parent, the tag of the models it applies to instead (userData's applyTo.tag; EntityTreeRenderer's
    // MATERIALS BY TAG), and whether it is applied that way now
    QString _applyToTag;
    bool _appliedByTag { false };
    QUuid _entityID;

    std::shared_ptr<NetworkMaterial> getMaterial() const;
    void setCurrentMaterialName(const std::string& currentMaterialName);

    void applyTextureTransform(std::shared_ptr<NetworkMaterial>& material);
    void applyMaterial(const TypedEntityPointer& entity);
    void deleteMaterial(const QUuid& oldParentID, const QString& oldParentMaterialName);

    NetworkMaterialResourcePointer _networkMaterial;
    NetworkMaterialResource::ParsedMaterials _parsedMaterials;
    std::shared_ptr<NetworkMaterial> _appliedMaterial;
    std::string _currentMaterialName;

};

} } 
#endif // hifi_RenderableMaterialEntityItem_h
