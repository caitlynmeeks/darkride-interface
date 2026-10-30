//
//  ToneMappingPropertyGroup.h
//  libraries/entities/src
//
//  Created by Caitlyn Meeks on 8/28/2020
//  Copyright 2020 Tivoli Cloud VR, Inc.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT

#ifndef hifi_ToneMappingPropertyGroup_h
#define hifi_ToneMappingPropertyGroup_h

#include <stdint.h>
#include <glm/glm.hpp>

#include <QtScript/QScriptEngine>

#include "PropertyGroup.h"
#include "EntityItemPropertiesMacros.h"

class EntityItemProperties;
class EncodeBitstreamParams;
class OctreePacketData;
class EntityTreeElementExtraEncodeData;
class ReadBitstreamToTreeParams;

static const float INITIAL_EXPOSURE { 0.0f };

/**jsdoc
 * Tone Mapping is defined by the following properties:
 * @typedef {object} Entities.ToneMapping
 * @property {number} exposure=0.0 - The amount of exposure used in tone mappinpg.
 */
class ToneMappingPropertyGroup : public PropertyGroup {
public:
    // EntityItemProperty related helpers
    template <typename Value, typename Engine>
    void copyToScriptValue(const EntityPropertyFlags& desiredProperties, Value& properties,
                                   Engine* engine, bool skipDefaults,
                                   EntityItemProperties& defaultEntityProperties) const;
    template <typename Value>
    void copyFromScriptValue(const Value& object, bool& _defaultSettings);

    void merge(const ToneMappingPropertyGroup& other);

    virtual void debugDump() const override;
    virtual void listChangedProperties(QList<QString>& out) override;

    virtual bool appendToEditPacket(OctreePacketData* packetData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual bool decodeFromEditPacket(EntityPropertyFlags& propertyFlags,
                                      const unsigned char*& dataAt, int& processedBytes, int bytesToRead) override;
    virtual void markAllChanged() override;
    virtual EntityPropertyFlags getChangedProperties() const override;

    // EntityItem related helpers
    // methods for getting/setting all properties of an entity
    virtual void getProperties(EntityItemProperties& propertiesOut) const override;

    /// returns true if something changed
    virtual bool setProperties(const EntityItemProperties& properties) override;

    virtual EntityPropertyFlags getEntityProperties(EncodeBitstreamParams& params) const override;

    virtual void appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                    EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual int readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                ReadBitstreamToTreeParams& args,
                                                EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                bool& somethingChanged) override;

    DEFINE_PROPERTY(PROP_TONE_MAPPING_EXPOSURE, Exposure, exposure, float, INITIAL_EXPOSURE);
    

};

#endif // hifi_ToneMappingPropertyGroup_h
