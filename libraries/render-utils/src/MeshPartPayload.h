//
//  MeshPartPayload.h
//  interface/src/renderer
//
//  Created by Sam Gateau on 10/3/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_MeshPartPayload_h
#define hifi_MeshPartPayload_h

#include <atomic>

#include <Interpolate.h>

#include <gpu/Batch.h>

#include <render/Scene.h>

#include <graphics/Geometry.h>

#include "Model.h"

class ImpostorSwitch;
class Model;

class MeshPartPayload {
public:
    MeshPartPayload() = default;
    MeshPartPayload(const std::shared_ptr<const graphics::Mesh>& mesh,
                    int partIndex,
                    graphics::MaterialPointer material,
                    const uint64_t& created);
    virtual ~MeshPartPayload() = default;

    typedef render::Payload<MeshPartPayload> Payload;
    typedef Payload::DataPointer Pointer;

    virtual void updateKey(const render::ItemKey& key);

    virtual void updateMeshPart(const std::shared_ptr<const graphics::Mesh>& drawMesh, int partIndex);

    virtual void notifyLocationChanged() {}
    void updateTransform(const Transform& transform);
    void updateTransformAndBound(const Transform& transform);

    // Render Item interface
    virtual render::ItemKey getKey() const;
    virtual render::Item::Bound getBound() const;
    virtual render::ShapeKey getShapeKey() const;
    virtual void render(RenderArgs* args);
    // GPU instancing (INSTANCING.md 3): a plain part draws alone
    virtual render::Instancing getInstancing(RenderArgs* args) { return render::Instancing(); }
    virtual void renderInstances(RenderArgs* args, const render::Transforms& transforms) { render(args); }

    // ModelMeshPartPayload functions to perform render
    void drawCall(gpu::Batch& batch) const;
    virtual void bindMesh(gpu::Batch& batch);
    virtual void bindTransform(gpu::Batch& batch, RenderArgs::RenderMode renderMode) const;

    // A part whose material has a custom shader draws through the shader's own pipeline (its key says so). When the shader
    // can't draw (Settings > Custom Shaders on Models off, the world still loading, a shader this GPU doesn't compile) the
    // part draws its plain material instead (a colour, textures, a glow), through the pass's standard pipeline for the
    // plain key, so that nothing vanishes (the Lightling's body, 10-28). The plumber comes with the args (DrawTask).
    bool bindPlainPipeline(RenderArgs* args, const render::ShapeKey& plainKey) const;
    render::ShapeKey buildShapeKey(bool allowOwnPipeline) const;   // getShapeKey's; false = the plain key

    // Payload resource cached values
    Transform _worldFromLocalTransform;
    int _partIndex = 0;
    bool _hasColorAttrib{ false };

    graphics::Box _localBound;
    mutable graphics::Box _worldBound;
    std::shared_ptr<const graphics::Mesh> _drawMesh;

    graphics::MultiMaterial _drawMaterials;
    // the layers of _drawMaterials that are not procedural: what a procedural on top draws over (bindUnderlyingMaterial)
    graphics::MultiMaterial _underlyingMaterials;
    graphics::Mesh::Part _drawPart;

    size_t getVerticesCount() const { return _drawMesh ? _drawMesh->getNumVertices() : 0; }
    size_t getMaterialTextureSize() { return _drawMaterials.getTextureSize(); }
    int getMaterialTextureCount() { return _drawMaterials.getTextureCount(); }
    bool hasTextureInfo() const { return _drawMaterials.hasTextureInfo(); }

    void addMaterial(graphics::MaterialLayer material);
    void removeMaterial(graphics::MaterialPointer material);

    void setCullWithParent(bool value) { _cullWithParent = value; }

    static bool DEFAULT_ENABLE_MATERIAL_PROCEDURAL_SHADERS;
    static bool enableMaterialProceduralShaders; // set from menu/settings
    static bool sceneIsReady; // set from entity tree renderer

    // A diagnostic (Test.dumpDraws(frames)): for that many frames, every main-view draw of a model part logs what it
    // drew and where (INSTANCING-DUMP lines), so a frame with GPU instancing on can be diffed against one with it off
    // (tests-manual/instancing/diff-dump.py). beginFrame is called once a frame on the render thread, before the frame.
    static std::atomic<int> dumpDrawFrames;
    static void beginFrame();
    static bool dumping() { return _dumpingFrame.load(std::memory_order_relaxed); }
    static int dumpFrame() { return _dumpFrame.load(std::memory_order_relaxed); }

protected:
    // for a procedural on top (simple_procedural.slf's getUnderlying* functions): the layers beneath it, merged as they
    // are without it (RenderPipelines::bindUnderlyingMaterial)
    void bindUnderlyingMaterial(gpu::Batch& batch);
    // this part's own standard inputs for a procedural (Procedural::prepare): one material on many models places each
    gpu::BufferPointer getProceduralInputs();
    gpu::BufferPointer _proceduralInputs;

    render::ItemKey _itemKey{ render::ItemKey::Builder::opaqueShape().build() };
    bool _cullWithParent{ false };
    uint64_t _created;
    static std::atomic<bool> _dumpingFrame;
    static std::atomic<int> _dumpFrame;
};

namespace render {
template <>
const ItemKey payloadGetKey(const MeshPartPayload::Pointer& payload);
template <>
const Item::Bound payloadGetBound(const MeshPartPayload::Pointer& payload);
template <>
const ShapeKey shapeGetShapeKey(const MeshPartPayload::Pointer& payload);
template <>
void payloadRender(const MeshPartPayload::Pointer& payload, RenderArgs* args);
}  // namespace render

class ModelMeshPartPayload : public MeshPartPayload {
public:
    ModelMeshPartPayload(ModelPointer model,
                         int meshIndex,
                         int partIndex,
                         int shapeIndex,
                         const Transform& transform,
                         const Transform& offsetTransform,
                         const uint64_t& created);

    typedef render::Payload<ModelMeshPartPayload> Payload;
    typedef Payload::DataPointer Pointer;

    void notifyLocationChanged() override;

    void updateKey(const render::ItemKey& key) override;

    // a part left out of every pass, its shadow too, until shown again (Model::setShapesHidden)
    void setHidden(bool hidden) { _hidden = hidden; }

    // matrix palette skinning
    void updateClusterBuffer(const std::vector<glm::mat4>& clusterMatrices);

    // dual quaternion skinning
    void updateClusterBuffer(const std::vector<Model::TransformDualQuaternion>& clusterDualQuaternions);

    // A skinned part's culling bound, from the clusters that place its vertices rather than from its mesh node: a
    // glTF armature exported at 1/100 scale left the node's bound a hundredth of the drawn body, at its feet.
    void updateSkinnedBound(const Transform& modelTransform, const std::vector<glm::mat4>& clusterMatrices);
    void updateSkinnedBound(const Transform& modelTransform,
                            const std::vector<Model::TransformDualQuaternion>& clusterDualQuaternions);

    // Render Item interface
    render::ShapeKey getShapeKey() const override;
    void render(RenderArgs* args) override;
    // GPU instancing (INSTANCING.md 3): settles the pass's level of detail and impostor choice, and says whether this
    // part can share a draw (its key: the mesh, the part and level, the material by content, the ShapeKey);
    // renderInstances then draws the part once for every transform of its group
    render::Instancing getInstancing(RenderArgs* args) override;
    void renderInstances(RenderArgs* args, const render::Transforms& transforms) override;

    void setShapeKey(bool invalidateShapeKey, PrimitiveMode primitiveMode, bool useDualQuaternionSkinning);
    void setCauterized(bool cauterized) { _cauterized = cauterized; }
    // the model's impostor switch (Model::setImpostorSwitch): this part draws only in the views where it says the
    // model shows rather than its card
    void setImpostorSwitch(const std::shared_ptr<ImpostorSwitch>& impostorSwitch) { _impostorSwitch = impostorSwitch; }

    // automatic LODs (AUTOMATIC-LODS.md 3.3): the part's triangles in full and in each level of its mesh, and the level
    // the main view draws now, 0 for the full mesh (read from the main thread by LODManager.describeEntity)
    int getFullTriangles() const { return (int)(_drawPart._numIndices / INDICES_PER_TRIANGLE); }
    std::vector<int> getLodTriangles() const;
    int getShownLodLevel() const { return _shownLodLevel.load(std::memory_order_relaxed); }

    // ModelMeshPartPayload functions to perform render
    void bindMesh(gpu::Batch& batch) override;
    void bindTransform(gpu::Batch& batch, RenderArgs::RenderMode renderMode) const override;

    gpu::BufferPointer _clusterBuffer;

    enum class ClusterBufferType
    {
        Matrices,
        DualQuaternions
    };
    ClusterBufferType _clusterBufferType{ ClusterBufferType::Matrices };

    int _meshIndex;
    bool _hidden { false };
    int _shapeID;
    uint32_t _deformerIndex;

    bool _isSkinned{ false };
    bool _isBlendShaped{ false };
    bool _hasTangents{ false };

    void setBlendshapeBuffer(const std::unordered_map<int, gpu::BufferPointer>& blendshapeBuffers,
                             const QVector<int>& blendedMeshSizes);

private:
    void initCache(const ModelPointer& model);

    gpu::BufferPointer _meshBlendshapeBuffer;
    int _meshNumVertices;
    render::ShapeKey _shapeKey{ render::ShapeKey::Builder::invalid() };
    render::ShapeKey _plainShapeKey{ render::ShapeKey::Builder::invalid() };   // _shapeKey without the shader's own pipeline (bindPlainPipeline)
    bool _prevUseDualQuaternionSkinning{ false };
    bool _cauterized{ false };
    std::shared_ptr<ImpostorSwitch> _impostorSwitch;

    static const uint32_t INDICES_PER_TRIANGLE { 3 };
    // the level a view draws: the coarsest whose error looks small enough from its camera; the shadow pass takes the
    // main view's, which keeps a hysteresis band
    int chooseLodLevel(const RenderArgs* args);
    std::vector<graphics::Mesh::Part> _lodParts;   // the part's range in each level of the mesh's LOD index buffer
    std::vector<float> _lodErrors;                 // each level's error, in the mesh's units
    int _mainViewLodLevel { 0 };
    std::atomic<int> _shownLodLevel { 0 };

    // GPU instancing: what getInstancing settled, and the pass it settled it for (render::Args::_instancingPass)
    uint32_t _settledPass { 0 };
    int _settledLodLevel { 0 };
    // the drawn material by content (INSTANCING.md 3.2), recomputed after the materials change; 0 until computed
    uint64_t _materialContentKey { 0 };
    bool _materialContentKeyDirty { true };
    QString _modelFileForLog;   // the model file's name, for the draw dump
};

namespace render {
template <>
const ItemKey payloadGetKey(const ModelMeshPartPayload::Pointer& payload);
template <>
const Item::Bound payloadGetBound(const ModelMeshPartPayload::Pointer& payload);
template <>
const ShapeKey shapeGetShapeKey(const ModelMeshPartPayload::Pointer& payload);
template <>
void payloadRender(const ModelMeshPartPayload::Pointer& payload, RenderArgs* args);
template <>
Instancing payloadGetInstancing(const ModelMeshPartPayload::Pointer& payload, RenderArgs* args);
template <>
void payloadRenderInstances(const ModelMeshPartPayload::Pointer& payload, RenderArgs* args, const Transforms& transforms);
}  // namespace render

#endif  // hifi_MeshPartPayload_h
