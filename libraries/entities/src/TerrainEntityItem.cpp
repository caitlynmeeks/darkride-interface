//
//  TerrainEntityItem.cpp
//  libraries/entities/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TerrainEntityItem.h"
#include "EntityContentAddresses.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include "EntityItemProperties.h"

const float TerrainEntityItem::DEFAULT_VOXEL_SIZE = 0.5f;
const float TerrainEntityItem::MIN_VOXEL_SIZE = 0.05f;
const float TerrainEntityItem::MAX_VOXEL_SIZE = 16.0f;

EntityItemPointer TerrainEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    Pointer entity(new TerrainEntityItem(entityID), [](EntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

TerrainEntityItem::TerrainEntityItem(const EntityItemID& entityItemID) : EntityItem(entityItemID) {
    _type = EntityTypes::Terrain;
}

EntityItemProperties TerrainEntityItem::getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const {
    EntityItemProperties properties = EntityItem::getProperties(desiredProperties, allowEmptyDesiredProperties); // get the properties from our base class

    COPY_ENTITY_PROPERTY_TO_PROPERTIES(voxelSize, getVoxelSize);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(terrainURL, getTerrainURL);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(terrainVersion, getTerrainVersion);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(terrainLayers, getTerrainLayers);

    return properties;
}

bool TerrainEntityItem::setSubClassProperties(const EntityItemProperties& properties) {
    bool somethingChanged = false;

    SET_ENTITY_PROPERTY_FROM_PROPERTIES(voxelSize, setVoxelSize);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(terrainURL, setTerrainURL);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(terrainVersion, setTerrainVersion);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(terrainLayers, setTerrainLayers);

    return somethingChanged;
}

int TerrainEntityItem::readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                        ReadBitstreamToTreeParams& args,
                                                        EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                        bool& somethingChanged) {
    int bytesRead = 0;
    const unsigned char* dataAt = data;

    READ_ENTITY_PROPERTY(PROP_TERRAIN_VOXEL_SIZE, float, setVoxelSize);
    READ_ENTITY_ADDRESS_PROPERTY(PROP_TERRAIN_URL, setTerrainURL,
                                 EntityContentAddresses::isOnThisComputer, "terrainURL");
    READ_ENTITY_PROPERTY(PROP_TERRAIN_VERSION, uint32_t, setTerrainVersion);
    READ_ENTITY_PROPERTY(PROP_TERRAIN_LAYERS, QString, setTerrainLayers);

    return bytesRead;
}

EntityPropertyFlags TerrainEntityItem::getEntityProperties(EncodeBitstreamParams& params) const {
    EntityPropertyFlags requestedProperties = EntityItem::getEntityProperties(params);

    requestedProperties += PROP_TERRAIN_VOXEL_SIZE;
    requestedProperties += PROP_TERRAIN_URL;
    requestedProperties += PROP_TERRAIN_VERSION;
    requestedProperties += PROP_TERRAIN_LAYERS;

    return requestedProperties;
}

void TerrainEntityItem::appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                           EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                           EntityPropertyFlags& requestedProperties,
                                           EntityPropertyFlags& propertyFlags,
                                           EntityPropertyFlags& propertiesDidntFit,
                                           int& propertyCount,
                                           OctreeElement::AppendState& appendState) const {
    bool successPropertyFits = true;

    APPEND_ENTITY_PROPERTY(PROP_TERRAIN_VOXEL_SIZE, getVoxelSize());
    APPEND_ENTITY_PROPERTY(PROP_TERRAIN_URL, getTerrainURL());
    APPEND_ENTITY_PROPERTY(PROP_TERRAIN_VERSION, getTerrainVersion());
    APPEND_ENTITY_PROPERTY(PROP_TERRAIN_LAYERS, getTerrainLayers());
}

void TerrainEntityItem::setVoxelSize(float voxelSize) {
    voxelSize = glm::clamp(voxelSize, MIN_VOXEL_SIZE, MAX_VOXEL_SIZE);
    withWriteLock([&] {
        _needsRenderUpdate |= _voxelSize != voxelSize;
        _voxelSize = voxelSize;
    });
}

float TerrainEntityItem::getVoxelSize() const {
    return resultWithReadLock<float>([&] {
        return _voxelSize;
    });
}

void TerrainEntityItem::setTerrainURL(const QString& url) {
    withWriteLock([&] {
        _needsRenderUpdate |= _terrainURL != url;
        _terrainURL = url;
    });
}

QString TerrainEntityItem::getTerrainURL() const {
    return resultWithReadLock<QString>([&] {
        return _terrainURL;
    });
}

void TerrainEntityItem::setTerrainVersion(uint32_t version) {
    withWriteLock([&] {
        _needsRenderUpdate |= _terrainVersion != version;
        _terrainVersion = version;
    });
}

uint32_t TerrainEntityItem::getTerrainVersion() const {
    return resultWithReadLock<uint32_t>([&] {
        return _terrainVersion;
    });
}

void TerrainEntityItem::setTerrainLayers(const QString& layers) {
    withWriteLock([&] {
        _needsRenderUpdate |= _terrainLayers != layers;
        _terrainLayers = layers;
    });
}

QString TerrainEntityItem::getTerrainLayers() const {
    return resultWithReadLock<QString>([&] {
        return _terrainLayers;
    });
}

std::vector<TerrainLayerSpec> TerrainEntityItem::parseTerrainLayers(const QString& json, QString* error) {
    std::vector<TerrainLayerSpec> layers;
    if (json.trimmed().isEmpty()) {
        return layers;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = parseError.errorString();
        }
        return layers;
    }
    if (!document.isArray()) {
        if (error) {
            *error = "not a list of layers";
        }
        return layers;
    }
    for (const QJsonValue& value : document.array()) {
        if ((int)layers.size() >= MAX_TERRAIN_LAYERS) {
            break;
        }
        TerrainLayerSpec layer;
        if (value.isObject()) {
            const QJsonObject object = value.toObject();
            layer.albedoURL = object.value("albedo").toString();
            const double tiling = object.value("tiling").toDouble(layer.tiling);
            if (tiling > 0.0) {
                layer.tiling = (float)tiling;
            }
        } else if (value.isString()) {
            layer.albedoURL = value.toString();   // a bare URL: the default tiling
        }
        layers.push_back(layer);
    }
    return layers;
}

glm::ivec3 TerrainEntityItem::getVoxelCount() const {
    const glm::vec3 dimensions = getScaledDimensions();
    const float voxelSize = getVoxelSize();
    return glm::max(glm::ivec3(glm::round(dimensions / voxelSize)), glm::ivec3(1));
}
