//
//  ImpostorCard.cpp
//  libraries/entities-renderer/src
//
//  Created by the editor session on 2026-10-18. See ImpostorCard.h and IMPOSTORS.md 2.3 and 2.4.
//

#include "ImpostorCard.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <glm/gtx/quaternion.hpp>

#include <DeferredLightingEffect.h>
#include <DependencyManager.h>
#include <FadeEffect.h>
#include <StencilMaskPass.h>
#include <ViewFrustum.h>
#include <shaders/Shaders.h>

#include "entities-renderer/ShaderConstants.h"
#include "render-utils/ShaderConstants.h"

using namespace render;

namespace ru {
    using render_utils::slot::texture::Texture;
    using render_utils::slot::buffer::Buffer;
}

namespace {
    struct Params {
        glm::mat4 worldToObject;
        glm::mat4 objectToWorld;
        glm::vec4 centerRadius;
        glm::vec4 material;   // roughness, metallic, the frame size in texels, 1 with a material atlas
        glm::vec4 flags;      // the debug view, the march, march 2's step in pixel footprints, unused
    };

    // The card's pipelines: the G-buffer write, the forward shading and the shadow pass's depth, all from
    // impostor.slv/.slf (entities-renderer/impostor.slp). The item carries the own-pipeline shape key, so every pass
    // (DrawStateSortDeferred, DrawForward, renderShadowCasters) hands it the batch and the card sets its own.
    enum Variant { VARIANT_DEFERRED = 0, VARIANT_FORWARD, VARIANT_SHADOW, VARIANT_COUNT };
    gpu::PipelinePointer _pipelines[VARIANT_COUNT];
    gpu::Stream::FormatPointer _quadFormat;
    gpu::BufferPointer _quadBuffer;

    void initImpostorPipelines() {
        static std::once_flag once;
        std::call_once(once, [] {
            namespace program = shader::entities_renderer::program;
            const uint32_t programs[VARIANT_COUNT] = { program::impostor, program::impostor_forward, program::impostor_shadow };
            for (int variant = 0; variant < VARIANT_COUNT; variant++) {
                auto state = std::make_shared<gpu::State>();
                state->setCullMode(gpu::State::CULL_NONE);   // the square faces the view by construction
                state->setDepthTest(true, true, gpu::GREATER_EQUAL);
                if (variant != VARIANT_SHADOW) {
                    PrepareStencil::testMaskDrawShape(*state);
                }
                _pipelines[variant] = gpu::Pipeline::create(gpu::Shader::createProgram(programs[variant]), state);
            }
            // the unit square, a triangle strip of two
            const glm::vec3 corners[4] = {
                glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(-1.0f, 1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f)
            };
            _quadBuffer = std::make_shared<gpu::Buffer>(sizeof(corners), (const gpu::Byte*)corners);
            _quadFormat = std::make_shared<gpu::Stream::Format>();
            _quadFormat->setAttribute(gpu::Stream::POSITION, 0, gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::XYZ), 0);
        });
    }
}

ImpostorEntry ImpostorEntry::parse(const QString& userData) {
    ImpostorEntry entry;
    if (userData.isEmpty() || !userData.contains(QStringLiteral("impostor"))) {
        return entry;
    }
    QJsonValue value = QJsonDocument::fromJson(userData.toUtf8()).object().value("impostor");
    if (!value.isObject()) {
        return entry;
    }
    QJsonObject object = value.toObject();
    entry.spec = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    entry.albedoUrl = object.value("albedo").toString();
    entry.normalUrl = object.value("normal").toString();
    entry.materialUrl = object.value("material").toString();
    QString frames = object.value("frames").toString("cube6");
    entry.size = object.value("size").toInt(256);
    entry.radius = (float)object.value("radius").toDouble(0.0);
    QJsonArray center = object.value("center").toArray();
    if (center.size() == 3) {
        entry.center = glm::vec3((float)center[0].toDouble(), (float)center[1].toDouble(), (float)center[2].toDouble());
    }
    entry.roughness = (float)object.value("roughness").toDouble(0.7);
    entry.metallic = (float)object.value("metallic").toDouble(0.0);
    entry.valid = frames == "cube6" && !entry.albedoUrl.isEmpty() && !entry.normalUrl.isEmpty() && entry.radius > 0.0f;
    return entry;
}

ImpostorCard::ImpostorCard() : _switch(std::make_shared<ImpostorSwitch>()) {
    initImpostorPipelines();
    _params = std::make_shared<gpu::Buffer>(sizeof(Params), nullptr);
}

void ImpostorCard::setEntry(const ImpostorEntry& entry) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (entry.spec == _pending.entry.spec && entry.valid == _pending.entry.valid) {
        return;
    }
    _pending.entry = entry;
    // the albedo as a colour texture (sRGB, mips); the normal and depth, and the roughness and metallic, as four
    // linear channels, uncompressed, which is what a lightmap's direction map is (a normal map's two compressed
    // channels would lose the depth)
    auto textureCache = DependencyManager::get<TextureCache>();
    _albedo = entry.valid ? textureCache->getTexture(QUrl(entry.albedoUrl), image::TextureUsage::ALBEDO_TEXTURE) : nullptr;
    _normal = entry.valid ? textureCache->getTexture(QUrl(entry.normalUrl), image::TextureUsage::LIGHTMAP_DIRECTION_TEXTURE) : nullptr;
    _material = (entry.valid && !entry.materialUrl.isEmpty())
        ? textureCache->getTexture(QUrl(entry.materialUrl), image::TextureUsage::LIGHTMAP_DIRECTION_TEXTURE) : nullptr;
}

void ImpostorCard::setFrame(const glm::vec3& positionWS, const glm::quat& rotationWS) {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.position = positionWS;
    _pending.rotation = rotationWS;
}

void ImpostorCard::setVisible(bool visible) {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.visible = visible;
}

void ImpostorCard::setKeyBits(render::hifi::Tag tagMask, render::hifi::Layer layer, bool subMetaCulled, bool castShadow) {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.tagMask = tagMask;
    _pending.layer = layer;
    _pending.subMetaCulled = subMetaCulled;
    _pending.castShadow = castShadow;
}

void ImpostorCard::setStandIn(bool standIn) {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.standIn = standIn;
}

bool ImpostorCard::waitingForTextures() const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto waiting = [](const NetworkTexturePointer& texture) { return texture && !texture->isLoaded() && !texture->isFailed(); };
    return waiting(_albedo) || waiting(_normal) || waiting(_material);
}

bool ImpostorCard::atlasesLoaded() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _albedo && _albedo->isLoaded() && _normal && _normal->isLoaded();
}

bool ImpostorCard::atlasesFailed() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return (_albedo && _albedo->isFailed()) || (_normal && _normal->isFailed());
}

bool ImpostorCard::getSphere(glm::vec3& centerWS, float& radius) const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_pending.entry.valid) {
        return false;
    }
    centerWS = _pending.position + _pending.rotation * _pending.entry.center;
    radius = _pending.entry.radius;
    return true;
}

void ImpostorCard::update() {
    NetworkTexturePointer albedo;
    NetworkTexturePointer normal;
    NetworkTexturePointer material;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _live = _pending;
        albedo = _albedo;
        normal = _normal;
        material = _material;
    }
    const auto& entry = _live.entry;
    _albedoTexture = (albedo && albedo->isLoaded()) ? albedo->getGPUTexture() : nullptr;
    _normalTexture = (normal && normal->isLoaded()) ? normal->getGPUTexture() : nullptr;
    _materialTexture = (material && material->isLoaded()) ? material->getGPUTexture() : nullptr;
    _ready = entry.valid && _albedoTexture && _normalTexture;   // the material atlas is a refinement, not a condition

    _centerWS = _live.position + _live.rotation * entry.center;
    float radius = entry.valid ? entry.radius : 0.0f;
    _bound = AABox(_centerWS - glm::vec3(radius), glm::vec3(2.0f * radius));
    _switch->setSphere(_centerWS, radius);
    _switch->setCardReady(_ready && _live.visible);
    _switch->setStandIn(_live.standIn);

    if (_ready) {
        Params params;
        params.worldToObject = glm::mat4_cast(glm::inverse(_live.rotation));
        params.objectToWorld = glm::mat4_cast(_live.rotation);
        params.centerRadius = glm::vec4(_centerWS, radius);
        // the atlas is 3 x 2 frames: a frame's side is half the atlas's height
        float frameSize = (float)(_albedoTexture->getHeight() / 2);
        params.material = glm::vec4(entry.roughness, entry.metallic, frameSize, _materialTexture ? 1.0f : 0.0f);
        params.flags = glm::vec4((float)_debugMode, (float)_marchMode, _marchFootprints, 0.0f);
        _params->setSubData(0, params);
    }
}

ItemKey ImpostorCard::getKey() {
    ItemKey::Builder builder = ItemKey::Builder().withTypeShape().withTagBits(_live.tagMask).withLayer(_live.layer);
    if (_live.subMetaCulled) {
        builder.withSubMetaCulled();
    }
    if (_live.castShadow) {
        builder.withShadowCaster();
    }
    if (!_live.visible || !_ready) {
        builder.withInvisible();
    }
    return builder.build();
}

ShapeKey ImpostorCard::getShapeKey() {
    return ShapeKey::Builder().withOwnPipeline().build();
}

Item::Bound ImpostorCard::getBound() {
    return _bound;
}

void ImpostorCard::render(RenderArgs* args) {
    if (!_ready || !_live.visible || !args || !args->_batch || !args->hasViewFrustum()) {
        return;
    }
    ImpostorSwitch::View view = _switch->evaluate(args);
    if (!view.card) {
        return;
    }
    const bool shadow = args->_renderMode == RenderArgs::RenderMode::SHADOW_RENDER_MODE;
    const bool forward = args->_renderMethod == Args::RenderMethod::FORWARD;
    const float radius = _live.entry.radius;
    const ViewFrustum& frustum = args->getViewFrustum();

    // An orthographic view (a shadow cascade's) looks along one direction: the card faces it, as wide as the sphere.
    // A perspective view's faces the camera, and a little wider up close, where the sphere's silhouette is wider
    // than its section through the centre (twice the radius at most, inside the sphere).
    const bool orthographic = frustum.getProjection()[2][3] == 0.0f;
    glm::vec3 toCamera;
    float halfSide;
    if (orthographic) {
        toCamera = -frustum.getDirection();
        halfSide = radius;
    } else {
        toCamera = frustum.getPosition() - _centerWS;
        float distance = glm::length(toCamera);
        toCamera = (distance > 1.0e-5f) ? toCamera / distance : glm::vec3(0.0f, 0.0f, 1.0f);
        halfSide = 2.0f * radius;
        if (distance > radius) {
            float ratio = radius / distance;
            halfSide = std::min(radius / sqrtf(1.0f - ratio * ratio), 2.0f * radius);
        }
    }
    Transform transform;
    transform.setTranslation(_centerWS);
    transform.setRotation(glm::rotation(glm::vec3(0.0f, 0.0f, 1.0f), toCamera));
    transform.setScale(glm::vec3(halfSide));

    if (args->_impostorDebug != _debugMode || args->_impostorMarch != _marchMode ||
        args->_impostorMarchFootprints != _marchFootprints) {
        // the shader's debug view and march, in the parameters' flags
        _debugMode = args->_impostorDebug;
        _marchMode = args->_impostorMarch;
        _marchFootprints = args->_impostorMarchFootprints;
        glm::vec4 flags((float)_debugMode, (float)_marchMode, _marchFootprints, 0.0f);
        _params->setSubData(offsetof(Params, flags), sizeof(flags), reinterpret_cast<const gpu::Byte*>(&flags));
    }

    gpu::Batch& batch = *args->_batch;
    batch.setPipeline(_pipelines[shadow ? VARIANT_SHADOW : (forward ? VARIANT_FORWARD : VARIANT_DEFERRED)]);
    if (forward && !shadow) {
        // the forward variant lights in place, so it binds the frame's lighting as the lit pipelines' batch setter
        // does (RenderPipelines' lightBatchSetter): the key light, the ambient with its skybox and probe volume, the
        // lights, clusters, probes and shadows
        auto deferredLightingEffect = DependencyManager::get<DeferredLightingEffect>();
        deferredLightingEffect->setupKeyLightBatch(args, batch);
        deferredLightingEffect->setupFrameLightingBatch(batch, args);
    }
    // the dissolve into or out of the model (ImpostorSwitch): the fade effect's noise, the switch's threshold
    auto fadeEffect = DependencyManager::get<FadeEffect>();
    batch.setUniformBuffer(ru::Buffer::FadeParameters, fadeEffect->getConfigurationBuffer());
    batch.setResourceTexture(ru::Texture::FadeMask, fadeEffect->getMaskMap());
    batch.setUniformBuffer(ru::Buffer::FadeObjectParameters, _switch->fadeParams(args));

    batch.setModelTransform(transform);
    batch.setInputFormat(_quadFormat);
    batch.setInputBuffer(0, _quadBuffer, 0, sizeof(glm::vec3));
    batch.setUniformBuffer(ENTITIES_BUFFER_IMPOSTOR, _params, 0, sizeof(Params));
    batch.setResourceTexture(ENTITIES_TEXTURE_IMPOSTOR_ALBEDO, _albedoTexture);
    batch.setResourceTexture(ENTITIES_TEXTURE_IMPOSTOR_NORMAL, _normalTexture);
    batch.setResourceTexture(ENTITIES_TEXTURE_IMPOSTOR_MATERIAL, _materialTexture ? _materialTexture : _normalTexture);
    batch.draw(gpu::TRIANGLE_STRIP, 4, 0);
    batch.setResourceTexture(ENTITIES_TEXTURE_IMPOSTOR_ALBEDO, nullptr);
    batch.setResourceTexture(ENTITIES_TEXTURE_IMPOSTOR_NORMAL, nullptr);
    batch.setResourceTexture(ENTITIES_TEXTURE_IMPOSTOR_MATERIAL, nullptr);
    args->_details._trianglesRendered += 2;
}
