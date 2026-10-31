//
//  RenderableShapeEntityItem (Austin)
//  interface/src
//

#include "RenderableShapeEntityItem.h"

#include <mutex>

#include <glm/gtx/quaternion.hpp>

#include <gpu/Batch.h>
#include <DependencyManager.h>
#include <DeferredLightingEffect.h>
#include <GeometryCache.h>
#include <Mirrors.h>
#include <StencilMaskPass.h>
#include <shaders/Shaders.h>
#include "entities-renderer/ShaderConstants.h"
#include "EntitiesRendererLogging.h"
#include <Transform.h>
#include <shared/Shapes.h>
#include <graphics/TextureMap.h>
#include <PerfStat.h>

#include "RenderPipelines.h"

using namespace render;
using namespace render::entities;

// Sphere entities should fit inside a cube entity of the same size, so a sphere that has dimensions 1x1x1
// is a half unit sphere.  However, the geometry cache renders a UNIT sphere, so we need to scale down.
static const float SPHERE_ENTITY_SCALE = 0.5f;

ShapeEntityRenderer::ShapeEntityRenderer(const EntityItemPointer& entity) : Parent(entity) {
    addMaterial(graphics::MaterialLayer(_material, 0), "0");
}

namespace {
    // where a Picture mirror's glass reads its picture. A box's faces share one texture laid out as a cross
    // (geometry::cube(), a cell a face; Quad is the same box flattened), so the +Z face's cell is stretched over the
    // whole picture, flipped across (the picture's left is the face's right, as in a mirror) and over (the cell's
    // rows run top-down, a rendered picture's bottom-up). Without the stretch the glass showed a quarter of the
    // picture's width and a third of its height, magnified and off to the right.
    Transform mirrorPictureTransform() {
        const auto& cube = geometry::cube();
        const size_t N = 4;   // corners a face
        glm::vec2 topLeft(0.0f);
        glm::vec2 bottomRight(1.0f);
        for (size_t f = 0; f < cube.faces.size(); ++f) {
            const auto& face = cube.faces[f];
            bool front = true;
            for (size_t i = 0; i < N; ++i) {
                front = front && cube.vertices[face[i]].z > 0.0f;
            }
            if (!front) {
                continue;
            }
            for (size_t i = 0; i < N; ++i) {
                const glm::vec3& corner = cube.vertices[face[i]];
                if (corner.x < 0.0f && corner.y > 0.0f) {
                    topLeft = cube.texCoords[f * N + i];
                } else if (corner.x > 0.0f && corner.y < 0.0f) {
                    bottomRight = cube.texCoords[f * N + i];
                }
            }
            break;
        }
        // the picture's (u, v) = (1 - across, 1 - down), across and down running 0 to 1 over the cell from its top left
        glm::vec2 size = bottomRight - topLeft;
        Transform transform;
        transform.setScale(glm::vec3(-1.0f / size.x, -1.0f / size.y, 1.0f));
        transform.setTranslation(glm::vec3(1.0f + topLeft.x / size.x, 1.0f + topLeft.y / size.y, 0.0f));
        return transform;
    }

    // the glass of a screen-mode mirror: its picture by screen position, unlit, into the G-buffer or, on the forward
    // path, straight to the colour
    gpu::PipelinePointer screenMirrorPipeline(bool forward) {
        static gpu::PipelinePointer pipelines[2];
        static std::once_flag once;
        std::call_once(once, [] {
            namespace program = shader::entities_renderer::program;
            for (int i = 0; i < 2; ++i) {
                auto shaderProgram = gpu::Shader::createProgram(i ? program::mirror_screen_forward : program::mirror_screen);
                auto state = std::make_shared<gpu::State>();
                state->setCullMode(gpu::State::CULL_BACK);
                state->setDepthTest(true, true, gpu::GREATER_EQUAL);
                PrepareStencil::testMaskDrawShape(*state);
                pipelines[i] = gpu::Pipeline::create(shaderProgram, state);
            }
        });
        return pipelines[forward ? 1 : 0];
    }
}

ShapeEntityRenderer::~ShapeEntityRenderer() {
    if (_mirror) {
        Mirrors::instance().removeSurface(_mirrorId);
    }
}

void ShapeEntityRenderer::onRemoveFromSceneTyped(const TypedEntityPointer& entity) {
    withWriteLock([&] {
        removeReflectionProbe();
    });
}

void ShapeEntityRenderer::removeReflectionProbe() {
    if (_reflectionProbeStage && !ReflectionProbeStage::isIndexInvalid(_reflectionProbeIndex)) {
        _reflectionProbeStage->removeProbe(_reflectionProbeIndex);
    }
    _reflectionProbeIndex = ReflectionProbeStage::INVALID_INDEX;
    _reflectionProbe.reset();
    _reflectionProbeTexture.reset();
    _reflectionProbeURL.clear();
    _reflectionProbePending = false;
}

bool ShapeEntityRenderer::needsRenderUpdate() const {
    if (_visible != wasPreviouslyVisible())
        return true;

    if (resultWithReadLock<bool>([&] {
            auto mat = _materials.find("0");
            if (mat != _materials.end() && mat->second.top().material && mat->second.top().material->isProcedural() &&
                mat->second.top().material->isReady()) {
                auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(mat->second.top().material);
                if (procedural->isFading()) {
                    return true;
                }
            }

            if (mat != _materials.end() && mat->second.shouldUpdate()) {
                return true;
            }

            // the reflection probe's cube map has loaded: hand it over
            if (_reflectionProbePending && _reflectionProbeTexture && _reflectionProbeTexture->isLoaded()) {
                return true;
            }

            return false;
        })) {
        return true;
    }

    return Parent::needsRenderUpdate();
}

bool ShapeEntityRenderer::needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const {
    // the zone-culling visibility is compared by needsRenderUpdate, not here: this says only whether the entity
    // itself has changed (EntityRenderer::entityHasChanged)
    if (_dimensions != entity->getScaledDimensions()) {
        return true;
    }

    if (_proceduralData != entity->getUserData()) {
        return true;
    }
    if (_mirror && !_mirrorScreen && Mirrors::instance().generation(_mirrorId) != _mirrorGeneration) {
        return true;   // its picture has changed hands or size (Screen mode takes the frame's picture when it draws)
    }

    return false;
}

void ShapeEntityRenderer::doRenderUpdateSynchronousTyped(const ScenePointer& scene,
                                                         Transaction& transaction,
                                                         const TypedEntityPointer& entity) {
    withWriteLock([&] {
        _shape = entity->getShape();
        _pulseProperties = entity->getPulseProperties();
        if (!_reflectionProbeStage) {
            _reflectionProbeStage = scene->getStage<ReflectionProbeStage>();
        }
    });

    void* key = (void*)this;
    AbstractViewStateInterface::instance()->pushPostUpdateLambda(key, [this, entity]() {
        withWriteLock([&] {
            _position = entity->getWorldPosition();
            _dimensions = entity->getUnscaledDimensions();  // get unscaled to avoid scaling twice
            _orientation = entity->getWorldOrientation();
            _renderTransform = getModelTransform();  // contains parent scale, if this entity scales with its parent
            if (_shape == entity::Sphere) {
                _renderTransform.postScale(SPHERE_ENTITY_SCALE);
            }

            _renderTransform.postScale(_dimensions);
            if (_mirror) {
                Mirrors::instance().setSurface(entity->getID(), { _position, _orientation, entity->getScaledDimensions(),
                                                                  _mirrorScreen ? Mirrors::Mode::Screen : Mirrors::Mode::Picture });
            }
            if (_reflectionProbe) {
                // the probe moves and its box resizes with the entity
                _reflectionProbe->position = _position;
                _reflectionProbe->orientation = _orientation;
                _reflectionProbe->boxSize = entity->getScaledDimensions();
                _reflectionProbe->touch();
            }
        });
    });
}

void ShapeEntityRenderer::doRenderUpdateAsynchronousTyped(const TypedEntityPointer& entity) {
    withReadLock([&] {
        auto mat = _materials.find("0");
        if (mat != _materials.end() && mat->second.top().material && mat->second.top().material->isProcedural() &&
            mat->second.top().material->isReady()) {
            auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(mat->second.top().material);
            if (procedural->isFading()) {
                procedural->setIsFading(Interpolate::calculateFadeRatio(procedural->getFadeStartTime()) < 1.0f);
            }
        }
    });

    withWriteLock([&] {
        bool materialChanged = false;
        glm::vec3 color = toGlm(entity->getColor());
        if (_color != color) {
            _color = color;
            // a Picture mirror shows its picture as light over black, nothing lit; a Screen mirror keeps its colour
            // for when it has no picture (a mirror's view, no slot this frame)
            _material->setAlbedo(_mirrorShown && !_mirrorScreen ? glm::vec3(0.0f) : color);
            materialChanged = true;
        }

        float alpha = entity->getAlpha();
        if (_alpha != alpha) {
            _alpha = alpha;
            _material->setOpacity(alpha);
            materialChanged = true;
        }

        auto userData = entity->getUserData();
        if (_proceduralData != userData) {
            _proceduralData = userData;
            _material->setProceduralData(_proceduralData);
            materialChanged = true;

            auto userDataObject = QJsonDocument::fromJson(userData.toUtf8()).object();
            if (userDataObject.contains("billboardMode")) {
                auto billboardMode = userDataObject["billboardMode"].toString();
                if (billboardMode == "full") {
                    _billboardMode = BillboardMode::FULL;
                } else if (billboardMode == "yaw") {
                    _billboardMode = BillboardMode::YAW;
                } else {
                    _billboardMode = BillboardMode::NONE;
                }
            }
            // a mirror: the entry "picture" (or true, or "true", as the editor wrote it at first) or "screen"
            QJsonValue mirrorEntry = userDataObject.value("mirror");
            QString mirrorText = mirrorEntry.toBool() ? QStringLiteral("picture") : mirrorEntry.toString().toLower();
            bool mirror = mirrorText == "screen" || mirrorText == "picture" || mirrorText == "true";
            // in a headset a Screen mirror is drawn as a Picture one: the Screen method reads its picture by its place
            // in the flat screen's view, which a stereo picture (two eyes side by side) has none of, and its view is the
            // whole stereo buffer re-rendered each frame (Caity's Quest 3, qa-town, 2026-10-28: streaks). doRender asks
            // for this update again when the display changes
            _mirrorScreenWanted = mirrorText == "screen";
            bool mirrorScreen = _mirrorScreenWanted && !AbstractViewStateInterface::instance()->isHMDMode();
            if (mirror != _mirror || mirrorScreen != _mirrorScreen) {
                if (_mirror) {
                    // out of the old mode: the material its own again
                    Mirrors::instance().removeSurface(_mirrorId);
                    _material->setTextureMap(graphics::MaterialKey::EMISSIVE_MAP, nullptr);
                    _material->setTexCoordTransform(0, glm::mat4());
                    _material->setEmissive(glm::vec3(0.0f), false);
                    _material->setAlbedo(_color);
                    _mirrorShown = false;
                    _mirrorPicture = nullptr;
                    materialChanged = true;
                }
                _mirror = mirror;
                _mirrorScreen = mirrorScreen;
                _mirrorId = entity->getID();
                _mirrorGeneration = 0;
                if (_mirror) {
                    Mirrors::instance().setSurface(_mirrorId, { entity->getWorldPosition(), entity->getWorldOrientation(), entity->getScaledDimensions(),
                                                               _mirrorScreen ? Mirrors::Mode::Screen : Mirrors::Mode::Picture });
                }
            }
            // a reflection probe: the entry "reflectionProbe" { url, boxOffset [x, y, z], intensity, importance,
            // blendDistance, boxProjection } (or true for one with the defaults); the entity is its box
            QJsonValue probeEntry = userDataObject.value("reflectionProbe");
            if (probeEntry.isObject() || probeEntry.toBool()) {
                QJsonObject entry = probeEntry.toObject();
                if (!_reflectionProbe) {
                    _reflectionProbe = std::make_shared<graphics::ReflectionProbe>();
                }
                auto& probe = *_reflectionProbe;
                probe.position = entity->getWorldPosition();
                probe.orientation = entity->getWorldOrientation();
                probe.boxSize = entity->getScaledDimensions();
                QJsonArray offset = entry.value("boxOffset").toArray();
                probe.boxOffset = offset.size() == 3 ? glm::vec3(offset[0].toDouble(), offset[1].toDouble(), offset[2].toDouble()) : glm::vec3(0.0f);
                probe.intensity = (float)entry.value("intensity").toDouble(1.0);
                probe.importance = entry.value("importance").toInt(0);
                probe.blendDistance = (float)std::max(0.0, entry.value("blendDistance").toDouble(1.0));
                probe.boxProjection = entry.value("boxProjection").toBool(true);
                QString url = entry.value("url").toString();
                if (url != _reflectionProbeURL) {
                    _reflectionProbeURL = url;
                    probe.cubemap.reset();
                    _reflectionProbeTexture = url.isEmpty() ? NetworkTexturePointer() :
                        DependencyManager::get<TextureCache>()->getTexture(QUrl(url), image::TextureUsage::AMBIENT_TEXTURE);
                    _reflectionProbePending = (bool)_reflectionProbeTexture;
                }
                probe.touch();
                if (_reflectionProbeStage && ReflectionProbeStage::isIndexInvalid(_reflectionProbeIndex)) {
                    _reflectionProbeIndex = _reflectionProbeStage->addProbe(_reflectionProbe);
                }
            } else if (_reflectionProbe) {
                removeReflectionProbe();
            }
        }

        // the probe's cube map, once loaded
        if (_reflectionProbePending && _reflectionProbeTexture && _reflectionProbeTexture->isLoaded()) {
            _reflectionProbePending = false;
            auto texture = _reflectionProbeTexture->getGPUTexture();
            if (texture && _reflectionProbe) {
                _reflectionProbe->cubemap = texture;
                _reflectionProbe->touch();
            } else {
                qCDebug(entitiesrenderer) << "Failed to load the reflection probe's cube map:" << _reflectionProbeURL;
            }
        }

        if (_mirror) {
            // the picture of the moment, when it has changed: a slot's texture, or nothing while it has no slot
            uint32_t generation = 0;
            gpu::TexturePointer picture = Mirrors::instance().textureFor(_mirrorId, generation);
            if (generation != _mirrorGeneration) {
                _mirrorGeneration = generation;
                _mirrorPicture = picture;
                if (_mirrorScreen) {
                    // the glass draws the picture itself, by screen position (doRender); the material stays its own
                    _mirrorShown = (bool)picture;
                } else if (picture) {
                    if (!_mirrorSource) {
                        _mirrorSource = std::make_shared<gpu::TextureSource>(QUrl("mirror://picture"));
                    }
                    _mirrorSource->resetTexture(picture);
                    auto map = std::make_shared<graphics::TextureMap>();
                    map->setTextureSource(_mirrorSource);
                    // the +Z face's cell of the box's texture over the whole picture, across and over (see above)
                    static const Transform flip = mirrorPictureTransform();
                    map->setTextureTransform(flip);
                    // the picture is the surface's light: an emissive map over a black albedo, so no lighting touches
                    // it and it needs no unlit path
                    _material->setTextureMap(graphics::MaterialKey::EMISSIVE_MAP, map);
                    _material->setTexCoordTransform(0, flip.getMatrix());   // the material applies only an albedo map's own
                    _material->setEmissive(glm::vec3(1.0f), false);
                    _material->setAlbedo(glm::vec3(0.0f));
                    _mirrorShown = true;
                } else {
                    _material->setTextureMap(graphics::MaterialKey::EMISSIVE_MAP, nullptr);
                    _material->setTexCoordTransform(0, glm::mat4());
                    _material->setEmissive(glm::vec3(0.0f), false);
                    _material->setAlbedo(_color);
                    _mirrorShown = false;
                }
                materialChanged = true;
            }
        }

        auto materials = _materials.find("0");
        if (materials != _materials.end()) {
            if (materialChanged) {
                materials->second.setNeedsUpdate(true);
            }

            if (materials->second.shouldUpdate()) {
                RenderPipelines::updateMultiMaterial(materials->second);
                emit requestRenderUpdate();
            }
        }
    });
}

bool ShapeEntityRenderer::isTransparent() const {
    if (_pulseProperties.getAlphaMode() != PulseMode::NONE) {
        return true;
    }

    auto mat = _materials.find("0");
    if (mat != _materials.end() && mat->second.top().material) {
        if (mat->second.top().material->isProcedural() && mat->second.top().material->isReady()) {
            auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(mat->second.top().material);
            if (procedural->isFading()) {
                return true;
            }
        }

        if (mat->second.getMaterialKey().isTranslucent()) {
            return true;
        }
    }

    return Parent::isTransparent();
}

ShapeEntityRenderer::Pipeline ShapeEntityRenderer::getPipelineType(const graphics::MultiMaterial& materials) const {
    if (materials.top().material && materials.top().material->isProcedural() && materials.top().material->isReady()) {
        return Pipeline::PROCEDURAL;
    }

    graphics::MaterialKey drawMaterialKey = materials.getMaterialKey();
    // A translucent material takes the material pipeline too: the simple translucent pipeline writes no depth, so the
    // shape would cast no shadow, and the material one shades it with its roughness and the local lights. So does a
    // material whose roughness was set (Smoothness in the Inspector), which the simple pipeline ignores.
    if (drawMaterialKey.isEmissive() || drawMaterialKey.isUnlit() || drawMaterialKey.isMetallic() ||
        drawMaterialKey.isScattering() || drawMaterialKey.isTranslucent() || drawMaterialKey.isGlossy()) {
        return Pipeline::MATERIAL;
    }

    // If the material is using any map, we need to use a material ShapeKey
    for (int i = 0; i < graphics::Material::MapChannel::NUM_MAP_CHANNELS; i++) {
        if (drawMaterialKey.isMapChannel(graphics::Material::MapChannel(i))) {
            return Pipeline::MATERIAL;
        }
    }
    return Pipeline::SIMPLE;
}

ShapeKey ShapeEntityRenderer::getShapeKey() {
    ShapeKey::Builder builder;
    auto mat = _materials.find("0");
    if (mat != _materials.end() && mat->second.shouldUpdate()) {
        RenderPipelines::updateMultiMaterial(mat->second);
    }

    // the transparency the item's key holds, not isTransparent() now: the key puts the item in the opaque or the
    // transparent pass, and a material that turned opaque (an opacity dragged back to 1) between the key's rebuilds
    // would be drawn in the transparent pass with the opaque pipeline, which blends nothing and shows the albedo flat
    // and bright until the key catches up
    if (_keyIsTransparent) {
        builder.withTranslucent();
    }

    if (_primitiveMode == PrimitiveMode::LINES) {
        builder.withWireframe();
    }

    auto pipelineType = getPipelineType(mat->second);
    if (pipelineType == Pipeline::MATERIAL) {
        builder.withMaterial();

        graphics::MaterialKey drawMaterialKey = mat->second.getMaterialKey();
        if (drawMaterialKey.isNormalMap()) {
            builder.withTangents();
        }
        if (drawMaterialKey.isLightMap()) {
            builder.withLightMap();
        }
        if (drawMaterialKey.isUnlit()) {
            builder.withUnlit();
        }
        builder.withCullFaceMode(mat->second.getCullFaceMode());
    } else if (pipelineType == Pipeline::PROCEDURAL) {
        builder.withOwnPipeline();
    }

    return builder.build();
}

Item::Bound ShapeEntityRenderer::getBound() {
    auto mat = _materials.find("0");
    if (mat != _materials.end() && mat->second.top().material && mat->second.top().material->isProcedural() &&
        mat->second.top().material->isReady()) {
        auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(mat->second.top().material);
        if (procedural->hasVertexShader() && procedural->hasBoundOperator()) {
            return procedural->getBound();
        }
    }
    return Parent::getBound();
}

void ShapeEntityRenderer::doRender(RenderArgs* args) {
    if (_mirror && !_mirrorScreen && Mirrors::instance().generation(_mirrorId) != _mirrorGeneration) {
        // its picture has changed hands or size: the update takes the new one (nothing polls for that, so it is
        // asked for from here, where the mirror is drawn every frame it is seen)
        emit requestRenderUpdate();
    }
    if (_mirror && _mirrorScreenWanted && args->_renderMode == RenderArgs::DEFAULT_RENDER_MODE &&
        _mirrorScreen == (args->_displayMode == RenderArgs::STEREO_HMD)) {
        // the display changed under a Screen mirror (a headset put on, or the desktop again): the update sets the
        // method the display can draw (see the userData reading); the main view's own frames only
        emit requestRenderUpdate();
    }
    // Shapes just aren't playing well with zone culling.  Since it's a marginal performance
    // win anyway, being so low-poly, we'll not
    // const bool hasChanged = evaluateEntityZoneCullState(_entity);

    PerformanceTimer perfTimer("RenderableShapeEntityItem::render");
    Q_ASSERT(args->_batch);

    gpu::Batch& batch = *args->_batch;

    graphics::MultiMaterial materials;
    auto geometryCache = DependencyManager::get<GeometryCache>();
    GeometryCache::Shape geometryShape;
    PrimitiveMode primitiveMode;
    RenderLayer renderLayer;
    glm::vec4 outColor;
    Pipeline pipelineType;
    {
        // Material entities add and remove their layers on the main thread under _materialsLock; [] here inserted
        // into the map under this renderer's lock instead, on the render thread (two writers at once)
        std::lock_guard<std::mutex> lock(_materialsLock);
        auto found = _materials.find("0");
        if (found != _materials.end()) {
            materials = found->second;
        }
    }
    withReadLock([&] {
        geometryShape = geometryCache->getShapeForEntityShape(_shape);
        primitiveMode = _primitiveMode;
        renderLayer = _renderLayer;
        if (_billboardMode != BillboardMode::NONE) {
            auto rotation = EntityItem::getBillboardRotation(
                _renderTransform.getTranslation(),
                _renderTransform.getRotation(),
                _billboardMode,
                args->getViewFrustum().getPosition()
            );
            // rotate quad so it's upright
            if (_shape == entity::Shape::Quad || _shape == entity::Shape::Circle) {
                rotation *= Quaternions::X_90;
            }
            _renderTransform.setRotation(rotation);
        }
        batch.setModelTransform(_renderTransform);  // use a transform with scale, rotation, registration point and translation
        pipelineType = getPipelineType(materials);
        auto& schema = materials.getSchemaBuffer().get<graphics::MultiMaterial::Schema>();
        outColor = glm::vec4(ColorUtils::tosRGBVec3(schema._albedo), schema._opacity);
        outColor = EntityRenderer::calculatePulseColor(outColor, _pulseProperties, _created);
    });

    if (outColor.a == 0.0f) {
        return;
    }

    // a screen-mode mirror in the main view draws its picture by screen position with a pipeline of its own, under
    // either render method (the mirror view was rendered by the frame's); in a mirror's view, or without a slot
    // this frame, it is a plain shape of its colour
    if (_mirrorScreen && args->_renderMode == RenderArgs::DEFAULT_RENDER_MODE && primitiveMode != PrimitiveMode::LINES) {
        gpu::TexturePointer picture;
        glm::vec4 region;
        if (Mirrors::instance().screenFor(_mirrorId, picture, region)) {
            // the picture and the region of the screen it covers are the frame's own (the planner's, run before
            // the mirror views), so the glass never shows a picture against last frame's region
            if (!_mirrorRegion) {
                _mirrorRegion = std::make_shared<gpu::Buffer>(sizeof(glm::vec4), (const gpu::Byte*)&region);
            } else {
                _mirrorRegion->setSubData(0, region);
            }
            auto previous = args->_shapePipeline;
            batch.setPipeline(screenMirrorPipeline(args->_renderMethod == render::Args::FORWARD));
            batch.setResourceTexture(ENTITIES_TEXTURE_MIRROR_PICTURE, picture);
            batch.setUniformBuffer(ENTITIES_BUFFER_MIRROR, _mirrorRegion, 0, sizeof(glm::vec4));
            geometryCache->renderShape(batch, geometryShape);
            if (previous && previous->pipeline) {
                batch.setPipeline(previous->pipeline);   // the items after this one expect theirs
            }
            args->_details._trianglesRendered += (int)geometryCache->getShapeTriangleCount(geometryShape);
            return;
        }
    }

    if (pipelineType == Pipeline::PROCEDURAL) {
        auto procedural = std::static_pointer_cast<graphics::ProceduralMaterial>(materials.top().material);
        outColor = procedural->getColor(outColor);
        outColor.a *= procedural->isFading() ? Interpolate::calculateFadeRatio(procedural->getFadeStartTime()) : 1.0f;
        const bool forward = DeferredLightingEffect::isForwardLit(args);
        RenderPipelines::bindDefaultUnderlyingMaterial(batch);
        procedural->prepare(batch, _position, _dimensions, _orientation, _created,
                            ProceduralProgramKey(outColor.a < 1.0f, false, false, forward));
        if (forward) {
            DependencyManager::get<DeferredLightingEffect>()->setupForwardLightingBatch(args, batch);
        }

        if (render::ShapeKey(args->_globalShapeKey).isWireframe() || primitiveMode == PrimitiveMode::LINES) {
            geometryCache->renderWireShape(batch, geometryShape, outColor);
        } else {
            geometryCache->renderShape(batch, geometryShape, outColor);
        }
    } else if (pipelineType == Pipeline::SIMPLE) {
        // FIXME, support instanced multi-shape rendering using multidraw indirect
        outColor.a *= _isFading ? Interpolate::calculateFadeRatio(_fadeStartTime) : 1.0f;
        render::ShapePipelinePointer pipeline =
            geometryCache->getShapePipelinePointer(outColor.a < 1.0f, false,
                                                   renderLayer != RenderLayer::WORLD ||
                                                       args->_renderMethod == Args::RenderMethod::FORWARD,
                                                   materials.top().material->getCullFaceMode());
        if (render::ShapeKey(args->_globalShapeKey).isWireframe() || primitiveMode == PrimitiveMode::LINES) {
            geometryCache->renderWireShapeInstance(args, batch, geometryShape, outColor, pipeline);
        } else {
            geometryCache->renderSolidShapeInstance(args, batch, geometryShape, outColor, pipeline);
        }
    } else {
        if (RenderPipelines::bindMaterials(materials, batch, args->_renderMode, args->_enableTexturing)) {
            args->_details._materialSwitches++;
        }

        geometryCache->renderShape(batch, geometryShape);
    }

    const auto triCount = geometryCache->getShapeTriangleCount(geometryShape);
    args->_details._trianglesRendered += (int)triCount;
}

scriptable::ScriptableModelBase ShapeEntityRenderer::getScriptableModel() {
    scriptable::ScriptableModelBase result;
    auto geometryCache = DependencyManager::get<GeometryCache>();
    auto geometryShape = geometryCache->getShapeForEntityShape(_shape);
    glm::vec3 vertexColor;
    {
        std::lock_guard<std::mutex> lock(_materialsLock);
        result.appendMaterials(_materials);
        auto materials = _materials.find("0");
        if (materials != _materials.end()) {
            vertexColor =
                ColorUtils::tosRGBVec3(materials->second.getSchemaBuffer().get<graphics::MultiMaterial::Schema>()._albedo);
        }
    }
    if (auto mesh = geometryCache->meshFromShape(geometryShape, vertexColor)) {
        result.objectID = getEntity()->getID();
        result.append(mesh);
    }
    return result;
}
