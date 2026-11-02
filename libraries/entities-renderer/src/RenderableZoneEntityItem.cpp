//
//  RenderableZoneEntityItem.cpp
//
//
//  Created by Clement on 4/22/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderableZoneEntityItem.h"

#include <gpu/Batch.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <ResourceManager.h>
#include <ResourceRequest.h>

#include <graphics/Stage.h>

#include <DependencyManager.h>
#include <GeometryCache.h>
#include <PerfStat.h>
#include <procedural/ProceduralSkybox.h>
#include <LightPayload.h>
#include <DeferredLightingEffect.h>

#include "EntityTreeRenderer.h"

// Sphere entities should fit inside a cube entity of the same size, so a sphere that has dimensions 1x1x1
// is a half unit sphere.  However, the geometry cache renders a UNIT sphere, so we need to scale down.
// static const float SPHERE_ENTITY_SCALE = 0.5f;

using namespace render;
using namespace render::entities;



ZoneEntityRenderer::ZoneEntityRenderer(const EntityItemPointer& entity) : Parent(entity) {
    _background->setSkybox(std::make_shared<ProceduralSkybox>(entity->getCreated()));
    // A zone with no ambient picture is lit by the built-in sphere from the start. setAmbientURL("") returns early
    // while the URL is still empty, so without this the sphere stayed black until a picture was set and cleared.
    _ambientLight->setAmbientSpherePreset(gpu::SphericalHarmonics::BREEZEWAY);
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
}

void ZoneEntityRenderer::onRemoveFromSceneTyped(const TypedEntityPointer& entity) {
    dropProbeVolumeRequest();
    if (_stage) {
        if (!LightStage::isIndexInvalid(_sunIndex)) {
            _stage->removeLight(_sunIndex);
            _sunIndex = INVALID_INDEX;
        }
        if (!LightStage::isIndexInvalid(_ambientIndex)) {
            _stage->removeLight(_ambientIndex);
            _ambientIndex = INVALID_INDEX;
        }
    }

    if (_backgroundStage) {
        if (!BackgroundStage::isIndexInvalid(_backgroundIndex)) {
            _backgroundStage->removeBackground(_backgroundIndex);
            _backgroundIndex = INVALID_INDEX;
        }
    }

    if (_hazeStage) {
        if (!HazeStage::isIndexInvalid(_hazeIndex)) {
            _hazeStage->removeHaze(_hazeIndex);
            _hazeIndex = INVALID_INDEX;
        }
    }

    if (_bloomStage) {
        if (!BloomStage::isIndexInvalid(_bloomIndex)) {
            _bloomStage->removeBloom(_bloomIndex);
            _bloomIndex = INVALID_INDEX;
        }
    }
}

void ZoneEntityRenderer::doRender(RenderArgs* args) {
    if (!_stage) {
        _stage = args->_scene->getStage<LightStage>();
        assert(_stage);
    }

    if (!_backgroundStage) {
        _backgroundStage = args->_scene->getStage<BackgroundStage>();
        assert(_backgroundStage);
    }

    if (!_hazeStage) {
        _hazeStage = args->_scene->getStage<HazeStage>();
        assert(_hazeStage);
    }

    if (!_bloomStage) {
        _bloomStage = args->_scene->getStage<BloomStage>();
        assert(_bloomStage);
    }

    {  // Sun
        if (_needSunUpdate) {
            if (LightStage::isIndexInvalid(_sunIndex)) {
                _sunIndex = _stage->addLight(_sunLight);
            } else {
                _stage->updateLightArrayBuffer(_sunIndex);
            }
            _needSunUpdate = false;
        }
    }

    {  // Ambient
        updateAmbientMap();

        if (_needAmbientUpdate) {
            if (LightStage::isIndexInvalid(_ambientIndex)) {
                _ambientIndex = _stage->addLight(_ambientLight);
            } else {
                _stage->updateLightArrayBuffer(_ambientIndex);
            }
            _needAmbientUpdate = false;
        }
    }

    {  // Skybox
        updateSkyboxMap();

        if (_needBackgroundUpdate) {
            if (BackgroundStage::isIndexInvalid(_backgroundIndex)) {
                _backgroundIndex = _backgroundStage->addBackground(_background);
            }
            _needBackgroundUpdate = false;
        }
    }

    {
        if (_needHazeUpdate) {
            if (HazeStage::isIndexInvalid(_hazeIndex)) {
                _hazeIndex = _hazeStage->addHaze(_haze);
            }
            _needHazeUpdate = false;
        }
    }

    {
        if (_needBloomUpdate) {
            if (BloomStage::isIndexInvalid(_bloomIndex)) {
                _bloomIndex = _bloomStage->addBloom(_bloom);
            }
            _needBloomUpdate = false;
        }
    }

    {
        if (_needZoneCullingUpdate) {
            _needZoneCullingUpdate = false;
        }
    }

    {
        if (_needToneMappingUpdate) {
            _needToneMappingUpdate = false;
        }
    }

    if (_visible) {
        // Finally, push the lights visible in the frame
        //
        // If component is disabled then push component off state
        // else if component is enabled then push current state
        // (else mode is inherit, the value from the parent zone will be used
        //
        if (_keyLightMode == COMPONENT_MODE_DISABLED //||
            //DependencyManager::get<TextureCache>()->isEverythingUnlit()
        ) {
            _stage->_currentFrame.pushSunLight(_stage->getSunOffLight());
        } else if (_keyLightMode == COMPONENT_MODE_ENABLED) {
            _stage->_currentFrame.pushSunLight(_sunIndex);
        }

        if (_skyboxMode == COMPONENT_MODE_DISABLED) {
            _backgroundStage->_currentFrame.pushBackground(INVALID_INDEX);
        } else if (_skyboxMode == COMPONENT_MODE_ENABLED) {
            _backgroundStage->_currentFrame.pushBackground(_backgroundIndex);
        }

        if (_ambientLightMode == COMPONENT_MODE_DISABLED// ||
           // DependencyManager::get<TextureCache>()->isEverythingUnlit()
        ) {
            _stage->_currentFrame.pushAmbientLight(_stage->getAmbientOffLight());
        } else if (_ambientLightMode == COMPONENT_MODE_ENABLED) {
            _stage->_currentFrame.pushAmbientLight(_ambientIndex);
        }

        // Haze only if the mode is not inherit, as the model deals with on/off
        if (_hazeMode != COMPONENT_MODE_INHERIT// &&
        //    !DependencyManager::get<TextureCache>()->isEverythingUnlit()
        ) {
            _hazeStage->_currentFrame.pushHaze(_hazeIndex);
        }

        if (_bloomMode == COMPONENT_MODE_DISABLED //||
            //DependencyManager::get<TextureCache>()->isEverythingUnlit()
            ) {
            _bloomStage->_currentFrame.pushBloom(INVALID_INDEX);
        } else if (_bloomMode == COMPONENT_MODE_ENABLED) {
            _bloomStage->_currentFrame.pushBloom(_bloomIndex);
        }
    }
}

void ZoneEntityRenderer::doRenderUpdateSynchronousTyped(const ScenePointer& scene,
                                                        Transaction& transaction,
                                                        const TypedEntityPointer& entity) {
    auto position = entity->getWorldPosition();
    auto rotation = entity->getWorldOrientation();
    auto dimensions = entity->getScaledDimensions();
    bool rotationChanged = rotation != _lastRotation;
    bool transformChanged = rotationChanged || position != _lastPosition || dimensions != _lastDimensions;

    auto visible = entity->getVisible();
    if (transformChanged || visible != _lastVisible) {
        _lastVisible = visible;
        DependencyManager::get<EntityTreeRenderer>()->updateZone(entity->getID());
    }

    auto proceduralUserData = entity->getUserData();
    bool proceduralUserDataChanged = _proceduralUserData != proceduralUserData;

    // FIXME one of the bools here could become true between being fetched and being reset,
    // resulting in a lost update
    bool keyLightChanged = entity->keyLightPropertiesChanged() || rotationChanged;
    bool ambientLightChanged = entity->ambientLightPropertiesChanged() || transformChanged;
    bool skyboxChanged = entity->skyboxPropertiesChanged() || proceduralUserDataChanged;
    bool hazeChanged = entity->hazePropertiesChanged();
    bool bloomChanged = entity->bloomPropertiesChanged();
    bool zoneCullingChanged = entity->zoneCullingPropertiesChanged();
    bool toneMappingChanged = entity->toneMappingPropertiesChanged();
    entity->resetRenderingPropertiesChanged();

    if (transformChanged) {
        _lastPosition = entity->getWorldPosition();
        _lastRotation = entity->getWorldOrientation();
        _lastDimensions = entity->getScaledDimensions();
    }

    if (proceduralUserDataChanged) {
        _proceduralUserData = entity->getUserData();
        updateProbeVolumeFromUserData();
    }

    updateKeyZoneItemFromEntity(entity);

    if (keyLightChanged) {
        _keyLightProperties = entity->getKeyLightProperties();
        updateKeySunFromEntity(entity);
    }

    if (ambientLightChanged) {
        _ambientLightProperties = entity->getAmbientLightProperties();
        updateAmbientLightFromEntity(entity);
    }

    if (skyboxChanged) {
        _skyboxProperties = entity->getSkyboxProperties();
        updateKeyBackgroundFromEntity(entity);
    }

    if (hazeChanged) {
        _hazeProperties = entity->getHazeProperties();
        updateHazeFromEntity(entity);
    }

    if (bloomChanged) {
        _bloomProperties = entity->getBloomProperties();
        updateBloomFromEntity(entity);
    }

    if (zoneCullingChanged) {
        _zoneCullingProperties = entity->getZoneCullingProperties();
        updateZoneCullingFromEntity(entity);
        // the zone's culling mode changed: the tree renderer decides again whether this zone culls, at its next check
        DependencyManager::get<EntityTreeRenderer>()->zoneCullingZoneChanged(entity->getID());
    }

    if (toneMappingChanged) {
        _toneMappingProperties = entity->getToneMappingProperties();
        updateToneMappingFromEntity(entity);
    }

    bool visuallyReady = true;
    uint32_t skyboxMode = entity->getSkyboxMode();
    if (skyboxMode == COMPONENT_MODE_ENABLED && !_skyboxTextureURL.isEmpty()) {
        bool skyboxLoadedOrFailed = (_skyboxTexture && (_skyboxTexture->isLoaded() || _skyboxTexture->isFailed()));

        visuallyReady = skyboxLoadedOrFailed;
    }

    entity->setVisuallyReady(visuallyReady);
}

ItemKey ZoneEntityRenderer::getKey() {
    return ItemKey::Builder().withTypeMeta().withTagBits(getTagMask()).build();
}

bool ZoneEntityRenderer::needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const {
    if (entity->getVisible() != _lastVisible) {
        return true;
    }

    if (entity->keyLightPropertiesChanged() || entity->ambientLightPropertiesChanged() || entity->hazePropertiesChanged() ||
        entity->bloomPropertiesChanged() || entity->skyboxPropertiesChanged()) {
        return true;
    }

    if (_skyboxTextureURL != entity->getSkyboxProperties().getURL()) {
        return true;
    }

    if (entity->getWorldPosition() != _lastPosition) {
        return true;
    }
    if (entity->getScaledDimensions() != _lastDimensions) {
        return true;
    }
    if (entity->getWorldOrientation() != _lastRotation) {
        return true;
    }

    if (entity->getUserData() != _proceduralUserData) {
        return true;
    }

    return false;
}

void ZoneEntityRenderer::updateKeySunFromEntity(const TypedEntityPointer& entity) {
    setKeyLightMode((ComponentMode)entity->getKeyLightMode());

    const auto& sunLight = editSunLight();
    sunLight->setType(graphics::Light::SUN);
    sunLight->setPosition(_lastPosition);
    sunLight->setOrientation(_lastRotation);

    // Set the keylight
    sunLight->setColor(ColorUtils::toVec3(_keyLightProperties.getColor()));
    sunLight->setIntensity(_keyLightProperties.getIntensity());
    sunLight->setDirection(_lastRotation * _keyLightProperties.getDirection());
    sunLight->setCastShadows(_keyLightProperties.getCastShadows());
    sunLight->setShadowBias(_keyLightProperties.getShadowBias());
    sunLight->setShadowsMaxDistance(_keyLightProperties.getShadowMaxDistance());
}

void ZoneEntityRenderer::updateAmbientLightFromEntity(const TypedEntityPointer& entity) {
    setAmbientLightMode((ComponentMode)entity->getAmbientLightMode());

    const auto& ambientLight = editAmbientLight();
    ambientLight->setType(graphics::Light::AMBIENT);
    ambientLight->setPosition(_lastPosition);
    ambientLight->setOrientation(_lastRotation);

    // Set the ambient light
    ambientLight->setAmbientIntensity(_ambientLightProperties.getAmbientIntensity());

    if (_ambientLightProperties.getAmbientURL().isEmpty()) {
        setAmbientURL(_skyboxProperties.getURL());
    } else {
        setAmbientURL(_ambientLightProperties.getAmbientURL());
    }

    ambientLight->setTransform(entity->getTransform().getInverseMatrix());
}

void ZoneEntityRenderer::updateHazeFromEntity(const TypedEntityPointer& entity) {
    setHazeMode((ComponentMode)entity->getHazeMode());

    const auto& haze = editHaze();

    const uint32_t hazeMode = entity->getHazeMode();
    haze->setHazeActive(hazeMode == COMPONENT_MODE_ENABLED);
    haze->setAltitudeBased(_hazeProperties.getHazeAltitudeEffect());

    haze->setHazeRangeFactor(graphics::Haze::convertHazeRangeToHazeRangeFactor(_hazeProperties.getHazeRange()));
    glm::u8vec3 hazeColor = _hazeProperties.getHazeColor();
    haze->setHazeColor(toGlm(hazeColor));
    glm::u8vec3 hazeGlareColor = _hazeProperties.getHazeGlareColor();
    haze->setHazeGlareColor(toGlm(hazeGlareColor));
    haze->setHazeEnableGlare(_hazeProperties.getHazeEnableGlare());
    haze->setHazeGlareBlend(graphics::Haze::convertGlareAngleToPower(_hazeProperties.getHazeGlareAngle()));

    float hazeAltitude = _hazeProperties.getHazeCeiling() - _hazeProperties.getHazeBaseRef();
    haze->setHazeAltitudeFactor(graphics::Haze::convertHazeAltitudeToHazeAltitudeFactor(hazeAltitude));
    haze->setHazeBaseReference(_hazeProperties.getHazeBaseRef());

    haze->setHazeBackgroundBlend(_hazeProperties.getHazeBackgroundBlend());

    haze->setHazeAttenuateKeyLight(_hazeProperties.getHazeAttenuateKeyLight());
    haze->setHazeKeyLightRangeFactor(graphics::Haze::convertHazeRangeToHazeRangeFactor(_hazeProperties.getHazeKeyLightRange()));
    haze->setHazeKeyLightAltitudeFactor(
        graphics::Haze::convertHazeAltitudeToHazeAltitudeFactor(_hazeProperties.getHazeKeyLightAltitude()));
}

void ZoneEntityRenderer::updateBloomFromEntity(const TypedEntityPointer& entity) {
    setBloomMode((ComponentMode)entity->getBloomMode());
    const auto& bloom = editBloom();
    bloom->setBloomIntensity(_bloomProperties.getBloomIntensity());
    bloom->setBloomThreshold(_bloomProperties.getBloomThreshold());
    bloom->setBloomSize(_bloomProperties.getBloomSize());
}

void ZoneEntityRenderer::updateToneMappingFromEntity(const TypedEntityPointer& entity) {
    setToneMappingMode((ToneMappingComponentMode)entity->getToneMappingMode());
    setToneMappingExposure(_toneMappingProperties.getExposure());
}

void ZoneEntityRenderer::updateZoneCullingFromEntity(const TypedEntityPointer& entity) {
    setZoneCullingMode((ZoneCullingComponentMode)entity->getZoneCullingMode());
}

void ZoneEntityRenderer::updateKeyBackgroundFromEntity(const TypedEntityPointer& entity) {
    setSkyboxMode((ComponentMode)entity->getSkyboxMode());

    editBackground();
    setSkyboxColor(toGlm(_skyboxProperties.getColor()));
    setProceduralUserData(_proceduralUserData);
    setSkyboxURL(_skyboxProperties.getURL());
}

void ZoneEntityRenderer::updateKeyZoneItemFromEntity(const TypedEntityPointer& entity) {
    // Update rotation values
    editSkybox()->setOrientation(_lastRotation);

    /* TODO: Implement the sun model behavior / Keep this code here for reference, this is how we
    {
    // Set the stage
    bool isSunModelEnabled = this->getStageProperties().getSunModelEnabled();
    sceneStage->setSunModelEnable(isSunModelEnabled);
    if (isSunModelEnabled) {
    sceneStage->setLocation(this->getStageProperties().getLongitude(),
    this->getStageProperties().getLatitude(),
    this->getStageProperties().getAltitude());

    auto sceneTime = sceneStage->getTime();
    sceneTime->setHour(this->getStageProperties().calculateHour());
    sceneTime->setDay(this->getStageProperties().calculateDay());
    }
    }*/
}

void ZoneEntityRenderer::setAmbientURL(const QString& ambientUrl) {
    if (_ambientTextureURL == ambientUrl) {
        return;
    }
    _ambientTextureURL = ambientUrl;

    if (_ambientTextureURL.isEmpty()) {
        _pendingAmbientTexture = false;
        _ambientTexture.clear();

        _ambientLight->setAmbientMap(nullptr);
        _ambientLight->setAmbientSpherePreset(gpu::SphericalHarmonics::BREEZEWAY);
    } else {
        _pendingAmbientTexture = true;
        auto textureCache = DependencyManager::get<TextureCache>();
        _ambientTexture = textureCache->getTexture(_ambientTextureURL, image::TextureUsage::AMBIENT_TEXTURE);
    }
}

void ZoneEntityRenderer::updateAmbientMap() {
    if (_pendingAmbientTexture) {
        if (_ambientTexture && _ambientTexture->isLoaded()) {
            _pendingAmbientTexture = false;

            auto texture = _ambientTexture->getGPUTexture();
            if (texture) {
                if (texture->getIrradiance()) {
                    _ambientLight->setAmbientSphere(*texture->getIrradiance());
                } else {
                    _ambientLight->setAmbientSpherePreset(gpu::SphericalHarmonics::BREEZEWAY); // CPM What's going on here?
                }
                editAmbientLight()->setAmbientMap(texture);
            } else {
                qCDebug(entitiesrenderer) << "Failed to load ambient texture:" << _ambientTexture->getURL();
            }
        }
    }
}

void ZoneEntityRenderer::setSkyboxURL(const QString& skyboxUrl) {
    if (_skyboxTextureURL == skyboxUrl) {
        return;
    }
    _skyboxTextureURL = skyboxUrl;

    if (_skyboxTextureURL.isEmpty()) {
        _pendingSkyboxTexture = false;
        _skyboxTexture.clear();

        editSkybox()->setCubemap(nullptr);
    } else {
        _pendingSkyboxTexture = true;
        auto textureCache = DependencyManager::get<TextureCache>();
        _skyboxTexture = textureCache->getTexture(_skyboxTextureURL, image::TextureUsage::SKY_TEXTURE);
    }
}

void ZoneEntityRenderer::updateSkyboxMap() {
    if (_pendingSkyboxTexture) {
        if (_skyboxTexture && _skyboxTexture->isLoaded()) {
            _pendingSkyboxTexture = false;

            auto texture = _skyboxTexture->getGPUTexture();
            if (texture) {
                editSkybox()->setCubemap(texture);
            } else {
                qCDebug(entitiesrenderer) << "Failed to load Skybox texture:" << _skyboxTexture->getURL();
            }
        }
    }
}

void ZoneEntityRenderer::setHazeMode(ComponentMode mode) {
    _hazeMode = mode;
}

void ZoneEntityRenderer::setKeyLightMode(ComponentMode mode) {
    _keyLightMode = mode;
}

void ZoneEntityRenderer::setAmbientLightMode(ComponentMode mode) {
    _ambientLightMode = mode;
}

void ZoneEntityRenderer::setSkyboxMode(ComponentMode mode) {
    _skyboxMode = mode;
}

void ZoneEntityRenderer::setBloomMode(ComponentMode mode) {
    _bloomMode = mode;
}

void ZoneEntityRenderer::setZoneCullingMode(ZoneCullingComponentMode mode) {
    _zoneCullingMode = mode;
}

void ZoneEntityRenderer::setToneMappingMode(ToneMappingComponentMode mode) {
    //  qDebug() << "CPM - SET TONE MAPPING MODE TO " << mode;
    _toneMappingMode = mode;
}

void ZoneEntityRenderer::setToneMappingExposure(const float value) {
    // qDebug() << "CPM - SET TONE MAPPING EXPOSURE TO " << value;
    _toneMappingExposure = value;
    //editSkybox()->setColor(color);
}

void ZoneEntityRenderer::setSkyboxColor(const glm::vec3& color) {
    editSkybox()->setColor(color);
}

void ZoneEntityRenderer::setProceduralUserData(const QString& userData) {
    std::dynamic_pointer_cast<ProceduralSkybox>(editSkybox())->parse(userData);
}

// The zone's probe volume (LIGHTMAPS.md step 4): the entry "probeVolume" { "url", "intensity" } of the zone's
// userData names a probe volume file (the Unity exporter's probes/ProbeVolume.bin), L2 spherical harmonics on a
// uniform grid in world space, which the zone's ambient light carries as a 3D texture (graphics::Light::setProbeVolume)
// and the shaders sample by world position for the ambient diffuse light of whatever has no lightmap. It rides with
// the ambient light, so the zone's ambient light mode has to be enabled for it to count.
void ZoneEntityRenderer::updateProbeVolumeFromUserData() {
    QString url;
    float intensity = 1.0f;
    QJsonValue entry = QJsonDocument::fromJson(_proceduralUserData.toUtf8()).object().value("probeVolume");
    if (entry.isObject()) {
        url = entry.toObject().value("url").toString().trimmed();
        intensity = (float)entry.toObject().value("intensity").toDouble(1.0);
    }
    if (url != _probeVolumeURL) {
        _probeVolumeURL = url;
        _probeVolumeTexture.reset();
        _probeVolumeDims = glm::uvec3(0);
        dropProbeVolumeRequest();
        if (url.isEmpty()) {
            applyProbeVolume();
        } else {
            fetchProbeVolume(url);
        }
    }
    if (intensity != _probeVolumeIntensity) {
        _probeVolumeIntensity = intensity;
        applyProbeVolume();
    }
}

void ZoneEntityRenderer::fetchProbeVolume(const QString& url) {
    _probeVolumeRequest = DependencyManager::get<ResourceManager>()->createResourceRequest(this, QUrl(url), true, -1, "ZoneEntityRenderer::fetchProbeVolume");
    if (!_probeVolumeRequest) {
        qCWarning(entitiesrenderer) << "Probe volume" << url << "cannot be fetched: no request for that kind of URL";
        return;
    }
    connect(_probeVolumeRequest, &ResourceRequest::finished, this, [this, url] {
        ResourceRequest* request = _probeVolumeRequest;
        _probeVolumeRequest = nullptr;
        if (!request) {
            return;
        }
        request->deleteLater();
        if (url != _probeVolumeURL) {
            return;
        }
        if (request->getResult() != ResourceRequest::Success) {
            qCWarning(entitiesrenderer) << "Probe volume" << url << "could not be fetched:" << request->getResultString();
            return;
        }
        QString error;
        if (!loadProbeVolume(request->getData(), url, error)) {
            qCWarning(entitiesrenderer) << "Probe volume" << url << "is not usable:" << error;
            _probeVolumeTexture.reset();
        }
        applyProbeVolume();
    });
    _probeVolumeRequest->send();
}

void ZoneEntityRenderer::dropProbeVolumeRequest() {
    if (_probeVolumeRequest) {
        _probeVolumeRequest->disconnect(this);
        _probeVolumeRequest->deleteLater();
        _probeVolumeRequest = nullptr;
    }
}

// The probe volume file: "DRPV", u32 version (1), u32 probes per axis W H D, f32 origin x y z (the world position
// of probe 0, 0, 0), f32 spacing x y z, u32 coefficients (9), u32 format (1: RGBA halves), then the 3D texture's
// texels, W by 9 H by D, x fastest, then the coefficient block and the probe row, then the slice: the nine
// coefficients L00, L1m1, L10, L11, L2m2, L2m1, L20, L21, L22 of the engine's spherical harmonics, RGB, the alpha
// of the first 1 for a probe Unity baked and 0 for one filled in from its neighbours
bool ZoneEntityRenderer::loadProbeVolume(const QByteArray& data, const QString& url, QString& error) {
    const int HEADER_SIZE = 52;
    const uint32_t MAX_DIMENSION = 2048;   // the least a GL implementation allows a 3D texture
    if (data.size() < HEADER_SIZE || memcmp(data.constData(), "DRPV", 4) != 0) {
        error = "not a probe volume file";
        return false;
    }
    uint32_t words[12];
    memcpy(words, data.constData() + 4, sizeof(words));
    float floats[6];
    memcpy(floats, data.constData() + 20, sizeof(floats));
    uint32_t version = words[0], width = words[1], height = words[2], depth = words[3];
    uint32_t coefficients = words[10], format = words[11];
    glm::vec3 origin(floats[0], floats[1], floats[2]);
    glm::vec3 spacing(floats[3], floats[4], floats[5]);
    if (version != 1 || coefficients != 9 || format != 1) {
        error = QString("version %1, %2 coefficients, format %3 (the client reads version 1, 9 coefficients, format 1)").arg(version).arg(coefficients).arg(format);
        return false;
    }
    if (width == 0 || height == 0 || depth == 0 || width > MAX_DIMENSION || 9 * height > MAX_DIMENSION || depth > MAX_DIMENSION) {
        error = QString("%1 x %2 x %3 probes (the texture is at most %4 texels a side, 9 rows a probe along y)").arg(width).arg(height).arg(depth).arg(MAX_DIMENSION);
        return false;
    }
    if (!(spacing.x > 0.0f && spacing.y > 0.0f && spacing.z > 0.0f)) {
        error = "the spacing is not positive";
        return false;
    }
    size_t texels = (size_t)width * 9 * height * depth;
    size_t bytes = texels * 8;
    if ((size_t)data.size() != HEADER_SIZE + bytes) {
        error = QString("%1 bytes of texels, %2 expected for %3 x %4 x %5 probes").arg(data.size() - HEADER_SIZE).arg(bytes).arg(width).arg(height).arg(depth);
        return false;
    }
    auto texture = gpu::Texture::create3D(gpu::Element::COLOR_RGBA16F, (uint16_t)width, (uint16_t)(9 * height), (uint16_t)depth, 1,
                                          gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_LINEAR, gpu::Sampler::WRAP_CLAMP));
    texture->setStoredMipFormat(gpu::Element::COLOR_RGBA16F);
    texture->assignStoredMip(0, bytes, (const gpu::Byte*)data.constData() + HEADER_SIZE);
    texture->setSource(url.toStdString());
    _probeVolumeTexture = texture;
    _probeVolumeOrigin = origin;
    _probeVolumeSpacing = spacing;
    _probeVolumeDims = glm::uvec3(width, height, depth);
    qCDebug(entitiesrenderer) << "Probe volume" << url << ":" << width << "x" << height << "x" << depth << "probes at" << spacing.x << "m, origin" << origin.x << origin.y << origin.z;
    return true;
}

void ZoneEntityRenderer::applyProbeVolume() {
    if (_probeVolumeTexture) {
        editAmbientLight()->setProbeVolume(_probeVolumeTexture, _probeVolumeOrigin, _probeVolumeSpacing, _probeVolumeDims, _probeVolumeIntensity);
    } else {
        editAmbientLight()->clearProbeVolume();
    }
}
