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

#include "ImpostorSwitch.h"

#include <QProcess>

#include <PerfStat.h>
#include <DualQuaternion.h>
#include <graphics/ShaderConstants.h>

#include "render-utils/ShaderConstants.h"
#include <procedural/Procedural.h>
#include "DeferredLightingEffect.h"

#include "RenderPipelines.h"
#include "MeshPartPayload.h"

#ifdef Q_OS_MACOS
bool MeshPartPayload::DEFAULT_ENABLE_MATERIAL_PROCEDURAL_SHADERS = false;
bool MeshPartPayload::enableMaterialProceduralShaders = false;
#else
bool MeshPartPayload::DEFAULT_ENABLE_MATERIAL_PROCEDURAL_SHADERS = true;
bool MeshPartPayload::enableMaterialProceduralShaders = true;
#endif

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
}

void MeshPartPayload::removeMaterial(graphics::MaterialPointer material) {
    _drawMaterials.remove(material);
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
    ShapeKey::Builder builder;
    graphics::MaterialPointer material = _drawMaterials.empty() ? nullptr : _drawMaterials.top().material;
    graphics::MaterialKey drawMaterialKey = _drawMaterials.getMaterialKey();

    if (drawMaterialKey.isTranslucent()) {
        builder.withTranslucent();
    }

    if (material && material->isProcedural() && material->isReady()) {
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
        if (enableMaterialProceduralShaders == false) return;
        auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(_drawMaterials.top().material);
        auto& schema = _drawMaterials.getSchemaBuffer().get<graphics::MultiMaterial::Schema>();
        glm::vec4 outColor = glm::vec4(ColorUtils::tosRGBVec3(schema._albedo), schema._opacity);
        outColor = procedural->getColor(outColor);
        procedural->prepare(batch, _worldFromLocalTransform.getTranslation(), _worldFromLocalTransform.getScale(),
                            _worldFromLocalTransform.getRotation(), _created, ProceduralProgramKey(outColor.a < 1.0f));
        batch._glColor4f(outColor.r, outColor.g, outColor.b, outColor.a);
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

void ModelMeshPartPayload::initCache(const ModelPointer& model) {
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

    // Once computed the cluster matrices, update the buffer(s)
    if (clusterMatrices.size() > 1) {
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

    // Once computed the cluster matrices, update the buffer(s)
    if (clusterDualQuaternions.size() > 1) {
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

// Note that this method is called for models but not for shapes
void ModelMeshPartPayload::updateKey(const render::ItemKey& key) {
    ItemKey::Builder builder(key);
    builder.withTypeShape();

    if (_isBlendShaped || _isSkinned) {
        builder.withDeformed();
    }

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

void ModelMeshPartPayload::setShapeKey(bool invalidateShapeKey, PrimitiveMode primitiveMode, bool useDualQuaternionSkinning) {
    if (invalidateShapeKey) {
        _shapeKey = ShapeKey::Builder::invalid();
        return;
    }

    if (_drawMaterials.shouldUpdate()) {
        RenderPipelines::updateMultiMaterial(_drawMaterials);
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

    if (material && material->isProcedural() && material->isReady()) {
        builder.withOwnPipeline();
    } else {
        bool hasTangents = drawMaterialKey.isNormalMap() && _hasTangents;
        bool hasLightmap = drawMaterialKey.isLightMap();
        bool isUnlit = drawMaterialKey.isUnlit();

        if (isWireframe) {
            hasTangents = hasLightmap = false;
        }

        builder.withMaterial();

        if (hasTangents) {
            builder.withTangents();
        }
        if (hasLightmap) {
            builder.withLightMap();
        }
        if (isUnlit) {
            builder.withUnlit();
        }
        if (material) {
            builder.withCullFaceMode(material->getCullFaceMode());
        }
    }

    _prevUseDualQuaternionSkinning = useDualQuaternionSkinning;
    _shapeKey = builder.build();
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
    // the model's impostor card stands in for it in this view, or the two dissolve into each other (IMPOSTORS.md 2.4)
    if (_impostorSwitch && !_impostorSwitch->evaluate(args).model) {
        return;
    }

    gpu::Batch& batch = *(args->_batch);

    bindTransform(batch, args->_renderMode);

    //Bind the index buffer and vertex buffer and Blend shapes if needed
    bindMesh(batch);
    // the automatic LOD this view draws (AUTOMATIC-LODS.md 3.3): a range of the mesh's LOD index buffer over the same
    // vertices
    const int lodLevel = chooseLodLevel(args);
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
        if (enableMaterialProceduralShaders == false || !sceneIsReady) {
            return;
        }
        auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(_drawMaterials.top().material);
        auto& schema = _drawMaterials.getSchemaBuffer().get<graphics::MultiMaterial::Schema>();
        glm::vec4 outColor = glm::vec4(ColorUtils::tosRGBVec3(schema._albedo), schema._opacity);
        outColor = procedural->getColor(outColor);
        procedural->prepare(batch, _worldFromLocalTransform.getTranslation(), _worldFromLocalTransform.getScale(),
                            _worldFromLocalTransform.getRotation(), _created,
                            ProceduralProgramKey(outColor.a < 1.0f, _shapeKey.isDeformed(), _shapeKey.isDualQuatSkinned()));
        batch._glColor4f(outColor.r, outColor.g, outColor.b, outColor.a);
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

    args->_details._trianglesRendered += drawPart._numIndices / INDICES_PER_TRIANGLE;
}

namespace {
    // the main view's hysteresis band on the error test, as the impostor switch's: a part steps down to a coarser
    // level where that level's error is a fifth under the tolerance, and back up where it is a fifth over
    const float LOD_BAND = 0.2f;
    // a caster's shadow takes this many times the view's error: a shadow shows a mesh's outline, softened by the
    // filtering, never its shading, so about a level coarser does
    const float SHADOW_LOD_ERROR_SCALE = 2.0f;
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
