//
//  Created by Bradley Austin Davis on 2016/05/09
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderableShapeEntityItem_h
#define hifi_RenderableShapeEntityItem_h

#include "RenderableEntityItem.h"

#include <gpu/Texture.h>
#include <procedural/Procedural.h>
#include <ReflectionProbeStage.h>
#include <ShapeEntityItem.h>
#include <TextureCache.h>

namespace render { namespace entities { 

class ShapeEntityRenderer : public TypedEntityRenderer<ShapeEntityItem> {
    using Parent = TypedEntityRenderer<ShapeEntityItem>;
    using Pointer = std::shared_ptr<ShapeEntityRenderer>;
public:
    ShapeEntityRenderer(const EntityItemPointer& entity);
    ~ShapeEntityRenderer();

    virtual scriptable::ScriptableModelBase getScriptableModel() override;

protected:
    ShapeKey getShapeKey() override;
    Item::Bound getBound() override;

private:
    virtual bool needsRenderUpdate() const override;
    virtual bool needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const override;
    virtual void doRenderUpdateSynchronousTyped(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) override;
    virtual void doRenderUpdateAsynchronousTyped(const TypedEntityPointer& entity) override;
    virtual void doRender(RenderArgs* args) override;
    virtual bool isTransparent() const override;
    virtual void onRemoveFromSceneTyped(const TypedEntityPointer& entity) override;

    // bool _doZoneCull{ false };  // TIVOLI Zone Culling

    enum Pipeline { SIMPLE, MATERIAL, PROCEDURAL };
    Pipeline getPipelineType(const graphics::MultiMaterial& materials) const;

    QString _proceduralData;
    entity::Shape _shape { entity::Sphere };

    PulsePropertyGroup _pulseProperties;
    std::shared_ptr<graphics::ProceduralMaterial> _material { std::make_shared<graphics::ProceduralMaterial>() };
    glm::vec3 _color { NAN };
    float _alpha { NAN };
    BillboardMode _billboardMode { BillboardMode::NONE };

    glm::vec3 _position;
    glm::vec3 _dimensions;
    glm::quat _orientation;

    // a mirror (userData "mirror": "picture" or "screen"): registered with Mirrors (render-utils). Picture mode shows
    // its picture as an emissive map; Screen mode draws the glass itself (doRender) with the picture of the frame
    bool _mirror { false };
    bool _mirrorScreen { false };   // screen mode: the glass draws its picture by screen position (else as a map)
    bool _mirrorScreenWanted { false };   // the userData says screen; _mirrorScreen is that only on a flat display
    bool _mirrorShown { false };   // Picture mode: it has a picture this frame (else it shows its colour)
    QUuid _mirrorId;
    uint32_t _mirrorGeneration { 0 };
    gpu::TextureSourcePointer _mirrorSource;
    gpu::TexturePointer _mirrorPicture;
    gpu::BufferPointer _mirrorRegion;  // Screen mode: the region of the screen the picture covers, for the glass shader

    // a reflection probe (userData "reflectionProbe", LIGHTMAPS.md §7): the entity is the probe's box, the probe at
    // its position; registered with the scene's ReflectionProbeStage, its cube map (the entry's url, loaded as an
    // ambient map) handed over once it has loaded
    graphics::ReflectionProbePointer _reflectionProbe;
    ReflectionProbeStagePointer _reflectionProbeStage;
    ReflectionProbeStage::Index _reflectionProbeIndex { ReflectionProbeStage::INVALID_INDEX };
    QString _reflectionProbeURL;
    NetworkTexturePointer _reflectionProbeTexture;
    bool _reflectionProbePending { false };
    void removeReflectionProbe();
};

} } 
#endif // hifi_RenderableShapeEntityItem_h
