//
//  EntityEditFilters.cpp
//  libraries/entities/src
//
//  Created by David Kelly on 2/7/2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EntityEditFilters.h"

#include <QUrl>

#include <ResourceManager.h>


namespace {

EntityEditFilterEngineFactory& engineFactory() {
    static EntityEditFilterEngineFactory factory;
    return factory;
}

// the entity server sets the engine (the WebAssembly one); without it a filter can't run
EntityEditFilterEnginePointer makeEngine(const EntityItemID& entityID) {
    return engineFactory() ? engineFactory()(entityID) : EntityEditFilterEnginePointer();
}

// A property's name ("position", "grab.grabbable") names one flag; a group's name alone names every property of
// the group (as EntityItemProperties reads a script's list of names)
void addPropertyFlagsByName(const QString& name, EntityPropertyFlags& flags) {
    EntityPropertyInfo propertyInfo;
    if (EntityItemProperties::getPropertyInfo(name, propertyInfo)) {
        flags << propertyInfo.propertyEnum;
        return;
    }
    if (!name.contains('.')) {
        for (EntityPropertyList property : EntityItemProperties::getPropertiesOfGroup(name)) {
            flags << property;
        }
    }
}

// A flag's value as a list of property names: text names one, a list each of its entries
void addPropertyFlags(const QVariant& value, EntityPropertyFlags& flags) {
    if (value.type() == QVariant::String) {
        addPropertyFlagsByName(value.toString(), flags);
    } else if (value.type() == QVariant::List) {
        for (const QVariant& entry : value.toList()) {
            addPropertyFlagsByName(entry.toString(), flags);
        }
    }
}

}  // namespace

void EntityEditFilters::setEngineFactory(EntityEditFilterEngineFactory factory) {
    engineFactory() = std::move(factory);
}

QList<EntityItemID> EntityEditFilters::getZonesByPosition(glm::vec3& position) {
    QList<EntityItemID> zones;
    QList<EntityItemID> missingZones;
    _lock.lockForRead();
    auto zoneIDs = _filterDataMap.keys();
    _lock.unlock();
    for (auto id : zoneIDs) {
        if (!id.isInvalidID()) {
            // for now, look it up in the tree (soon we need to cache or similar?)
            EntityItemPointer itemPtr = _tree->findEntityByEntityItemID(id); 
            auto zone = std::dynamic_pointer_cast<ZoneEntityItem>(itemPtr);
            if (!zone) {
                // TODO: maybe remove later?
                removeFilter(id);
            } else if (zone->contains(position)) {
                zones.append(id);
            }
        } else {
            // the null id is the global filter we put in the domain server's 
            // advanced entity server settings
            zones.append(id);
        }
    }
    return zones;
}

bool EntityEditFilters::filter(glm::vec3& position, EntityItemProperties& propertiesIn, EntityItemProperties& propertiesOut,
        bool& wasChanged, EntityTree::FilterType filterType, EntityItemID& itemID, const EntityItemPointer& existingEntity) {
    
    // get the ids of all the zones (plus the global entity edit filter) that the position
    // lies within
    auto zoneIDs = getZonesByPosition(position);
    for (auto id : zoneIDs) {
        if (!itemID.isInvalidID() && id == itemID) {
            continue;
        }
        
        // get the filter pair, etc...  
        _lock.lockForRead();
        FilterData filterData = _filterDataMap.value(id);
        _lock.unlock();
    
        if (filterData.valid()) {
            if (filterData.rejectAll) {
                return false;
            }

            // check to see if this filter wants to filter this message type
            if ((!filterData.wantsToFilterEdit && filterType == EntityTree::FilterType::Edit) ||
                (!filterData.wantsToFilterPhysics && filterType == EntityTree::FilterType::Physics) ||
                (!filterData.wantsToFilterDelete && filterType == EntityTree::FilterType::Delete) ||
                (!filterData.wantsToFilterAdd && filterType == EntityTree::FilterType::Add)) {

                wasChanged = false;
                return true; // accept the message
            }

            // get the current properties for then entity and include them for the filter call
            EntityItemProperties currentProperties;
            bool hasCurrentProperties = false;
            if (existingEntity && filterData.wantsOriginalProperties) {
                currentProperties = existingEntity->getProperties(filterData.includedOriginalProperties);
                hasCurrentProperties = true;
            }

            // get the zone properties
            EntityItemProperties zoneProperties;
            bool hasZoneProperties = false;
            AABox zoneBox;
            bool hasZoneBox = false;
            if (filterData.wantsZoneProperties) {
                auto zoneEntity = _tree->findEntityByEntityItemID(id);
                if (zoneEntity) {
                    zoneProperties = zoneEntity->getProperties(filterData.includedZoneProperties);
                    hasZoneProperties = true;
                    if (filterData.wantsZoneBoundingBox) {
                        bool success = true;
                        zoneBox = zoneEntity->getAABox(success);
                        hasZoneBox = success;
                    }
                }
            }

            // the filter sees the properties the edit specifies, no more
            auto oldProperties = propertiesIn.getDesiredProperties();
            auto specifiedProperties = propertiesIn.getChangedProperties();
            propertiesIn.setDesiredProperties(specifiedProperties);
            EntityEditFilterEngine::Result result;
            {
                std::lock_guard<std::mutex> guard(*filterData.callLock);
                result = filterData.engine->call(propertiesIn, propertiesOut, filterType,
                                                 hasCurrentProperties ? &currentProperties : nullptr,
                                                 hasZoneProperties ? &zoneProperties : nullptr,
                                                 hasZoneBox ? &zoneBox : nullptr);
            }
            propertiesIn.setDesiredProperties(oldProperties);

            switch (result.kind) {
                case EntityEditFilterEngine::Result::Failed:
                case EntityEditFilterEngine::Result::Rejected:
                    // an exception, the time limit, false, or something that is neither an object nor a boolean
                    return false;
                case EntityEditFilterEngine::Result::Properties:
                    // propertiesIn and propertiesOut hold what the filter returned; whether it changed anything
                    // shows in the JSON of what it was given and what it gave back
                    wasChanged |= (result.inAsJson != result.outAsJson);
                    break;
                case EntityEditFilterEngine::Result::Accepted:
                    // true: the filter wants to pass all properties
                    propertiesOut = propertiesIn;
                    wasChanged = false;
                    break;
            }
        }
    }
    // if we made it here, 
    return true;
}

void EntityEditFilters::removeFilter(EntityItemID entityID) {
    QWriteLocker writeLock(&_lock);
    _filterDataMap.remove(entityID);
}

void EntityEditFilters::addFilter(EntityItemID entityID, QString filterURL) {

    QUrl scriptURL(filterURL);
    
    // setting it to an empty string is same as removing 
    if (filterURL.size() == 0) {
        removeFilter(entityID);
        return;
    }
   
    // The following should be abstracted out for use in Agent.cpp (and maybe later AvatarMixer.cpp)
    if (scriptURL.scheme().isEmpty() || (scriptURL.scheme() == HIFI_URL_SCHEME_FILE)) {
        qWarning() << "Cannot load script from local filesystem, because assignment may be on a different computer.";
        scriptRequestFinished(entityID);
        return;
    }
   
    // first remove any existing info for this entity
    removeFilter(entityID);
    
    // reject all edits until we load the script
    FilterData filterData;
    filterData.rejectAll = true;

    _lock.lockForWrite();
    _filterDataMap.insert(entityID, filterData);
    _lock.unlock();
   
    auto scriptRequest = DependencyManager::get<ResourceManager>()->createResourceRequest(
        this, scriptURL, true, -1, "EntityEditFilters::addFilter");
    if (!scriptRequest) {
        qWarning() << "Could not create ResourceRequest for Entity Edit filter.";
        scriptRequestFinished(entityID);
        return;
    }
    // Agent.cpp sets up a timeout here, but that is unnecessary, as ResourceRequest has its own.
    connect(scriptRequest, &ResourceRequest::finished, this, [this, entityID]{ EntityEditFilters::scriptRequestFinished(entityID);} );
    // FIXME: handle atp rquests setup here. See Agent::requestScript()
    scriptRequest->send();
    qDebug() << "script request sent for entity " << entityID;
}

void EntityEditFilters::addFilterSource(EntityItemID entityID, const QString& url, const QString& source) {
    FilterData filterData;
    filterData.engine = makeEngine(entityID);
    filterData.callLock = std::make_shared<std::mutex>();
    QString error;
    if (!filterData.engine) {
        error = "no script engine for edit filters in this process";
    }
    if (!filterData.engine || !filterData.engine->load(source, url, error)) {
        qDebug() << "Filter" << url << "not loaded:" << error << "- will reject all edits for those without lock rights.";
        filterData.engine.reset();
        filterData.rejectAll = true;
    } else {
        const QVariantMap flags = filterData.engine->flags();
        auto wants = [&](const char* name, bool fallback) {
            const QVariant value = flags.value(name);
            return value.type() == QVariant::Bool ? value.toBool() : fallback;
        };
        // if the wantsToFilterEdit is a boolean evaluate as a boolean, otherwise assume true (delete: false)
        filterData.wantsToFilterAdd = wants("wantsToFilterAdd", true);
        filterData.wantsToFilterEdit = wants("wantsToFilterEdit", true);
        filterData.wantsToFilterPhysics = wants("wantsToFilterPhysics", true);
        filterData.wantsToFilterDelete = wants("wantsToFilterDelete", false);

        // check to see if the filterFn has properties asking for Original props
        // if the wantsOriginalProperties is a boolean, or a string, or list of strings, then evaluate as follows:
        //   - boolean - true  - include all original properties
        //               false - no properties at all
        //   - string  - empty - no properties at all
        //               any valid property - include just that property in the Original properties
        //   - list of strings - include only those properties in the Original properties
        const QVariant wantsOriginalProperties = flags.value("wantsOriginalProperties");
        if (wantsOriginalProperties.type() == QVariant::Bool) {
            filterData.wantsOriginalProperties = wantsOriginalProperties.toBool();
        } else if (wantsOriginalProperties.type() == QVariant::String) {
            filterData.wantsOriginalProperties = !wantsOriginalProperties.toString().isEmpty();
            if (filterData.wantsOriginalProperties) {
                addPropertyFlags(wantsOriginalProperties, filterData.includedOriginalProperties);
            }
        } else if (wantsOriginalProperties.type() == QVariant::List) {
            addPropertyFlags(wantsOriginalProperties, filterData.includedOriginalProperties);
            filterData.wantsOriginalProperties = !filterData.includedOriginalProperties.isEmpty();
        }

        // check to see if the filterFn has properties asking for Zone props
        // if the wantsZoneProperties is a boolean, or a string, or list of strings, then evaluate as follows:
        //   - boolean - true  - include all Zone properties
        //               false - no properties at all
        //   - string  - empty - no properties at all
        //               any valid property - include just that property in the Zone properties
        //   - list of strings - include only those properties in the Zone properties
        const QVariant wantsZoneProperties = flags.value("wantsZoneProperties");
        if (wantsZoneProperties.type() == QVariant::Bool) {
            filterData.wantsZoneProperties = wantsZoneProperties.toBool();
            filterData.wantsZoneBoundingBox = filterData.wantsZoneProperties; // include this too
        } else if (wantsZoneProperties.type() == QVariant::String) {
            const QString stringValue = wantsZoneProperties.toString();
            filterData.wantsZoneProperties = !stringValue.isEmpty();
            if (filterData.wantsZoneProperties) {
                if (stringValue == "boundingBox") {
                    filterData.wantsZoneBoundingBox = true;
                } else {
                    addPropertyFlags(wantsZoneProperties, filterData.includedZoneProperties);
                }
            }
        } else if (wantsZoneProperties.type() == QVariant::List) {
            for (const QVariant& entry : wantsZoneProperties.toList()) {
                const QString stringValue = entry.toString();
                if (!stringValue.isEmpty()) {
                    filterData.wantsZoneProperties = true;

                    // boundingBox is a special case since it's not a true EntityPropertyFlag, so we
                    // need to detect it here.
                    if (stringValue == "boundingBox") {
                        filterData.wantsZoneBoundingBox = true;
                        break; // we can break here, since there are no other special cases
                    }
                }
            }
            if (filterData.wantsZoneProperties) {
                addPropertyFlags(wantsZoneProperties, filterData.includedZoneProperties);
            }
        }
    }

    _lock.lockForWrite();
    _filterDataMap.insert(entityID, filterData);
    _lock.unlock();

    if (filterData.rejectAll) {
        emit filterAdded(entityID, false);
        return;
    }
    qDebug() << "script request filter processed for entity id " << entityID;
    emit filterAdded(entityID, true);
}

void EntityEditFilters::scriptRequestFinished(EntityItemID entityID) {
    qDebug() << "script request completed for entity " << entityID;
    auto scriptRequest = qobject_cast<ResourceRequest*>(sender());
    if (scriptRequest && scriptRequest->getResult() == ResourceRequest::Success) {
        const QString urlString = scriptRequest->getUrl().toString();
        auto scriptContents = scriptRequest->getData();
        qInfo() << "Downloaded script:" << scriptContents;
        addFilterSource(entityID, urlString, scriptContents);
        return;
    } else if (scriptRequest) {
        const QString urlString = scriptRequest->getUrl().toString();
        qCritical() << "Failed to download script";
        // See HTTPResourceRequest::onRequestFinished for interpretation of codes. For example, a 404 is code 6 and 403 is 3. A timeout is 2. Go figure.
        qCritical() << "ResourceRequest error was" << scriptRequest->getResult();
    } else {
        qCritical() << "Failed to create script request.";
    }
    emit filterAdded(entityID, false);
}
