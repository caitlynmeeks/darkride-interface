//
//  RenderableModelEntityItem.h
//  interface/src/entities
//
//  Created by Brad Hefta-Gaub on 8/6/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderableModelEntityItem_h
#define hifi_RenderableModelEntityItem_h

#include <ModelEntityItem.h>

#include <atomic>

#include <QString>
#include <QStringList>

#include <ModelEntityItem.h>
#include <AnimationCache.h>
#include <Model.h>
#include <model-networking/ModelCache.h>
#include <MetaModelPayload.h>

#include "RenderableEntityItem.h"
#include "EntityTreeRenderer.h"

class Model;
class EntityTreeRenderer;

class NetworkMaterial;

namespace render { namespace entities {
class ModelEntityRenderer;
} }

// #define MODEL_ENTITY_USE_FADE_EFFECT
class ModelEntityWrapper : public ModelEntityItem {
    using Parent = ModelEntityItem;
    friend class render::entities::ModelEntityRenderer;

protected:
    ModelEntityWrapper(const EntityItemID& entityItemID) : Parent(entityItemID) {}
    void setModel(const ModelPointer& model);
    void clearModel(const ModelPointer& model);
    ModelPointer getModel() const;
    bool isModelLoaded() const;

    bool _needsInitialSimulation{ true };
private:
    ModelPointer _model;
};

class RenderableModelEntityItem : public ModelEntityWrapper {
    Q_OBJECT

    friend class render::entities::ModelEntityRenderer;
    using Parent = ModelEntityWrapper;
public:
    static EntityItemPointer factory(const EntityItemID& entityID, const EntityItemProperties& properties);

    // for the arrival's log (SafeLanding): whether the renderer has made the model, and its geometry's and textures' state
    QString loadStateForLog() const;
    // the model and all its textures have loaded, whether or not the renderer has updated since (SafeLanding: a far model
    // nothing else updates can keep its "not drawn" flag long after it has loaded)
    bool isModelLoadedWithTextures() const;

    RenderableModelEntityItem(const EntityItemID& entityItemID, bool dimensionsInitialized);
    virtual ~RenderableModelEntityItem();

    virtual void setUnscaledDimensions(const glm::vec3& value) override;

    virtual EntityItemProperties getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const override;
    void doInitialModelSimulation();
    void updateModelBounds();

    virtual bool supportsDetailedIntersection() const override;
    virtual bool findDetailedRayIntersection(const glm::vec3& origin, const glm::vec3& direction,
                        OctreeElementPointer& element, float& distance,
                        BoxFace& face, glm::vec3& surfaceNormal,
                        QVariantMap& extraInfo, bool precisionPicking) const override;
    virtual bool findDetailedParabolaIntersection(const glm::vec3& origin, const glm::vec3& velocity,
                        const glm::vec3& acceleration, OctreeElementPointer& element, float& parabolicDistance,
                        BoxFace& face, glm::vec3& surfaceNormal,
                        QVariantMap& extraInfo, bool precisionPicking) const override;

    virtual void setShapeType(ShapeType type) override;
    virtual void setCompoundShapeURL(const QString& url) override;

    virtual bool isReadyToComputeShape() const override;
    virtual void computeShapeInfo(ShapeInfo& shapeInfo) override;
    bool unableToLoadCollisionShape();
    // the mesh is wanted now, though it waits behind its impostor card (ENTITY-STREAMING.md 3.1): physics asks for a
    // shape made from it (isReadyToComputeShape, which physics calls in its own regions only), or the impostor baker
    // is to draw it; any thread
    void askForMesh() const;

    virtual bool contains(const glm::vec3& point) const override;
    void stopModelOverrideIfNoParent();

    virtual bool shouldBePhysical() const override;
    void simulateRelayedJoints();
    bool getJointMapCompleted();
    void setJointMap(std::vector<int> jointMap);
    int avatarJointIndex(int modelJointIndex);
    void setOverrideTransform(const Transform& transform, const glm::vec3& offset);

    // these are in the frame of this object (model space)
    virtual glm::quat getAbsoluteJointRotationInObjectFrame(int index) const override;
    virtual glm::vec3 getAbsoluteJointTranslationInObjectFrame(int index) const override;
    virtual int getJointParent(int index) const override;
    virtual bool setAbsoluteJointRotationInObjectFrame(int index, const glm::quat& rotation) override;
    virtual bool setAbsoluteJointTranslationInObjectFrame(int index, const glm::vec3& translation) override;

    virtual glm::quat getLocalJointRotation(int index) const override;
    virtual glm::vec3 getLocalJointTranslation(int index) const override;
    virtual bool setLocalJointRotation(int index, const glm::quat& rotation) override;
    virtual bool setLocalJointTranslation(int index, const glm::vec3& translation) override;

    virtual void setJointRotations(const QVector<glm::quat>& rotations) override;
    virtual void setJointRotationsSet(const QVector<bool>& rotationsSet) override;
    virtual void setJointTranslations(const QVector<glm::vec3>& translations) override;
    virtual void setJointTranslationsSet(const QVector<bool>& translationsSet) override;

    virtual void locationChanged(bool tellPhysics = true, bool tellChildren = true) override;

    virtual int getJointIndex(const QString& name) const override;
    virtual QStringList getJointNames() const override;
    virtual QVariantMap getModelExtras() const override;

private:
    bool needsUpdateModelBounds() const;
    void autoResizeJointArrays();
    void copyAnimationJointDataToModel();
    bool readyToAnimate() const;
    void fetchCollisionGeometryResource();

    ModelResource::Pointer _collisionGeometryResource;
    mutable std::atomic<bool> _meshWanted { false };
    std::vector<int> _jointMap;
    QVariantMap _originalTextures;
    bool _jointMapCompleted { false };
    std::atomic<bool> _originalTexturesRead { false }; // set on the main thread after _originalTextures, read by scripts
    bool _dimensionsInitialized { true };
    bool _needsJointSimulation { false };
};

namespace render { namespace entities { 

class ModelEntityRenderer : public TypedEntityRenderer<RenderableModelEntityItem>, public MetaModelPayload {
    using Parent = TypedEntityRenderer<RenderableModelEntityItem>;
    friend class EntityRenderer;
    Q_OBJECT

public:
    ModelEntityRenderer(const EntityItemPointer& entity);
    // Whether a model whose file hasn't loaded yet (or failed to) shows as a magenta wire box, as High Fidelity always
    // drew it; off by default, so a world arriving shows nothing in a model's place until the model is ready
    // (Developer > Render > Show Models Still Loading)
    static void setShowModelsStillLoading(bool show) { _showModelsStillLoading = show; }
    // the model, for the impostor bake (ImpostorBaker): null until it is set
    ModelPointer getModelForBake() const { return _model; }
    // its model file or textures that failed (Model::loadFailures)
    QStringList loadFailures() const override { return _model ? _model->loadFailures() : QStringList(); }
    // the mesh waits behind the impostor card (ENTITY-STREAMING.md 3.1): not fetched yet
    bool isMeshDeferred() const { return resultWithReadLock<bool>([&] { return _meshDeferred; }); }
    bool needsImpostorStandIn() const override;
    virtual scriptable::ScriptableModelBase getScriptableModel() override;
    virtual bool canReplaceModelMeshPart(int meshIndex, int partIndex) override;
    virtual bool replaceScriptableModelMeshPart(scriptable::ScriptableModelBasePointer model, int meshIndex, int partIndex) override;

    void addMaterial(graphics::MaterialLayer material, const std::string& parentMaterialName) override;
    void removeMaterial(graphics::MaterialPointer material, const std::string& parentMaterialName) override;

    // FIXME: model mesh parts should fade individually
    bool isFading() const override { return false; }

protected:
    virtual void removeFromScene(const ScenePointer& scene, Transaction& transaction) override;
    virtual void onRemoveFromSceneTyped(const TypedEntityPointer& entity) override;

    void setKey(bool didVisualGeometryRequestSucceed);
    virtual ItemKey getKey() override;
    virtual uint32_t metaFetchMetaSubItems(ItemIDs& subItems) const override;
    virtual void handleBlendedVertices(int blendshapeNumber, const QVector<BlendshapeOffset>& blendshapeOffsets,
                                       const QVector<int>& blendedMeshSizes, const render::ItemIDs& subItemIDs) override;

    virtual bool needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const override;
    virtual bool needsRenderUpdate() const override;
    virtual void doRender(RenderArgs* args) override;
    virtual void doRenderUpdateSynchronousTyped(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) override;

    void setIsVisibleInSecondaryCamera(bool value) override;
    render::hifi::Tag tagMaskKeepingBake(const ModelPointer& model) const;
    void setRenderLayer(RenderLayer value) override;
    void setPrimitiveMode(PrimitiveMode value) override;
    void setCullWithParent(bool value) override;

private:
    void animate(const TypedEntityPointer& entity);
    void mapJoints(const TypedEntityPointer& entity, const ModelPointer& model);

    // Transparency is handled in ModelMeshPartPayload
    virtual bool isTransparent() const override { return false; }

    bool _hasModel { false };
    ModelPointer _model;
    QString _textures;
    bool _texturesLoaded { false };
    // int _lastKnownCurrentFrame { -1 };
#ifdef MODEL_ENTITY_USE_FADE_EFFECT
    bool _hasTransitioned{ false };
#endif

    const void* _collisionMeshKey { nullptr };

    QUrl _parsedModelURL;
    bool _jointMappingCompleted { false };
    QVector<int> _jointMapping; // domain is index into model-joints, range is index into animation-joints
    AnimationPointer _animation;
    bool _animating { false };
    QString _animationURL;
    uint64_t _lastAnimated { 0 };

    render::ItemKey _itemKey { render::ItemKey::Builder().withTypeMeta() };

    bool _didLastVisualGeometryRequestSucceed { true };
    bool _prevModelLoaded { false };

    // the mesh waiting behind the impostor card (ENTITY-STREAMING.md 3.1)
    bool shouldDeferMesh(const TypedEntityPointer& entity, ImpostorStandIn& standIn);
    void reportImpostorStandIn(const TypedEntityPointer& entity);
    void updateVisuallyReady(const TypedEntityPointer& entity, bool deferMesh, bool atlasesLoaded);
    bool _meshDeferred { false };
    int _reportedStandIn { -1 };   // what the card's owner was last told: -1 nothing yet
    bool _meshFetched { false };   // setURL was called: the mesh never waits again

    void processMaterials();
    bool _allProceduralMaterialsLoaded { false };

    // the lightmap the entity's userData assigns to the whole model (LIGHTMAPS.md §3.2): a material layer over all
    // its parts, remade when the entry changes
    void applyLightmap(const TypedEntityPointer& entity);
    QString _userData;
    QString _lightmapSpec;
    std::shared_ptr<NetworkMaterial> _lightmapMaterial;

    // the Material entities that apply to every model with a tag this one carries (EntityTreeRenderer's MATERIALS BY
    // TAG), kept up to date as they, the model and its userData change
    void applyTaggedMaterials(const TypedEntityPointer& entity);
    uint32_t _taggedMaterialsVersion { 0 };
    QUrl _taggedMaterialsModelURL;
    QString _taggedMaterialsUserData;
    EntityTreeRenderer::TaggedMaterials _appliedTaggedMaterials;

    static void metaBlendshapeOperator(render::ItemID renderItemID, int blendshapeNumber, const QVector<BlendshapeOffset>& blendshapeOffsets,
                                       const QVector<int>& blendedMeshSizes, const render::ItemIDs& subItemIDs);

    static std::atomic<bool> _showModelsStillLoading;
};

} } // namespace 

#endif // hifi_RenderableModelEntityItem_h
