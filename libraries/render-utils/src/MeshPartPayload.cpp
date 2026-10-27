//
//  MeshPartPayload.cpp
//  interface/src/renderer
//
//  Created by Sam Gateau on 10/3/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "MeshPartPayload.h"

#include "RenderUtilsLogging.h"

#include "ImpostorSwitch.h"

#include <QProcess>

#include <PerfStat.h>
#include <DualQuaternion.h>
#include <graphics/ShaderConstants.h>

#include "render-utils/ShaderConstants.h"
#include <procedural/Procedural.h>
#include <procedural/ShaderConstants.h>
#include "DeferredLightingEffect.h"

#include "RenderPipelines.h"
#include "MeshPartPayload.h"

// On everywhere (Settings > Custom Shaders on Models). It was off on macOS from 2020, when a procedural on a Mac read
// the wrong textures and inputs and drew its small mips black; with it off, a model whose material is a procedural
// draws nothing at all (Tivoli Town's ground)
bool MeshPartPayload::DEFAULT_ENABLE_MATERIAL_PROCEDURAL_SHADERS = true;
bool MeshPartPayload::enableMaterialProceduralShaders = true;

bool MeshPartPayload::sceneIsReady = false;

using namespace render;

namespace render {
template <>
const ItemKey payloadGetKey(const MeshPartPayload::Pointer& payload) {
    if (payload) {
        return payload->getKey();
    }
    return ItemKey::Builder::opaqueShape();  // for lack of a better idea
}

template <>
const Item::Bound payloadGetBound(const MeshPartPayload::Pointer& payload) {
    if (payload) {
        return payload->getBound();
    }
    return Item::Bound();
}

template <>
const ShapeKey shapeGetShapeKey(const MeshPartPayload::Pointer& payload) {
    if (payload) {
        return payload->getShapeKey();
    }
    return ShapeKey::Builder::invalid();
}

template <>
void payloadRender(const MeshPartPayload::Pointer& payload, RenderArgs* args) {
    return payload->render(args);
}
}  // namespace render

MeshPartPayload::MeshPartPayload(const std::shared_ptr<const graphics::Mesh>& mesh,
                                 int partIndex,
                                 graphics::MaterialPointer material,
                                 const uint64_t& created) :
    _created(created) {
    updateMeshPart(mesh, partIndex);
    addMaterial(graphics::MaterialLayer(material, 0));
}

void MeshPartPayload::updateMeshPart(const std::shared_ptr<const graphics::Mesh>& drawMesh, int partIndex) {
    _drawMesh = drawMesh;
    _partIndex = partIndex;   // was never stored (editor-renderer's find, 10-22): every part of a mesh read as part 0
    if (_drawMesh) {
        auto vertexFormat = _drawMesh->getVertexFormat();
        _hasColorAttrib = vertexFormat->hasAttribute(gpu::Stream::COLOR);
        _drawPart = _drawMesh->getPartBuffer().get<graphics::Mesh::Part>(partIndex);
        _localBound = _drawMesh->evalPartBound(partIndex);
    }
}

void MeshPartPayload::updateTransform(const Transform& transform) {
    _worldFromLocalTransform = transform;
    _worldBound = _localBound;
    _worldBound.transform(_worldFromLocalTransform);
}

void MeshPartPayload::updateTransformAndBound(const Transform& transform) {
    _worldBound = _localBound;
    _worldBound.transform(transform);
}

void MeshPartPayload::addMaterial(graphics::MaterialLayer material) {
    _drawMaterials.push(material);
    if (!material.material || !material.material->isProcedural()) {
        _underlyingMaterials.push(material);
    }
}

void MeshPartPayload::removeMaterial(graphics::MaterialPointer material) {
    _drawMaterials.remove(material);
    _underlyingMaterials.remove(material);
}

void MeshPartPayload::bindUnderlyingMaterial(gpu::Batch& batch) {
    if (_underlyingMaterials.shouldUpdate()) {
        RenderPipelines::updateMultiMaterial(_underlyingMaterials);
    }
    batch.setUniformBuffer(graphics::slot::buffer::Material, _underlyingMaterials.getSchemaBuffer());
    auto textures = _underlyingMaterials.getTextureTable()->getTextures();
    batch.setResourceTexture(graphics::slot::texture::MaterialAlbedo, textures[graphics::slot::texture::MaterialAlbedo]);
    batch.setResourceTexture(graphics::slot::texture::MaterialNormal, textures[graphics::slot::texture::MaterialNormal]);
    batch.setResourceTexture(procedural::slot::texture::UnderlyingRoughness, textures[graphics::slot::texture::MaterialRoughness]);
    batch.setResourceTexture(procedural::slot::texture::UnderlyingMetallic, textures[graphics::slot::texture::MaterialMetallic]);
    batch.setResourceTexture(procedural::slot::texture::UnderlyingOcclusion, textures[graphics::slot::texture::MaterialOcclusion]);
}

gpu::BufferPointer MeshPartPayload::getProceduralInputs() {
    if (!_proceduralInputs) {
        _proceduralInputs = Procedural::makeInputsBuffer();
    }
    return _proceduralInputs;
}

void MeshPartPayload::updateKey(const render::ItemKey& key) {
    ItemKey::Builder builder(key);
    builder.withTypeShape();

    if (_drawMaterials.shouldUpdate()) {
        RenderPipelines::updateMultiMaterial(_drawMaterials);
    }

    auto matKey = _drawMaterials.getMaterialKey();
    if (matKey.isTranslucent()) {
        builder.withTransparent();
    }

    if (_cullWithParent) {
        builder.withSubMetaCulled();
    }

    _itemKey = builder.build();
}

ItemKey MeshPartPayload::getKey() const {
    return _itemKey;
}

Item::Bound MeshPartPayload::getBound() const {
    graphics::MaterialPointer material = _drawMaterials.empty() ? nullptr : _drawMaterials.top().material;
    if (material && material->isProcedural() && material->isReady()) {
        auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(_drawMaterials.top().material);
        if (procedural->hasVertexShader() && procedural->hasBoundOperator()) {
            return procedural->getBound();
        }
    }
    return _worldBound;
}

ShapeKey MeshPartPayload::getShapeKey() const {
    return buildShapeKey(true);
}

ShapeKey MeshPartPayload::buildShapeKey(bool allowOwnPipeline) const {
    ShapeKey::Builder builder;
    graphics::MaterialPointer material = _drawMaterials.empty() ? nullptr : _drawMaterials.top().material;
    graphics::MaterialKey drawMaterialKey = _drawMaterials.getMaterialKey();

    if (drawMaterialKey.isTranslucent()) {
        builder.withTranslucent();
    }

    if (allowOwnPipeline && material && material->isProcedural() && material->isReady()) {
        builder.withOwnPipeline();
    } else {
        builder.withMaterial();

        if (drawMaterialKey.isNormalMap()) {
            builder.withTangents();
        }
        if (drawMaterialKey.isLightMap()) {
            builder.withLightMap();
        }
        if (drawMaterialKey.isUnlit()) {
            builder.withUnlit();
        }
        if (material) {
            builder.withCullFaceMode(material->getCullFaceMode());
        }
    }

    return builder.build();
}

bool MeshPartPayload::bindPlainPipeline(RenderArgs* args, const render::ShapeKey& plainKey) const {
    if (!args->_shapePlumber) {
        return false;
    }
    // the pass's own flags ride on the item's (renderShape ORs its global key in): a shadow pass's, the wireframe's
    render::ShapeKey key = plainKey | render::ShapeKey(render::ShapeKey::Flags(args->_itemShapeKey));
    key._flags.reset(render::ShapeKey::OWN_PIPELINE);
    // picking a pipeline sets it on the batch and runs its batch setter (the lights, in forward)
    return (bool)args->_shapePlumber->pickPipeline(args, key);
}

void MeshPartPayload::drawCall(gpu::Batch& batch) const {
    batch.drawIndexed(gpu::TRIANGLES, _drawPart._numIndices, _drawPart._startIndex);
}

void MeshPartPayload::bindMesh(gpu::Batch& batch) {
    batch.setIndexBuffer(gpu::UINT32, (_drawMesh->getIndexBuffer()._buffer), 0);

    batch.setInputFormat((_drawMesh->getVertexFormat()));

    batch.setInputStream(0, _drawMesh->getVertexStream());
}

void MeshPartPayload::bindTransform(gpu::Batch& batch, RenderArgs::RenderMode renderMode) const {
    batch.setModelTransform(_worldFromLocalTransform);
}

void MeshPartPayload::render(RenderArgs* args) {
    PerformanceTimer perfTimer("MeshPartPayload::render");

    if (!args) {
        return;
    }

    gpu::Batch& batch = *(args->_batch);

    // Bind the model transform and the skinCLusterMatrices if needed
    bindTransform(batch, args->_renderMode);

    //Bind the index buffer and vertex buffer and Blend shapes if needed
    bindMesh(batch);

    if (!_drawMaterials.empty() && _drawMaterials.top().material && _drawMaterials.top().material->isProcedural() &&
        _drawMaterials.top().material->isReady()) {
        auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(_drawMaterials.top().material);
        auto& schema = _drawMaterials.getSchemaBuffer().get<graphics::MultiMaterial::Schema>();
        glm::vec4 outColor = glm::vec4(ColorUtils::tosRGBVec3(schema._albedo), schema._opacity);
        outColor = procedural->getColor(outColor);
        const bool forward = DeferredLightingEffect::isForwardLit(args);
        const ProceduralProgramKey programKey(outColor.a < 1.0f, false, false, forward);
        // a shadow pass wants depth alone (see ModelMeshPartPayload::render)
        const bool plainShadow = args->_renderMode == RenderArgs::RenderMode::SHADOW_RENDER_MODE && !procedural->hasVertexShader();
        if (plainShadow || !enableMaterialProceduralShaders || procedural->compilationFailed(programKey)) {
            // the plain material underneath, through the standard pipeline (bindPlainPipeline)
            if (!bindPlainPipeline(args, buildShapeKey(false))) {
                return;
            }
            if (RenderPipelines::bindMaterials(_drawMaterials, batch, args->_renderMode, args->_enableTexturing)) {
                args->_details._materialSwitches++;
            }
        } else {
            bindUnderlyingMaterial(batch);
            procedural->prepare(batch, _worldFromLocalTransform.getTranslation(), _worldFromLocalTransform.getScale(),
                                _worldFromLocalTransform.getRotation(), _created, programKey,
                                getProceduralInputs());
            if (forward) {
                DependencyManager::get<DeferredLightingEffect>()->setupForwardLightingBatch(args, batch);
            }
            batch._glColor4f(outColor.r, outColor.g, outColor.b, outColor.a);
        }
    } else {
        // apply material properties
        if (RenderPipelines::bindMaterials(_drawMaterials, batch, args->_renderMode, args->_enableTexturing)) {
            args->_details._materialSwitches++;
        }
    }

    // Draw!
    {
        PerformanceTimer perfTimer("batch.drawIndexed()");
        drawCall(batch);
    }

    const int INDICES_PER_TRIANGLE = 3;
    args->_details._trianglesRendered += _drawPart._numIndices / INDICES_PER_TRIANGLE;
}

namespace render {
template <>
const ItemKey payloadGetKey(const ModelMeshPartPayload::Pointer& payload) {
    if (payload) {
        return payload->getKey();
    }
    return ItemKey::Builder::opaqueShape();  // for lack of a better idea
}

template <>
const Item::Bound payloadGetBound(const ModelMeshPartPayload::Pointer& payload) {
    if (payload) {
        return payload->getBound();
    }
    return Item::Bound();
}

template <>
const ShapeKey shapeGetShapeKey(const ModelMeshPartPayload::Pointer& payload) {
    if (payload) {
        return payload->getShapeKey();
    }
    return ShapeKey::Builder::invalid();
}

template <>
void payloadRender(const ModelMeshPartPayload::Pointer& payload, RenderArgs* args) {
    return payload->render(args);
}
template <>
Instancing payloadGetInstancing(const ModelMeshPartPayload::Pointer& payload, RenderArgs* args) {
    if (payload) {
        return payload->getInstancing(args);
    }
    return Instancing(Instancing::NONE);
}
template <>
void payloadRenderInstances(const ModelMeshPartPayload::Pointer& payload, RenderArgs* args, const Transforms& transforms) {
    if (payload) {
        payload->renderInstances(args, transforms);
    }
}

}  // namespace render

ModelMeshPartPayload::ModelMeshPartPayload(ModelPointer model,
                                           int meshIndex,
                                           int partIndex,
                                           int shapeIndex,
                                           const Transform& transform,
                                           const Transform& offsetTransform,
                                           const uint64_t& created) :
    _meshIndex(meshIndex),
    _shapeID(shapeIndex) {
    assert(model && model->isLoaded());

    auto shape = model->getHFMModel().shapes[shapeIndex];
    assert(shape.mesh == meshIndex);
    assert(shape.meshPart == partIndex);

    auto& modelMesh = model->getNetworkModel()->getMeshes().at(_meshIndex);
    _meshNumVertices = (int)modelMesh->getNumVertices();

    updateMeshPart(modelMesh, partIndex);
    for (const auto& lod : modelMesh->getLods()) {
        if (partIndex >= (int)lod.parts.size()) {
            break;
        }
        _lodParts.push_back(lod.parts[partIndex]);
        _lodErrors.push_back(lod.error);
    }

    Transform renderTransform = transform;
    const Model::ShapeState& shapeState = model->getShapeState(shapeIndex);
    renderTransform = transform.worldTransform(shapeState._rootFromJointTransform);
    updateTransform(renderTransform);

    _deformerIndex = shape.skinDeformer;

    initCache(model);
    _impostorSwitch = model->getImpostorSwitch();

#if defined(Q_OS_MACOS) || defined(Q_OS_ANDROID)
    // On mac AMD, we specifically need to have a _meshBlendshapeBuffer bound when using a deformed mesh pipeline
    // it cannot be null otherwise we crash in the drawcall using a deformed pipeline with a skinned only (not blendshaped) mesh
    if (_isBlendShaped) {
        std::vector<BlendshapeOffset> data(_meshNumVertices);
        const auto blendShapeBufferSize = _meshNumVertices * sizeof(BlendshapeOffset);
        _meshBlendshapeBuffer =
            std::make_shared<gpu::Buffer>(blendShapeBufferSize, reinterpret_cast<const gpu::Byte*>(data.data()),
                                          blendShapeBufferSize);
    } else if (_isSkinned) {
        BlendshapeOffset data;
        _meshBlendshapeBuffer =
            std::make_shared<gpu::Buffer>(sizeof(BlendshapeOffset), reinterpret_cast<const gpu::Byte*>(&data),
                                          sizeof(BlendshapeOffset));
    }
#endif

    _created = created;
}

std::atomic<int> MeshPartPayload::dumpDrawFrames { 0 };
std::atomic<bool> MeshPartPayload::_dumpingFrame { false };
std::atomic<int> MeshPartPayload::_dumpFrame { 0 };

void MeshPartPayload::beginFrame() {
    int left = dumpDrawFrames.load();
    if (left > 0) {
        dumpDrawFrames.store(left - 1);
        _dumpFrame.fetch_add(1);
        _dumpingFrame.store(true);
    } else {
        _dumpingFrame.store(false);
    }
}

namespace {
    void dumpDraw(const RenderArgs* args, const char* kind, const QString& file, const void* mesh, int part, int lod,
                  uint32_t numIndices, const render::Transforms& transforms) {
        if (!MeshPartPayload::dumping() || args->_renderMode != RenderArgs::RenderMode::DEFAULT_RENDER_MODE) {
            return;
        }
        QString at;
        for (const auto& transform : transforms) {
            const glm::vec3 t = transform.getTranslation();
            at += QString(at.isEmpty() ? "%1,%2,%3" : ";%1,%2,%3").arg(t.x, 0, 'f', 2).arg(t.y, 0, 'f', 2).arg(t.z, 0, 'f', 2);
        }
        qCDebug(renderutils).noquote() << QString("INSTANCING-DUMP frame=%1 kind=%2 file=%3 mesh=%4 part=%5 lod=%6 indices=%7 n=%8 at=%9")
            .arg(MeshPartPayload::dumpFrame()).arg(kind).arg(file).arg((quintptr)mesh, 0, 16).arg(part).arg(lod)
            .arg(numIndices).arg(transforms.size()).arg(at);
    }
}

void ModelMeshPartPayload::initCache(const ModelPointer& model) {
    if (model) {
        _modelFileForLog = model->getURL().fileName();
    }
    if (_drawMesh) {
        auto vertexFormat = _drawMesh->getVertexFormat();
        _hasColorAttrib = vertexFormat->hasAttribute(gpu::Stream::COLOR);
        if (_deformerIndex != hfm::UNDEFINED_KEY) {
            _isSkinned = vertexFormat->hasAttribute(gpu::Stream::SKIN_CLUSTER_WEIGHT) &&
                         vertexFormat->hasAttribute(gpu::Stream::SKIN_CLUSTER_INDEX);
        }

        const HFMModel& hfmModel = model->getHFMModel();
        const HFMMesh& mesh = hfmModel.meshes.at(_meshIndex);

        _isBlendShaped = !mesh.blendshapes.isEmpty();
        _hasTangents = !mesh.tangents.isEmpty();
    }

    auto networkMaterial = model->getNetworkModel()->getShapeMaterial(_shapeID);
    if (networkMaterial) {
        addMaterial(graphics::MaterialLayer(networkMaterial, 0));
    }
}

void ModelMeshPartPayload::notifyLocationChanged() {
}

void ModelMeshPartPayload::updateClusterBuffer(const std::vector<glm::mat4>& clusterMatrices) {
    // reset cluster buffer if we change the cluster buffer type
    if (_clusterBufferType != ClusterBufferType::Matrices) {
        _clusterBuffer.reset();
    }
    _clusterBufferType = ClusterBufferType::Matrices;

    // Once computed the cluster matrices, update the buffer(s); a mesh skinned to one joint has one (a VRM's head
    // pieces), and without it the shader read no buffer and drew the piece off the neck
    if (!clusterMatrices.empty()) {
        if (!_clusterBuffer) {
            _clusterBuffer = std::make_shared<gpu::Buffer>(clusterMatrices.size() * sizeof(glm::mat4),
                                                           (const gpu::Byte*)clusterMatrices.data());
        } else {
            _clusterBuffer->setSubData(0, clusterMatrices.size() * sizeof(glm::mat4), (const gpu::Byte*)clusterMatrices.data());
        }
    }
}

void ModelMeshPartPayload::updateClusterBuffer(const std::vector<Model::TransformDualQuaternion>& clusterDualQuaternions) {
    // reset cluster buffer if we change the cluster buffer type
    if (_clusterBufferType != ClusterBufferType::DualQuaternions) {
        _clusterBuffer.reset();
    }
    _clusterBufferType = ClusterBufferType::DualQuaternions;

    // Once computed the cluster matrices, update the buffer(s), for a mesh skinned to one joint too
    if (!clusterDualQuaternions.empty()) {
        if (!_clusterBuffer) {
            _clusterBuffer =
                std::make_shared<gpu::Buffer>(clusterDualQuaternions.size() * sizeof(Model::TransformDualQuaternion),
                                              (const gpu::Byte*)clusterDualQuaternions.data());
        } else {
            _clusterBuffer->setSubData(0, clusterDualQuaternions.size() * sizeof(Model::TransformDualQuaternion),
                                       (const gpu::Byte*)clusterDualQuaternions.data());
        }
    }
}

// The part's mesh-space bound carried by every cluster's skinning transform holds wherever the skin can put its
// vertices, in the model's frame; that includes a single cluster (a VRM head piece weighted to one joint draws at that
// joint). With no clusters the part draws unskinned at the model (updateClusterBuffer keeps no buffer then), so its
// bound is the mesh's own there.
void ModelMeshPartPayload::updateSkinnedBound(const Transform& modelTransform, const std::vector<glm::mat4>& clusterMatrices) {
    graphics::Box modelBound;
    if (!clusterMatrices.empty() && !_localBound.isInvalid()) {
        for (const auto& clusterMatrix : clusterMatrices) {
            graphics::Box clusterBound = _localBound;
            clusterBound.transform(clusterMatrix);
            modelBound += clusterBound;
        }
    }
    if (modelBound.isInvalid()) {
        modelBound = _localBound;
    }
    _worldBound = modelBound;
    _worldBound.transform(modelTransform);
}

void ModelMeshPartPayload::updateSkinnedBound(const Transform& modelTransform,
                                              const std::vector<Model::TransformDualQuaternion>& clusterDualQuaternions) {
    graphics::Box modelBound;
    if (!clusterDualQuaternions.empty() && !_localBound.isInvalid()) {
        for (const auto& clusterDualQuaternion : clusterDualQuaternions) {
            graphics::Box clusterBound = _localBound;
            clusterBound.transform(clusterDualQuaternion.getMatrix());
            modelBound += clusterBound;
        }
    }
    if (modelBound.isInvalid()) {
        modelBound = _localBound;
    }
    _worldBound = modelBound;
    _worldBound.transform(modelTransform);
}

// Note that this method is called for models but not for shapes
void ModelMeshPartPayload::updateKey(const render::ItemKey& key) {
    ItemKey::Builder builder(key);
    builder.withTypeShape();

    if (_isBlendShaped || _isSkinned) {
        builder.withDeformed();
    }

    if (_drawMaterials.shouldUpdate()) {
        RenderPipelines::updateMultiMaterial(_drawMaterials);
        _materialContentKeyDirty = true;
    }

    auto matKey = _drawMaterials.getMaterialKey();
    if (matKey.isTranslucent()) {
        builder.withTransparent();
    }

    if (_cullWithParent) {
        builder.withSubMetaCulled();
    }

    if (_hidden) {
        builder.withInvisible();
    }

    _itemKey = builder.build();
}

void ModelMeshPartPayload::setShapeKey(bool invalidateShapeKey, PrimitiveMode primitiveMode, bool useDualQuaternionSkinning) {
    if (invalidateShapeKey) {
        _shapeKey = ShapeKey::Builder::invalid();
        return;
    }

    if (_drawMaterials.shouldUpdate()) {
        RenderPipelines::updateMultiMaterial(_drawMaterials);
        _materialContentKeyDirty = true;
    }

    ShapeKey::Builder builder;
    graphics::MaterialPointer material = _drawMaterials.empty() ? nullptr : _drawMaterials.top().material;
    graphics::MaterialKey drawMaterialKey = _drawMaterials.getMaterialKey();

    bool isWireframe = primitiveMode == PrimitiveMode::LINES;

    if (isWireframe) {
        builder.withWireframe();
    } else if (drawMaterialKey.isTranslucent()) {
        builder.withTranslucent();
    }

    if (_isSkinned || (_isBlendShaped && _meshBlendshapeBuffer)) {
        builder.withDeformed();
        if (useDualQuaternionSkinning) {
            builder.withDualQuatSkinned();
        }
    }

    // the plain key, the part's whether or not its material has a custom shader (bindPlainPipeline draws with it when
    // the shader can't)
    ShapeKey::Builder plain(builder);
    {
        bool hasTangents = drawMaterialKey.isNormalMap() && _hasTangents;
        bool hasLightmap = drawMaterialKey.isLightMap();
        bool isUnlit = drawMaterialKey.isUnlit();

        if (isWireframe) {
            hasTangents = hasLightmap = false;
        }

        plain.withMaterial();

        if (hasTangents) {
            plain.withTangents();
        }
        if (hasLightmap) {
            plain.withLightMap();
        }
        if (isUnlit) {
            plain.withUnlit();
        }
        if (material) {
            plain.withCullFaceMode(material->getCullFaceMode());
        }
    }
    _plainShapeKey = plain.build();

    if (material && material->isProcedural() && material->isReady()) {
        builder.withOwnPipeline();
        _shapeKey = builder.build();
    } else {
        _shapeKey = _plainShapeKey;
    }
    _prevUseDualQuaternionSkinning = useDualQuaternionSkinning;
}

ShapeKey ModelMeshPartPayload::getShapeKey() const {
    // during the dissolve into or out of the model's impostor card the part draws through its fade pipeline, which
    // clips it by the dissolve's noise (ImpostorSwitch, IMPOSTORS.md 2.4)
    if (_impostorSwitch && _impostorSwitch->isFading() && _shapeKey.isValid() && !_shapeKey.hasOwnPipeline()) {
        return ShapeKey::Builder(_shapeKey).withFade().build();
    }
    return _shapeKey;
}

void ModelMeshPartPayload::bindMesh(gpu::Batch& batch) {
    batch.setIndexBuffer(gpu::UINT32, (_drawMesh->getIndexBuffer()._buffer), 0);
    batch.setInputFormat((_drawMesh->getVertexFormat()));
    if (_meshBlendshapeBuffer) {
        batch.setResourceBuffer(0, _meshBlendshapeBuffer);
    }
    batch.setInputStream(0, _drawMesh->getVertexStream());
}

void ModelMeshPartPayload::bindTransform(gpu::Batch& batch, RenderArgs::RenderMode renderMode) const {
    if (_clusterBuffer) {
        batch.setUniformBuffer(graphics::slot::buffer::Skinning, _clusterBuffer);
    }
    batch.setModelTransform(_worldFromLocalTransform);
}

void ModelMeshPartPayload::render(RenderArgs* args) {
    PerformanceTimer perfTimer("ModelMeshPartPayload::render");

    if (!args || (args->_renderMode == RenderArgs::RenderMode::DEFAULT_RENDER_MODE && _cauterized)) {
        return;
    }
    // hidden (Model::setShapesHidden): the invisible key keeps a part out of the scene's own fetch, but a group's
    // parts (an avatar's) reach the passes through their parent (MetaToSubItems), whatever their key
    if (_hidden) {
        return;
    }
    // settled by getInstancing for this pass (GPU instancing): the impostor choice and the level were made then
    const bool settled = _settledPass != 0 && _settledPass == args->_instancingPass;
    // the model's impostor card stands in for it in this view, or the two dissolve into each other (IMPOSTORS.md 2.4)
    if (!settled && _impostorSwitch && !_impostorSwitch->evaluate(args).model) {
        return;
    }

    gpu::Batch& batch = *(args->_batch);

    bindTransform(batch, args->_renderMode);

    //Bind the index buffer and vertex buffer and Blend shapes if needed
    bindMesh(batch);
    // the automatic LOD this view draws (AUTOMATIC-LODS.md 3.3): a range of the mesh's LOD index buffer over the same
    // vertices
    const int lodLevel = settled ? _settledLodLevel : chooseLodLevel(args);
    const graphics::Mesh::Part& drawPart = lodLevel > 0 ? _lodParts[lodLevel - 1] : _drawPart;
    if (lodLevel > 0) {
        batch.setIndexBuffer(gpu::UINT32, _drawMesh->getLodIndexBuffer()._buffer, 0);
    }

    // IF deformed pass the mesh key
    auto drawcallInfo = (uint16_t)(((_isBlendShaped && _meshBlendshapeBuffer && args->_enableBlendshape) << 0) |
                                   ((_isSkinned && args->_enableSkinning) << 1));
    if (drawcallInfo) {
        batch.setDrawcallUniform(drawcallInfo);
    }

    if (_shapeKey.hasOwnPipeline()) {
        auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(_drawMaterials.top().material);
        auto& schema = _drawMaterials.getSchemaBuffer().get<graphics::MultiMaterial::Schema>();
        glm::vec4 outColor = glm::vec4(ColorUtils::tosRGBVec3(schema._albedo), schema._opacity);
        outColor = procedural->getColor(outColor);
        const bool forward = DeferredLightingEffect::isForwardLit(args);
        const ProceduralProgramKey programKey(outColor.a < 1.0f, _shapeKey.isDeformed(), _shapeKey.isDualQuatSkinned(), forward);
        // a shadow pass wants depth alone: a part whose shader only colours its pixels casts its mesh's shadow through
        // the standard shadow pipeline (one pipeline for all such parts, no shader setup a draw); a shader that moves
        // vertices keeps its own program there, so its shadow follows the moved shape. 20 Lightlings' three neon parts
        // in four cascades were 240 full-shader draws a frame, each with Procedural::prepare (performance's crowd run,
        // 2026-10-28)
        const bool plainShadow = args->_renderMode == RenderArgs::RenderMode::SHADOW_RENDER_MODE && !procedural->hasVertexShader();
        if (plainShadow || !enableMaterialProceduralShaders || !sceneIsReady || procedural->compilationFailed(programKey)) {
            // the plain material underneath, through the standard pipeline (bindPlainPipeline): the part shows, and
            // the shader takes over when it can
            if (!bindPlainPipeline(args, _plainShapeKey)) {
                return;
            }
            if (RenderPipelines::bindMaterials(_drawMaterials, batch, args->_renderMode, args->_enableTexturing)) {
                args->_details._materialSwitches++;
            }
        } else {
            bindUnderlyingMaterial(batch);
            procedural->prepare(batch, _worldFromLocalTransform.getTranslation(), _worldFromLocalTransform.getScale(),
                                _worldFromLocalTransform.getRotation(), _created, programKey,
                                getProceduralInputs());
            if (forward) {
                DependencyManager::get<DeferredLightingEffect>()->setupForwardLightingBatch(args, batch);
            }
            batch._glColor4f(outColor.r, outColor.g, outColor.b, outColor.a);
        }
    } else {
        // apply material properties
        if (RenderPipelines::bindMaterials(_drawMaterials, batch, args->_renderMode, args->_enableTexturing)) {
            args->_details._materialSwitches++;
        }
    }
    if (_impostorSwitch) {
        // the dissolve's threshold for the fade pipeline the key asked for (the fade effect's own item setter binds
        // nothing for an item without a transition of the scene's)
        batch.setUniformBuffer(render_utils::slot::buffer::FadeObjectParameters, _impostorSwitch->fadeParams(args));
    }

    // Draw!
    {
        PerformanceTimer perfTimer("batch.drawIndexed()");
        batch.drawIndexed(gpu::TRIANGLES, drawPart._numIndices, drawPart._startIndex);
    }
    if (MeshPartPayload::dumping()) {
        dumpDraw(args, "single", _modelFileForLog, _drawMesh.get(), _partIndex, lodLevel, drawPart._numIndices, { _worldFromLocalTransform });
    }

    args->_details._trianglesRendered += drawPart._numIndices / INDICES_PER_TRIANGLE;
}

namespace {
    // the main view's hysteresis band on the error test, as the impostor switch's: a part steps down to a coarser
    // level where that level's error is a fifth under the tolerance, and back up where it is a fifth over
    const float LOD_BAND = 0.2f;
    // a caster's shadow takes this many times the view's error: a shadow shows a mesh's outline, softened by the
    // filtering, never its shading, so about a level coarser does
    const float SHADOW_LOD_ERROR_SCALE = 2.0f;

    // the drawn material by content (INSTANCING.md 3.2): its schema's bytes, its textures and its cull mode, hashed
    // (FNV-1a); two parts with equal keys bind the same uniform block and texture table, so one may draw for both
    uint64_t materialContentKey(graphics::MultiMaterial& materials) {
        uint64_t hash = 1469598103934665603ULL;
        auto mix = [&hash](const void* data, size_t size) {
            const uint8_t* bytes = (const uint8_t*)data;
            for (size_t i = 0; i < size; i++) {
                hash ^= bytes[i];
                hash *= 1099511628211ULL;
            }
        };
        const auto& schema = materials.getSchemaBuffer();
        if (schema._buffer) {
            mix(schema._buffer->getData() + schema._offset, schema._size);
        }
        if (materials.getTextureTable()) {
            for (const auto& texture : materials.getTextureTable()->getTextures()) {
                const void* pointer = texture.get();
                mix(&pointer, sizeof(pointer));
            }
        }
        auto cullFaceMode = materials.getCullFaceMode();
        mix(&cullFaceMode, sizeof(cullFaceMode));
        return hash == 0 ? 1 : hash;
    }
}

render::Instancing ModelMeshPartPayload::getInstancing(RenderArgs* args) {
    render::Instancing answer(render::Instancing::NONE);
    if (!args || (args->_renderMode == RenderArgs::RenderMode::DEFAULT_RENDER_MODE && _cauterized) || _hidden) {
        return answer;
    }
    if (_impostorSwitch && !_impostorSwitch->evaluate(args).model) {
        return answer;
    }
    // settled for this pass: the level this view draws, chosen once (the main view's hysteresis moves once a frame)
    _settledLodLevel = chooseLodLevel(args);
    _settledPass = args->_instancingPass;
    answer.draw = render::Instancing::ALONE;
    answer.transform = _worldFromLocalTransform;

    // what draws on its own (INSTANCING.md 3.1): translucent (its order), deformed (its skinning buffer), fading (its
    // dissolve), procedural (its pipeline), lightmapped (its texture transform, until 3.5)
    const auto shapeKey = getShapeKey();
    if (!shapeKey.isValid() || shapeKey.hasOwnPipeline() || shapeKey.isTranslucent() || shapeKey.isDeformed() ||
        shapeKey.isFaded() || shapeKey.hasLightMap() || !_drawMesh || _drawMaterials.empty()) {
        return answer;
    }
    if (_drawMaterials.shouldUpdate()) {
        RenderPipelines::updateMultiMaterial(_drawMaterials);
        _materialContentKeyDirty = true;
    }
    if (_materialContentKeyDirty) {
        _materialContentKey = materialContentKey(_drawMaterials);
        _materialContentKeyDirty = false;
    }
    answer.draw = render::Instancing::SHARED;
    answer.key.mesh = _drawMesh.get();
    answer.key.material = _materialContentKey;
    // the range the draw takes at this level, which is what the group's draw binds for every member
    const graphics::Mesh::Part& drawPart = _settledLodLevel > 0 ? _lodParts[_settledLodLevel - 1] : _drawPart;
    answer.key.range = ((uint64_t)drawPart._startIndex << 32) | (uint64_t)drawPart._numIndices;
    answer.key.part = ((uint32_t)(_partIndex & 0xffffff) << 8) | (uint32_t)(_settledLodLevel & 0xff);
    answer.key.shape = (uint32_t)shapeKey._flags.to_ulong();
    return answer;
}

void ModelMeshPartPayload::renderInstances(RenderArgs* args, const render::Transforms& transforms) {
    PerformanceTimer perfTimer("ModelMeshPartPayload::renderInstances");
    if (!args || !args->_batch || transforms.empty()) {
        return;
    }
    gpu::Batch& batch = *(args->_batch);
    // settled by getInstancing for this pass: every member of the group answered the same key, so the mesh, the part
    // and level, the material and the pipeline are this part's; only the transforms differ
    const bool settled = _settledPass != 0 && _settledPass == args->_instancingPass;
    const int lodLevel = settled ? _settledLodLevel : chooseLodLevel(args);

    bindMesh(batch);
    const graphics::Mesh::Part& drawPart = lodLevel > 0 ? _lodParts[lodLevel - 1] : _drawPart;
    if (lodLevel > 0) {
        batch.setIndexBuffer(gpu::UINT32, _drawMesh->getLodIndexBuffer()._buffer, 0);
    }
    if (RenderPipelines::bindMaterials(_drawMaterials, batch, args->_renderMode, args->_enableTexturing)) {
        args->_details._materialSwitches++;
    }
    if (_impostorSwitch) {
        batch.setUniformBuffer(render_utils::slot::buffer::FadeObjectParameters, _impostorSwitch->fadeParams(args));
    }
    {
        PerformanceTimer perfTimer("batch.drawIndexedInstances()");
        batch.drawIndexedInstances(gpu::TRIANGLES, drawPart._numIndices, drawPart._startIndex, transforms);
    }
    if (MeshPartPayload::dumping()) {
        dumpDraw(args, "group", _modelFileForLog, _drawMesh.get(), _partIndex, lodLevel, drawPart._numIndices, transforms);
    }
    args->_details._trianglesRendered += (int)(drawPart._numIndices / INDICES_PER_TRIANGLE) * (int)transforms.size();
    args->_details._instancedDrawcalls++;
    args->_details._instancesDrawn += (int)transforms.size();
}

int ModelMeshPartPayload::chooseLodLevel(const RenderArgs* args) {
    const int levels = (int)_lodParts.size();
    if (levels == 0) {
        return 0;
    }
    const bool shadow = args->_renderMode == RenderArgs::RenderMode::SHADOW_RENDER_MODE;
    if (shadow && (args->_meshLodForceLevel < 0 && args->_shadowLodPixelsPerRadian <= 0.0f)) {
        return _mainViewLodLevel;   // no camera to choose from: as the main view last drew it
    }
    const bool mainView = args->_renderMode == RenderArgs::RenderMode::DEFAULT_RENDER_MODE;
    int level = 0;
    if (args->_meshLodForceLevel >= 0) {
        level = std::min(args->_meshLodForceLevel, levels);
    } else if (args->_meshLodErrorPixels > 0.0f && args->hasViewFrustum()) {
        // a shadow pass's view is the light's: its casters choose from the camera the shadow is seen from, every
        // caster from where it is, in view or not (one out of view kept full detail before, 2026-10-21)
        const ViewFrustum& frustum = args->getViewFrustum();
        glm::vec3 eye = shadow ? args->_shadowLodEye : frustum.getPosition();
        glm::vec3 nearest = glm::clamp(eye, _worldBound.getMinimumPoint(), _worldBound.getMaximumPoint());
        float distance = glm::distance(eye, nearest);   // 0 inside the part's box: full detail
        // the view's pixels per radian at its centre, and the part's scale from its mesh's units to the world's
        float pixelsPerRadian = shadow ? args->_shadowLodPixelsPerRadian
            : 0.5f * (float)args->_viewport.w * frustum.getProjection()[1][1];
        glm::vec3 scale = glm::abs(_worldFromLocalTransform.getScale());
        float worldPerUnit = std::max(scale.x, std::max(scale.y, scale.z));
        // the coarsest level, going from the finest, whose error looks no bigger than the tolerance: a level's error in
        // pixels is its error in the world over the distance, times the pixels per radian
        auto coarsestWithin = [&](float tolerance) {
            int chosen = 0;
            for (int i = 0; i < levels; i++) {
                if (_lodErrors[i] * worldPerUnit * pixelsPerRadian > tolerance * distance) {
                    break;
                }
                chosen = i + 1;
            }
            return chosen;
        };
        if (mainView) {
            int stepDownTo = coarsestWithin(args->_meshLodErrorPixels * (1.0f - LOD_BAND));
            int coarsestAllowed = coarsestWithin(args->_meshLodErrorPixels * (1.0f + LOD_BAND));
            level = glm::clamp(_mainViewLodLevel, stepDownTo, coarsestAllowed);
        } else if (shadow) {
            level = coarsestWithin(args->_meshLodErrorPixels * SHADOW_LOD_ERROR_SCALE);
        } else {
            level = coarsestWithin(args->_meshLodErrorPixels);   // mirrors and the secondary camera: no band
        }
    }
    if (mainView) {
        _mainViewLodLevel = level;
        _shownLodLevel.store(level, std::memory_order_relaxed);
    }
    return level;
}

std::vector<int> ModelMeshPartPayload::getLodTriangles() const {
    std::vector<int> triangles;
    for (const auto& part : _lodParts) {
        triangles.push_back((int)(part._numIndices / INDICES_PER_TRIANGLE));
    }
    return triangles;
}

void ModelMeshPartPayload::setBlendshapeBuffer(const std::unordered_map<int, gpu::BufferPointer>& blendshapeBuffers,
                                               const QVector<int>& blendedMeshSizes) {
    if (_meshIndex < blendedMeshSizes.length() && blendedMeshSizes.at(_meshIndex) == _meshNumVertices) {
        auto blendshapeBuffer = blendshapeBuffers.find(_meshIndex);
        if (blendshapeBuffer != blendshapeBuffers.end()) {
            _meshBlendshapeBuffer = blendshapeBuffer->second;
            if (_isSkinned || (_isBlendShaped && _meshBlendshapeBuffer)) {
                ShapeKey::Builder builder(_shapeKey);
                builder.withDeformed();
                if (_prevUseDualQuaternionSkinning) {
                    builder.withDualQuatSkinned();
                }
                _shapeKey = builder.build();
            }
        }
    }
}
