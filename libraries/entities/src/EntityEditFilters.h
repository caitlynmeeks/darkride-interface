//
//  EntityEditFilters.h
//  libraries/entities/src
//
//  Created by David Kelly on 2/7/2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_EntityEditFilters_h
#define hifi_EntityEditFilters_h

#include <QObject>
#include <QMap>
#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <mutex>

#include "EntityEditFilterEngine.h"
#include "EntityItemID.h"
#include "EntityItemProperties.h"
#include "EntityTree.h"

class EntityEditFilters : public QObject, public Dependency {
    Q_OBJECT
public:
    struct FilterData {
        std::shared_ptr<EntityEditFilterEngine> engine;
        std::shared_ptr<std::mutex> callLock;  // one call into the engine at a time
        bool wantsOriginalProperties { false };
        bool wantsZoneProperties { false };

        bool wantsToFilterAdd { true };
        bool wantsToFilterEdit { true };
        bool wantsToFilterPhysics { true };
        bool wantsToFilterDelete { true };

        EntityPropertyFlags includedOriginalProperties;
        EntityPropertyFlags includedZoneProperties;
        bool wantsZoneBoundingBox { false };

        bool rejectAll { false };

        bool valid() const { return rejectAll || engine != nullptr; }
    };

    EntityEditFilters() {};
    EntityEditFilters(EntityTreePointer tree ): _tree(tree) {};

    /// The engine a filter script runs in (the WebAssembly one; the entity server sets the factory).
    static void setEngineFactory(EntityEditFilterEngineFactory factory);

    void addFilter(EntityItemID entityID, QString filterURL);
    /// A filter from its source, as addFilter installs one once the download is in; filterAdded says whether
    /// it loaded (tests feed scripts this way).
    void addFilterSource(EntityItemID entityID, const QString& url, const QString& source);
    void removeFilter(EntityItemID entityID);

    bool filter(glm::vec3& position, EntityItemProperties& propertiesIn, EntityItemProperties& propertiesOut, bool& wasChanged, 
                EntityTree::FilterType filterType, EntityItemID& entityID, const EntityItemPointer& existingEntity);

signals:
    void filterAdded(EntityItemID id, bool success);

private slots:
    void scriptRequestFinished(EntityItemID entityID);
    
private:
    QList<EntityItemID> getZonesByPosition(glm::vec3& position);

    EntityTreePointer _tree {};
    
    QReadWriteLock _lock;
    QMap<EntityItemID, FilterData> _filterDataMap;
};

#endif //hifi_EntityEditFilters_h
