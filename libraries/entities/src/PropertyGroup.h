//
//  PropertyGroup.h
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 12/4/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_PropertyGroup_h
#define hifi_PropertyGroup_h

#include <QtScript/QScriptEngine>

#include <OctreeElement.h>

#include "EntityPropertyFlags.h"


class EntityItemProperties;
class EncodeBitstreamParams;
class OctreePacketData;
class EntityTreeElementExtraEncodeData;
class ReadBitstreamToTreeParams;
using EntityTreeElementExtraEncodeDataPointer = std::shared_ptr<EntityTreeElementExtraEncodeData>;


class PropertyGroup {
public:
    virtual ~PropertyGroup() = default;

    // EntityItemProperty related helpers. Each group also has, as member templates over the value and engine types
    // (QScriptValue with QScriptEngine, PlainScriptValue with PlainScriptEngine), compiled for both by
    // INSTANTIATE_PROPERTY_GROUP_CONVERSIONS at the end of its .cpp:
    //   void copyToScriptValue(const EntityPropertyFlags& desiredProperties, Value& properties, Engine* engine, bool skipDefaults,
    //                          EntityItemProperties& defaultEntityProperties) const;
    //   void copyFromScriptValue(const Value& object, bool& _defaultSettings);
    virtual void debugDump() const { }
    virtual void listChangedProperties(QList<QString>& out) { }

    virtual bool appendToEditPacket(OctreePacketData* packetData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const = 0;

    virtual bool decodeFromEditPacket(EntityPropertyFlags& propertyFlags, const unsigned char*& dataAt , int& processedBytes, int bytesToRead) = 0;
    virtual void markAllChanged() = 0;
    virtual EntityPropertyFlags getChangedProperties() const = 0;

    // EntityItem related helpers
    // methods for getting/setting all properties of an entity
    virtual void getProperties(EntityItemProperties& propertiesOut) const = 0;
    
    /// returns true if something changed
    virtual bool setProperties(const EntityItemProperties& properties) = 0;

    virtual EntityPropertyFlags getEntityProperties(EncodeBitstreamParams& params) const = 0;
        
    virtual void appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params, 
                                    EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount, 
                                    OctreeElement::AppendState& appendState) const = 0;

    virtual int readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead, 
                                                ReadBitstreamToTreeParams& args,
                                                EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                bool& somethingChanged) = 0;
};

// The two conversions of a group, compiled for QtScript's values and for plain values
#define INSTANTIATE_PROPERTY_GROUP_CONVERSIONS(T) \
    template void T::copyToScriptValue<QScriptValue, QScriptEngine>(const EntityPropertyFlags&, QScriptValue&, QScriptEngine*, \
                                                                     bool, EntityItemProperties&) const; \
    template void T::copyToScriptValue<PlainScriptValue, PlainScriptEngine>(const EntityPropertyFlags&, PlainScriptValue&, \
                                                                             PlainScriptEngine*, bool, EntityItemProperties&) const; \
    template void T::copyFromScriptValue<QScriptValue>(const QScriptValue&, bool&); \
    template void T::copyFromScriptValue<PlainScriptValue>(const PlainScriptValue&, bool&);

#endif // hifi_PropertyGroup_h
