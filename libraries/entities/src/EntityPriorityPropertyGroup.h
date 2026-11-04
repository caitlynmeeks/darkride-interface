//
//  EntityPriorityPropertyGroup.h
//  libraries/entities/src
//
//  Created by Caitlyn Meeks on 12/23/2019
//  Copyright 2019, Tivoli Cloud VR


#ifndef hifi_EntityPriorityPropertyGroup_h
#define hifi_EntityPriorityPropertyGroup_h


#include <stdint.h>
#include <glm/glm.hpp>


#include "PropertyGroup.h"
#include "EntityItemPropertiesMacros.h"

class EntityItemProperties;
class EncodeBitstreamParams;
class OctreePacketData;
class EntityTreeElementExtraEncodeData;
class ReadBitstreamToTreeParams;

class EntityPriorityPropertyGroup : public PropertyGroup {
public:
    // EntityItemProperty related helpers
    template <typename Value, typename Engine>
    void copyToScriptValue(const EntityPropertyFlags& desiredProperties,
                                   Value& properties,
                                   Engine* engine,
                                   bool skipDefaults,
                                   EntityItemProperties& defaultEntityProperties) const;
    template <typename Value>
    void copyFromScriptValue(const Value& object, bool& _defaultSettings);

    void merge(const EntityPriorityPropertyGroup& other);

    virtual void debugDump() const override;
    virtual void listChangedProperties(QList<QString>& out) override;

    virtual bool appendToEditPacket(OctreePacketData* packetData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual bool decodeFromEditPacket(EntityPropertyFlags& propertyFlags,
                                      const unsigned char*& dataAt,
                                      int& processedBytes, int bytesToRead) override;
    virtual void markAllChanged() override;
    virtual EntityPropertyFlags getChangedProperties() const override;

    // EntityItem related helpers
    // methods for getting/setting all properties of an entity
    virtual void getProperties(EntityItemProperties& propertiesOut) const override;

    /// returns true if something changed
    virtual bool setProperties(const EntityItemProperties& properties) override;

    virtual EntityPropertyFlags getEntityProperties(EncodeBitstreamParams& params) const override;

    virtual void appendSubclassData(OctreePacketData* packetData,
                                    EncodeBitstreamParams& params,
                                    EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual int readEntitySubclassDataFromBuffer(const unsigned char* data,
                                                 int bytesLeftToRead,
                                                 ReadBitstreamToTreeParams& args,
                                                 EntityPropertyFlags& propertyFlags,
                                                 bool overwriteLocalData,
                                                 bool& somethingChanged) override;

};

#endif  // hifi_EntityPriorityPropertyGroup_h