//
//  EntityEditFilterEngine.h
//  libraries/entities/src
//
//  What EntityEditFilters asks of a script engine for one filter script: evaluate the script, read its filter
//  function's flags, and call the function for an edit. One engine per filter, used on the thread that calls it
//  (the entity server's packet thread), synchronously, as the filters have always run. The implementation,
//  WasmEditFilterEngine, is in the script-engine library, which depends on this one, so the entity server installs
//  it through EntityEditFilters::setEngineFactory.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_EntityEditFilterEngine_h
#define hifi_EntityEditFilterEngine_h

#include <functional>
#include <memory>

#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <AABox.h>

#include "EntityItemID.h"
#include "EntityItemProperties.h"

class EntityEditFilterEngine {
public:
    virtual ~EntityEditFilterEngine() = default;

    /// Evaluates the script under its URL. False, with the reason, when it has a syntax error, throws, or
    /// defines no filter function.
    virtual bool load(const QString& source, const QString& url, QString& error) = 0;

    /// The filter function's properties as the script set them, those that are set: wantsToFilterAdd,
    /// wantsToFilterEdit, wantsToFilterPhysics, wantsToFilterDelete, wantsOriginalProperties and
    /// wantsZoneProperties, each a boolean, text, or a list of text.
    virtual QVariantMap flags() = 0;

    struct Result {
        enum Kind {
            Failed,      // an exception, or the call's time limit: the edit is refused
            Properties,  // an object: the properties to apply, read into `in` and `out`
            Accepted,    // true
            Rejected     // false, or anything else
        };
        Kind kind { Failed };
        QJsonValue inAsJson;   // what the filter was given, taken before the call (it may change its argument)
        QJsonValue outAsJson;  // for Properties: the object it returned
        QString error;         // for Failed
    };
    /// filter(properties, type, originalProperties, zoneProperties): `in` is the edit (its changed properties
    /// alone, as the caller narrowed them); `original` the entity's current properties when the filter asked
    /// for them, else null; `zone` the zone's properties when asked for, with its bounding box when asked
    /// for that, else null. An object the filter returns is read into `in` (for the next filter) and `out`.
    virtual Result call(EntityItemProperties& in, EntityItemProperties& out, int filterType,
                        const EntityItemProperties* original, const EntityItemProperties* zone, const AABox* zoneBox) = 0;
};

using EntityEditFilterEnginePointer = std::unique_ptr<EntityEditFilterEngine>;
using EntityEditFilterEngineFactory = std::function<EntityEditFilterEnginePointer(const EntityItemID& entityID)>;

#endif // hifi_EntityEditFilterEngine_h
